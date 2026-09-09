#Requires -Version 7.0
<#
.SYNOPSIS
    Builds the golden ExoSnap release-verification guest. Runs on the HOST, elevated.

.DESCRIPTION
    One machine, built once, from a recipe that is in the repository rather than in
    somebody's memory. Everything a campaign runs against afterwards is a differencing
    disk taken from the image this produces, so what this script installs is the
    definition of "a clean machine" for every Tier 2 gate.

    Five phases, selectable with -Phase, because three of them cannot run while the
    machine is in the state the previous one leaves it in:

      create     the machine, its disk, its firmware, the installation DVD and the
                 answer DVD
      install    boot once; autounattend.xml installs Windows and logs the console
                 session on
      gpu        the GPU partition, which Hyper-V only attaches to a stopped machine
      driver     the host adapter's own driver files, staged with Copy-VMFile
      provision  provision.ps1, over PowerShell Direct

    -DryRun (or -WhatIf) prints the exact command plan and touches nothing. It is the
    same plan the real run executes, not a description of it.

.PARAMETER IsoPath
    A Windows 11 x64 installation ISO. Required for the create phase.

.PARAMETER Root
    Where the golden image lives. Defaults to the image root named by
    Get-ReleaseVmDefault.

.PARAMETER Phase
    Which phases to run. All five by default.

.PARAMETER HostDriverPackage
    The host display driver package to stage into the guest. Discovered from the
    host's DriverStore when not given.

.PARAMETER ProvisionSwitchName
    The Hyper-V switch used only while the guest downloads pinned provisioning
    packages. Defaults to the Windows client Default Switch.

.PARAMETER DryRun
    Print the plan and exit.

.EXAMPLE
    pwsh -NoProfile -File tools/vm/New-ReleaseVm.ps1 -IsoPath <Win11 x64 ISO> -DryRun

.EXAMPLE
    Start-Process -FilePath pwsh -Verb RunAs -ArgumentList '-NoProfile','-File','tools/vm/New-ReleaseVm.ps1','-IsoPath','<Win11 x64 ISO>'
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $IsoPath,
    [string] $Root,
    [string] $VMName,
    # No ValidateSet: `pwsh -File script.ps1 -Phase gpu,driver` does not parse
    # PowerShell array syntax, so the whole thing arrives as one string and a set
    # attribute refuses it before anything can split it. Validated below instead.
    [string[]] $Phase = @('create', 'install', 'gpu', 'driver', 'provision'),
    [string] $HostDriverPackage,
    [string] $ProvisionSwitchName = 'Default Switch',
    [long] $MemoryBytes = 0,
    [int] $ProcessorCount = 0,
    [long] $DiskSizeBytes = 0,
    [int] $InstallTimeoutMinutes = 90,
    [switch] $DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$knownPhases = @('create', 'install', 'gpu', 'driver', 'provision')
$Phase = @($Phase | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$unknownPhase = @($Phase | Where-Object { $_ -notin $knownPhases })
if ($unknownPhase.Count -gt 0) {
    throw "unknown phase(s) '$($unknownPhase -join ', ')'; the phases are $($knownPhases -join ', ')"
}
if ($Phase.Count -eq 0) { throw "no phase was named; the phases are $($knownPhases -join ', ')" }

Import-Module (Join-Path $PSScriptRoot 'ReleaseVm.psm1') -Force -DisableNameChecking

$defaults = Get-ReleaseVmDefault
if (-not $VMName) { $VMName = $defaults.VMName }
if ($MemoryBytes -le 0) { $MemoryBytes = $defaults.MemoryBytes }
if ($ProcessorCount -le 0) { $ProcessorCount = $defaults.ProcessorCount }
if ($DiskSizeBytes -le 0) { $DiskSizeBytes = $defaults.DiskSizeBytes }

$paths = Get-ReleaseVmPath -Root $Root
$plan = @()
$planning = $DryRun -or $WhatIfPreference

# Discovered rather than required, but never guessed at execution time: a driver
# package that could not be found has to be visible in the plan, not surface as an
# error twenty minutes into an unattended build.
if (-not $HostDriverPackage) {
    $HostDriverPackage = Get-ReleaseVmHostDriverPackage
}

$hostState = Get-ReleaseVmHostState
$prerequisiteArguments = @{
    ScriptPath             = $PSCommandPath
    ScriptArgument         = @('-IsoPath', $(if ($IsoPath) { $IsoPath } else { '<path to Win11 x64 ISO>' }))
    IsElevated             = $hostState.IsElevated
    InHyperVAdministrators = $hostState.InHyperVAdministrators
    HyperVModuleAvailable  = $hostState.HyperVModuleAvailable
    UserName               = $hostState.UserName
}
if ($Phase -contains 'create') { $prerequisiteArguments['IsoPath'] = $IsoPath }
$verdict = Test-ReleaseVmPrerequisite @prerequisiteArguments

Write-Host ''
Write-Host "  golden image : $($paths.GoldenDisk)"
Write-Host "  machine      : $VMName ($ProcessorCount vCPU, $($MemoryBytes / 1GB) GB, $($DiskSizeBytes / 1GB) GB dynamic disk)"
Write-Host "  phases       : $($Phase -join ', ')"
if ($HostDriverPackage) {
    Write-Host "  host driver  : $HostDriverPackage"
}
else {
    Write-Host "  host driver  : NOT FOUND under $($defaults.HostDriverRepository) matching $($defaults.HostDriverPattern)"
}

Write-ReleaseVmPrerequisite -Verdict $verdict

$plan = New-ReleaseVmCreatePlan `
    -VMName $VMName `
    -Root $paths.Root `
    -GoldenDisk $paths.GoldenDisk `
    -AnswerIso $paths.AnswerIso `
    -AnswerFile (Join-Path $PSScriptRoot 'autounattend.xml') `
    -IsoPath ([string]$IsoPath) `
    -ProvisionScript (Join-Path $PSScriptRoot 'provision.ps1') `
    -ProvisionManifest (Join-Path $PSScriptRoot 'provision-manifest.psd1') `
    -HostDriverPackage $HostDriverPackage `
    -ProvisionSwitchName $ProvisionSwitchName `
    -MemoryBytes $MemoryBytes `
    -ProcessorCount $ProcessorCount `
    -DiskSizeBytes $DiskSizeBytes `
    -Phase $Phase `
    -InstallTimeoutMinutes $InstallTimeoutMinutes

if ($planning) {
    Write-ReleaseVmPlan -Plan $plan -Title 'New-ReleaseVm plan (nothing was executed)'
    exit 0
}

if (-not $verdict.Ok) {
    Write-Host '  refusing to run: fix the preconditions above first.'
    exit 2
}

Write-Host ''
Invoke-ReleaseVmPlan -Plan $plan -Credential (New-ReleaseVmCredential) | Out-Null
Write-Host ''
Write-Host "  the golden image is built. Verify it in the guest, then freeze it:"
Write-Host "    the two probes (probe_gpup_nvenc, probe_idd_duplication) must both succeed inside the guest"
Write-Host "    then keep $($paths.GoldenDisk) read-only; every run takes a differencing disk from it"
exit 0
