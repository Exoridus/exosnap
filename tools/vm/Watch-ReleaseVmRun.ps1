#Requires -Version 7.0
<#
.SYNOPSIS
    Follows a campaign running in a release-verification guest, read-only and bounded.
    Runs on the HOST.

.DESCRIPTION
    Prints the guest campaign's log as it grows and returns once the guest writes its
    exit file. The watch never waits without a limit: each read over PowerShell Direct
    has its own timeout, repeated failed reads end it, and -TimeoutMinutes ends it
    regardless. It never prompts. A missing argument is an error, not a question, so
    it is safe to run unattended or from a background monitor.

    Exit codes: 0 the exit file appeared (its content is printed, not propagated),
    2 the deadline passed, 3 the guest stopped answering, 1 anything else.

.PARAMETER VMName
    The running guest to read from.

.PARAMETER ExitFile
    Guest path the campaign writes when it is done.

.PARAMETER LogFile
    Guest path of a UTF-8 log to follow. Optional.

.EXAMPLE
    pwsh -NoProfile -NonInteractive -File tools/vm/Watch-ReleaseVmRun.ps1 -VMName ExoSnap-Run-smoke-001 `
        -ExitFile $guestExitFile -LogFile $guestLogFile -TimeoutMinutes 60
#>
[CmdletBinding()]
param(
    # Not Mandatory: PowerShell asks for a missing mandatory argument, and a question
    # nobody sees stalls an unattended watch.
    [string] $VMName,
    [string] $ExitFile,
    [string] $LogFile,
    [ValidateRange(1, 1440)] [int] $TimeoutMinutes = 90,
    [ValidateRange(1, 600)] [int] $PollSeconds = 15,
    [ValidateRange(1, 100)] [int] $MaxConsecutiveFailures = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$code = 1
try {
    if (-not $VMName) { throw '-VMName is required' }
    if (-not $ExitFile) { throw '-ExitFile is required' }

    Import-Module (Join-Path $PSScriptRoot 'ReleaseVm.psm1') -Force -DisableNameChecking
    $credential = New-ReleaseVmCredential
    $read = {
        param($Offset)
        Read-ReleaseVmGuestProgress -VMName $VMName -Credential $credential -ExitFile $ExitFile `
            -LogFile $LogFile -Offset $Offset
    }.GetNewClosure()
    $print = { param($Text) [Console]::Out.Write($Text); [Console]::Out.Flush() }

    $result = Watch-ReleaseVmGuestRun -Read $read -OnText $print -TimeoutMinutes $TimeoutMinutes `
        -PollSeconds $PollSeconds -MaxConsecutiveFailures $MaxConsecutiveFailures
    switch ($result.Outcome) {
        'finished' { [Console]::Out.WriteLine("campaign finished; exit file: $($result.ExitText)"); $code = 0 }
        'deadline' { [Console]::Out.WriteLine("watch ended: $($result.Detail)"); $code = 2 }
        'unreachable' { [Console]::Out.WriteLine("watch ended, '$VMName' unreachable: $($result.Detail)"); $code = 3 }
    }
}
catch {
    [Console]::Error.WriteLine("watch failed: $($_.Exception.Message)")
    $code = 1
}
[Console]::Out.Flush()
# A normal exit disposes the runspaces of abandoned PowerShell Direct reads, and that
# blocks for as long as the guest does not answer.
[Environment]::Exit($code)
