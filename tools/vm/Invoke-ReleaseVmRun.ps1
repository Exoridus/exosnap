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
    The complete command line the guest runs. Required because campaign setup and
    the executable under test have no safe implicit path. The command must write
    evidence beneath the guest results directory and return its campaign exit code.

.PARAMETER Network
    Connected, Disconnected (default) or HostOnly.

.PARAMETER RequireInteractiveGuest
    The campaign captures the guest desktop, so the run proves before it starts that
    the guest has an interactive session on the console with a display attached --
    rather than inferring it from PowerShell Direct answering, which proves only that
    the OS is up. Declared per campaign: a scenario that installs and uninstalls needs
    none of it, and refusing such a run for a missing desktop would demand more than
    the scenario does.

    Note what it does not do: the campaign is launched over PowerShell Direct, which
    is session 0, so a run that declares this today is told so instead of producing a
    picture nobody can explain. Launching the campaign into the interactive session
    is the transport's job, not this switch's.

.PARAMETER ProveGpuBinding
    The run measures the host adapter it is about to partition and, once the guest
    is up, holds the guest to it: the adapter the guest is on presents that host
    GPU, and the driver package staged into the guest is the package the host is
    running. Also asks for the virtual monitor mode the provisioning manifest pins,
    on one attached display path. Implies -RequireInteractiveGuest, because the
    receipt this reads is the interactive agent's.

    A campaign whose evidence has to name the GPU it ran on declares this; an
    install-and-uninstall run does not need it and is not refused for it.

.PARAMETER KeepDisk
    Leave the differencing disk in place. For diagnosing a run that failed inside the
    guest; it is not a normal mode, and the disk has to be deleted by hand afterwards.

.EXAMPLE
    pwsh -NoProfile -File tools/vm/Invoke-ReleaseVmRun.ps1 -RunId smoke-001 -DryRun

.EXAMPLE
    ./tools/vm/Invoke-ReleaseVmRun.ps1 -ArtifactDirectory $artifacts -HarnessDirectory $harness -GuestCommand $campaign
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
    [switch] $RequireInteractiveGuest,
    [switch] $ProveGpuBinding,
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
$planning = $DryRun -or $WhatIfPreference
$guestCommandMissing = [string]::IsNullOrWhiteSpace($GuestCommand)
if ($guestCommandMissing) {
    $GuestCommand = '<required: pass -GuestCommand that writes evidence to C:\ExoSnapRun\out>'
}

$hostState = Get-ReleaseVmHostState
$verdict = Test-ReleaseVmPrerequisite `
    -ScriptPath $PSCommandPath `
    -ScriptArgument @('-RunId', $RunId) `
    -IsElevated $hostState.IsElevated `
    -InHyperVAdministrators $hostState.InHyperVAdministrators `
    -HyperVModuleAvailable $hostState.HyperVModuleAvailable `
    -UserName $hostState.UserName
if ($guestCommandMissing) {
    $verdict.Ok = $false
    $verdict.Problems += @{
        Id = 'guest-command-missing'
        Message = 'no guest command was supplied; campaign setup and the executable under test must be explicit'
        Remedy = "pass -GuestCommand 'C:\ExoSnapRun\harness\campaign.cmd' with your campaign wrapper in HarnessDirectory; write evidence to C:\ExoSnapRun\out"
    }
}

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

$readiness = $null
if ($ProveGpuBinding) {
    # Measured now, before anything is partitioned, so the identity the guest is held
    # to is the adapter as it was when this run began. An adapter that cannot be
    # measured binds nothing, and a run that would produce evidence attributed to no
    # GPU is refused here rather than after an hour.
    $hostGpu = Get-ReleaseVmHostGpu
    if (-not $hostGpu.Measured) {
        $verdict.Ok = $false
        $verdict.Problems += @{
            Id = 'host-gpu-unmeasured'
            Message = "the host GPU could not be measured: $($hostGpu.Detail)"
            Remedy = 'a run that proves its GPU binding needs a host adapter matching HostGpuInstancePattern'
        }
    }
    $manifest = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'provision-manifest.psd1')
    $mode = @{
        Width     = [int] $manifest.display.width
        Height    = [int] $manifest.display.height
        # The first pinned rate is the mode the virtual monitor comes up in; a gate
        # that needs another one switches to it and says so.
        RefreshHz = [int] @($manifest.display.refreshRates)[0]
    }
    $readiness = New-ReleaseVmReadinessRequirement -InteractiveAgent -ExpectedUser $defaults.GuestUserName `
        -Display $mode -GpuBoundTo $hostGpu
    Write-Host "  gpu binding  : $(Format-ReleaseVmGpuBinding -HostGpu $hostGpu -Display $mode)"
}

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
    -RequireInteractiveGuest:($RequireInteractiveGuest -or $ProveGpuBinding) `
    -Readiness $readiness `
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

if ($ProveGpuBinding -and $outputs.ContainsKey('guest-readiness') -and $null -ne $outputs['guest-readiness']) {
    # The binding this run was measured under, beside its evidence: the host adapter
    # and package, the guest's receipt, and whether the guest runs the display profile
    # the scenario was qualified on. The profile verdict is three-valued on purpose --
    # an image that cannot run the qualified profile makes a scenario unrunnable, not
    # failing, and a record that could only say pass or fail would say the wrong one.
    $receipt = $outputs['guest-readiness']
    $profile = Test-ReleaseVmDisplayProfile -Required "$($manifest.qualifiedDisplayProfile)" `
        -Measured $(if ($receipt.Contains('displayDriver')) { $receipt['displayDriver'] } else { @{} })
    $binding = [ordered]@{
        runId              = $RunId
        measuredUtc        = [DateTime]::UtcNow.ToString('o')
        hostGpu            = $hostGpu
        requirement        = $readiness
        guestReceipt       = $receipt
        displayProfile     = [ordered]@{
            built     = "$($manifest.displayProfile)"
            qualified = "$($manifest.qualifiedDisplayProfile)"
            verdict   = $profile.Verdict
            detail    = $profile.Detail
        }
    }
    New-Item -ItemType Directory -Path $resultDirectory -Force | Out-Null
    $bindingPath = Join-Path $resultDirectory 'campaign-binding.json'
    $binding | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $bindingPath -Encoding utf8NoBOM
    Write-Host "  binding      : $bindingPath ($($profile.Verdict): $($profile.Detail))"
}

Write-Host ''
Write-Host "  the guest command exited $exitCode; its evidence is in $resultDirectory"
exit $exitCode
