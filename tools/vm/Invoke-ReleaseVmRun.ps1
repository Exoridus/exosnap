#Requires -Version 7.0
<#
.SYNOPSIS
    Runs one release-verification campaign in a throwaway clone of the golden guest.
    Runs on the HOST, elevated.

.DESCRIPTION
    Every run gets a differencing disk taken from the golden image, and the golden
    image is never written to. That is what makes two runs comparable: the machine
    the second one starts on is the same machine the first one started on, down to
    the bytes, including the parts a previous campaign would otherwise have changed --
    an installed product, a recovery manifest, a running updater holding its own
    executable.

    The disk, the virtual machine and everything the guest wrote are gone when this
    returns. What survives is the evidence, copied into the repository's private
    working directory under release-verify/<runId> before the machine is turned off.

    Network is off by default. A gate that installs or updates and can also reach the
    internet is not measuring the artifact it was handed. -Network HostOnly and
    -Network Connected exist for the gates whose subject is the network itself.

    -DryRun (or -WhatIf) prints the exact command plan and touches nothing.

.PARAMETER RunId
    Names the run directory, the differencing disk and the virtual machine. Defaults
    to a timestamp.

.PARAMETER ArtifactDirectory
    The release artifacts under test, copied into the guest.

.PARAMETER HarnessDirectory
    The published verification harness, copied into the guest.

.PARAMETER GuestCommand
    The command line the guest runs. A string on purpose: it names a program built
    elsewhere in the tree, and this script must be usable before that program exists.

.PARAMETER Network
    Connected, Disconnected (default) or HostOnly.

.PARAMETER KeepDisk
    Leave the differencing disk in place. For diagnosing a run that failed inside the
    guest; it is not a normal mode, and the disk has to be deleted by hand afterwards.

.EXAMPLE
    pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 -RunId smoke-001 -DryRun

.EXAMPLE
    Start-Process -FilePath pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','tools/vm/Invoke-ReleaseVmRun.ps1','-ArtifactDirectory','<rc artifacts>'
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $RunId,
    [string] $Root,
    [string] $ArtifactDirectory,
    [string] $HarnessDirectory,
    [string] $GuestCommand,
    [string] $ResultRoot,
    [ValidateSet('Connected', 'Disconnected', 'HostOnly')] [string] $Network = 'Disconnected',
    [long] $MemoryBytes = 0,
    [int] $ProcessorCount = 0,
    [int] $BootTimeoutMinutes = 15,
    [int] $RunTimeoutMinutes = 120,
    [switch] $KeepDisk,
    [switch] $DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'ReleaseVm.psm1') -Force -DisableNameChecking

$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$defaults = Get-ReleaseVmDefault
if ($MemoryBytes -le 0) { $MemoryBytes = $defaults.MemoryBytes }
if ($ProcessorCount -le 0) { $ProcessorCount = $defaults.ProcessorCount }
# A timestamp rather than a GUID: the run id appears in the evidence directory, in the
# machine name and in the qualification record, and the first question asked of any of
# those is when it ran.
if (-not $RunId) { $RunId = 'vm-' + (Get-Date).ToString('yyyyMMdd-HHmmss') }
if (-not $ResultRoot) { $ResultRoot = Join-Path $repositoryRoot '.workspace/release-verify' }

$runPath = Get-ReleaseVmRunPath -RunId $RunId -Root $Root
$resultDirectory = [IO.Path]::Combine($ResultRoot, $RunId)
if (-not $GuestCommand) {
    $GuestCommand = "$([IO.Path]::Combine($runPath.GuestHarness, 'ExoSnap.Verify.exe')) qualify --run-id $RunId --output $($runPath.GuestResults)"
}

$planning = $DryRun -or $WhatIfPreference

$hostState = Get-ReleaseVmHostState
$verdict = Test-ReleaseVmPrerequisite `
    -ScriptPath $PSCommandPath `
    -ScriptArgument @('-RunId', $RunId) `
    -IsElevated $hostState.IsElevated `
    -InHyperVAdministrators $hostState.InHyperVAdministrators `
    -HyperVModuleAvailable $hostState.HyperVModuleAvailable `
    -UserName $hostState.UserName

Write-Host ''
Write-Host "  run          : $RunId"
Write-Host "  golden image : $($runPath.GoldenDisk)"
Write-Host "  run disk     : $($runPath.DifferencingDisk)"
Write-Host "  machine      : $($runPath.VMName) ($ProcessorCount vCPU, $($MemoryBytes / 1GB) GB)"
Write-Host "  network      : $Network"
Write-Host "  evidence     : $resultDirectory"

if (-not $planning) {
    if (-not (Test-Path -LiteralPath $runPath.GoldenDisk)) {
        Write-Host ''
        Write-Host "  there is no golden image at $($runPath.GoldenDisk)."
        Write-Host '  Build it once, elevated:'
        Write-Host "      pwsh -NoProfile -File $(Join-Path $PSScriptRoot 'New-ReleaseVm.ps1') -IsoPath <Win11 x64 ISO>"
        exit 2
    }
}

Write-ReleaseVmPrerequisite -Verdict $verdict

$plan = New-ReleaseVmRunPlan `
    -RunPath $runPath `
    -GuestCommand $GuestCommand `
    -ResultDirectory $resultDirectory `
    -ArtifactDirectory $ArtifactDirectory `
    -HarnessDirectory $HarnessDirectory `
    -Network $Network `
    -MemoryBytes $MemoryBytes `
    -ProcessorCount $ProcessorCount `
    -BootTimeoutMinutes $BootTimeoutMinutes `
    -RunTimeoutMinutes $RunTimeoutMinutes `
    -KeepDisk:$KeepDisk

if ($planning) {
    Write-ReleaseVmPlan -Plan $plan -Title "Invoke-ReleaseVmRun plan for $RunId (nothing was executed)"
    exit 0
}

if (-not $verdict.Ok) {
    Write-Host '  refusing to run: fix the preconditions above first.'
    exit 2
}

Write-Host ''
$outputs = Invoke-ReleaseVmPlan -Plan $plan -Credential (New-ReleaseVmCredential)

$exitCode = 0
if ($outputs.ContainsKey('run') -and $null -ne $outputs['run']) { $exitCode = [int]$outputs['run'] }

Write-Host ''
Write-Host "  the guest command exited $exitCode; its evidence is in $resultDirectory"
exit $exitCode
