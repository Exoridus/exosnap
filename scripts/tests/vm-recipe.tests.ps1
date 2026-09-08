#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the Hyper-V release-verification guest recipe under tools/vm.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use, so CTest and
    `verify.ps1` run all of them the same way and a contributor reads one style.

    What is testable about a virtual machine recipe on a machine that may not have
    Hyper-V at all is exactly what the recipe was written to make testable. The two
    host scripts build a plan before they run one, and the plan is data, so every case
    below reads a plan or a refusal and none of them touches Hyper-V, the network, a
    disk image, or the machine's configuration.

    The refusals matter as much as the plans. "Not elevated", "not in Hyper-V
    Administrators" and "no installation ISO" are the three ways the first attempt
    fails on a machine that has never built the image, and each of them has to name
    the exact command that fixes it rather than the fact that something is wrong.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $scriptRoot
$script:VmRoot = Join-Path $repositoryRoot 'tools/vm'
Import-Module (Join-Path $script:VmRoot 'ReleaseVm.psm1') -Force -DisableNameChecking

$script:NewVmScript = Join-Path $script:VmRoot 'New-ReleaseVm.ps1'
$script:RunScript = Join-Path $script:VmRoot 'Invoke-ReleaseVmRun.ps1'
$script:ProvisionScript = Join-Path $script:VmRoot 'provision.ps1'
$script:ManifestPath = Join-Path $script:VmRoot 'provision-manifest.psd1'
$script:AnswerFile = Join-Path $script:VmRoot 'autounattend.xml'
$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }
function Assert-Equal {
    param($Expected, $Actual, [string] $Message)
    if ("$Expected" -ne "$Actual") { throw "$Message (expected '$Expected', got '$Actual')" }
}
function Assert-Match {
    param([string] $Pattern, [string] $Text, [string] $Message)
    if ($Text -notmatch $Pattern) { throw "$Message (no match for '$Pattern')" }
}
function Assert-NoMatch {
    param([string] $Pattern, [string] $Text, [string] $Message)
    if ($Text -match $Pattern) { throw "$Message (unexpected match for '$Pattern')" }
}

function New-TestDirectory {
    $path = Join-Path ([IO.Path]::GetTempPath()) "vm-recipe-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

function Invoke-Script {
    <#
    .SYNOPSIS
        Runs one of the host scripts in a child pwsh and returns its output and code.
    #>
    param([Parameter(Mandatory)] [string] $Path, [string[]] $Arguments = @())
    $output = & pwsh -NoProfile -NonInteractive -File $Path @Arguments 2>&1 | Out-String
    return @{ Output = $output; ExitCode = $LASTEXITCODE }
}

function New-Prerequisite {
    param(
        [bool] $IsElevated = $true,
        [bool] $InHyperVAdministrators = $true,
        [bool] $HyperVModuleAvailable = $true,
        [string] $IsoPath
    )
    $arguments = @{
        ScriptPath             = 'C:\repo\tools\vm\New-ReleaseVm.ps1'
        ScriptArgument         = @('-IsoPath', 'W:\win11.iso')
        IsElevated             = $IsElevated
        InHyperVAdministrators = $InHyperVAdministrators
        HyperVModuleAvailable  = $HyperVModuleAvailable
        UserName               = 'TESTHOST\tester'
    }
    if ($PSBoundParameters.ContainsKey('IsoPath')) { $arguments['IsoPath'] = $IsoPath }
    return Test-ReleaseVmPrerequisite @arguments
}

function Get-ProblemId { param($Verdict) return @($Verdict.Problems | ForEach-Object { $_.Id }) }

# ---------------------------------------------------------------------------
# Preconditions: the three documented refusals
# ---------------------------------------------------------------------------

Test-Case 'a shell that is not elevated is refused, with the elevated command to run' {
    $verdict = New-Prerequisite -IsElevated $false
    Assert-True (-not $verdict.Ok) 'an unelevated shell must not be allowed to run a plan'
    Assert-True ((Get-ProblemId -Verdict $verdict) -contains 'not-elevated') 'the refusal must be identified'
    $remedy = @($verdict.Problems | Where-Object { $_.Id -eq 'not-elevated' })[0].Remedy
    Assert-Match 'Start-Process' $remedy 'the remedy must be a command, not a description'
    Assert-Match '-Verb RunAs' $remedy 'the remedy must be the elevated form'
    Assert-Match 'New-ReleaseVm\.ps1' $remedy 'the remedy must name the script being run'
    Assert-Match 'W:\\win11\.iso' $remedy 'the remedy must carry the arguments the caller used'
}

Test-Case 'an account outside Hyper-V Administrators is refused, by name' {
    $verdict = New-Prerequisite -InHyperVAdministrators $false
    Assert-True (-not $verdict.Ok) 'a plan must not run for an account whose cmdlets will be denied'
    $problem = @($verdict.Problems | Where-Object { $_.Id -eq 'not-in-hyperv-administrators' })[0]
    Assert-Match 'TESTHOST\\tester' $problem.Message 'the message must name the account'
    Assert-Match 'Add-LocalGroupMember' $problem.Remedy 'the remedy must be the command that adds it'
    Assert-Match 'sign out' $problem.Remedy 'group membership only takes effect on a new token'
}

Test-Case 'a missing installation ISO is refused, and an absent one is the same refusal' {
    $missing = New-Prerequisite -IsoPath 'W:\does-not-exist.iso'
    Assert-True ((Get-ProblemId -Verdict $missing) -contains 'iso-missing') 'a named ISO that is not there is refused'
    $empty = New-Prerequisite -IsoPath ''
    Assert-True ((Get-ProblemId -Verdict $empty) -contains 'iso-missing') 'no ISO at all is the same refusal'
    $remedy = @($empty.Problems | Where-Object { $_.Id -eq 'iso-missing' })[0].Remedy
    Assert-Match '-IsoPath' $remedy 'the remedy must name the parameter'
}

Test-Case 'a machine without the Hyper-V module is refused with the feature command' {
    $verdict = New-Prerequisite -HyperVModuleAvailable $false
    $problem = @($verdict.Problems | Where-Object { $_.Id -eq 'hyperv-module-missing' })[0]
    Assert-Match 'Enable-WindowsOptionalFeature' $problem.Remedy 'the remedy must install the feature'
}

Test-Case 'an elevated shell in the group with a real ISO passes' {
    $iso = Join-Path (New-TestDirectory) 'win11.iso'
    Set-Content -LiteralPath $iso -Value 'not really an iso' -Encoding utf8NoBOM
    $verdict = New-Prerequisite -IsoPath $iso
    Assert-True $verdict.Ok "the happy path must pass: $((Get-ProblemId -Verdict $verdict) -join ', ')"
    Assert-Equal 0 $verdict.Problems.Count 'a passing verdict has no problems'
}

Test-Case 'the elevated remedy preserves script and argument paths with spaces' {
    $verdict = Test-ReleaseVmPrerequisite -ScriptPath 'C:\source tree\New-ReleaseVm.ps1' `
        -ScriptArgument @('-IsoPath', 'D:\Windows 11.iso') -IsElevated $false `
        -InHyperVAdministrators $true -HyperVModuleAvailable $true
    Assert-Match '-ArgumentList.+"C:\\source tree\\New-ReleaseVm\.ps1".+"D:\\Windows 11\.iso"' `
        $verdict.ElevatedCommand 'Start-Process joins an argument array, so paths must be quoted in one command line'
}

# ---------------------------------------------------------------------------
# Path derivation
# ---------------------------------------------------------------------------

Test-Case 'a run id derives its directory, its differencing disk and its machine name' {
    $paths = Get-ReleaseVmRunPath -RunId 'rc1-smoke' -Root 'T:\images'
    Assert-Equal 'T:\images\runs\rc1-smoke' $paths.RunDirectory 'the run directory hangs off the image root'
    Assert-Equal 'T:\images\runs\rc1-smoke\disk.vhdx' $paths.DifferencingDisk 'the run disk lives in the run directory'
    Assert-Equal 'T:\images\golden.vhdx' $paths.GoldenDisk 'the parent is the golden image'
    Assert-Match 'rc1-smoke$' $paths.VMName 'the machine is named after the run'
}

Test-Case 'a run id that would escape its directory is rejected rather than sanitized' {
    foreach ($bad in @('..', 'a/b', 'a\b', 'C:\evil', '-leading', '')) {
        $rejected = $false
        try { Get-ReleaseVmRunPath -RunId $bad -Root 'T:\images' | Out-Null }
        catch { $rejected = $true }
        Assert-True $rejected "the run id '$bad' must be rejected"
    }
}

# ---------------------------------------------------------------------------
# Network modes
# ---------------------------------------------------------------------------

Test-Case 'each network mode maps to exactly one cmdlet' {
    $disconnected = Get-ReleaseVmNetworkStep -Mode 'Disconnected' -VMName 'vm'
    Assert-Equal 'Disconnect-VMNetworkAdapter' $disconnected.Command 'no network means disconnecting the adapter'
    Assert-True (-not $disconnected.Parameters.Contains('SwitchName')) 'a disconnected adapter names no switch'

    $hostOnly = Get-ReleaseVmNetworkStep -Mode 'HostOnly' -VMName 'vm' -HostOnlySwitchName 'Test-Internal'
    Assert-Equal 'Connect-VMNetworkAdapter' $hostOnly.Command 'host-only connects to a switch'
    Assert-Equal 'Test-Internal' $hostOnly.Parameters['SwitchName'] 'host-only uses the internal switch'

    $connected = Get-ReleaseVmNetworkStep -Mode 'Connected' -VMName 'vm' -ExternalSwitchName 'Test-External'
    Assert-Equal 'Connect-VMNetworkAdapter' $connected.Command 'connected connects to a switch'
    Assert-Equal 'Test-External' $connected.Parameters['SwitchName'] 'connected uses the external switch'
}

Test-Case 'the run plan defaults to no network' {
    $paths = Get-ReleaseVmRunPath -RunId 'net-default' -Root 'T:\images'
    $plan = New-ReleaseVmRunPlan -RunPath $paths -GuestCommand 'verify.exe' -ResultDirectory 'T:\out'
    $network = @($plan | Where-Object { $_.Name -eq 'network' })[0]
    Assert-Equal 'Disconnect-VMNetworkAdapter' $network.Command 'an install gate that can reach the internet measures nothing'
}

# ---------------------------------------------------------------------------
# Plan shape
# ---------------------------------------------------------------------------

Test-Case 'the run plan takes a differencing disk and deletes it again' {
    $paths = Get-ReleaseVmRunPath -RunId 'lifecycle' -Root 'T:\images'
    $plan = New-ReleaseVmRunPlan -RunPath $paths -GuestCommand 'verify.exe' -ResultDirectory 'T:\out'
    $create = @($plan | Where-Object { $_.Name -eq 'differencing-disk' })[0]
    Assert-Equal 'New-VHD' $create.Command 'the run disk is a VHD'
    Assert-Equal $paths.GoldenDisk $create.Parameters['ParentPath'] 'its parent is the golden image'
    Assert-True ([bool]$create.Parameters['Differencing']) 'the golden image is never written to'

    $names = @($plan | ForEach-Object { $_.Name })
    Assert-True ($names -contains 'remove-disk') 'a run leaves nothing behind'
    Assert-True ([array]::IndexOf($names, 'collect') -lt [array]::IndexOf($names, 'stop')) `
        'the evidence is copied out before the machine is turned off'
}

Test-Case 'a kept disk is a deliberate exception, not the default' {
    $paths = Get-ReleaseVmRunPath -RunId 'keep' -Root 'T:\images'
    $plan = New-ReleaseVmRunPlan -RunPath $paths -GuestCommand 'verify.exe' -ResultDirectory 'T:\out' -KeepDisk
    Assert-True (-not (@($plan | ForEach-Object { $_.Name }) -contains 'remove-disk')) `
        '-KeepDisk must keep the disk'
}

Test-Case 'the create plan disables checkpoints before it attaches a GPU partition' {
    $plan = New-ReleaseVmCreatePlan -VMName 'vm' -Root 'T:\images' -GoldenDisk 'T:\images\golden.vhdx' `
        -AnswerIso 'T:\images\unattend.iso' -AnswerFile $script:AnswerFile -IsoPath 'W:\win11.iso' `
        -ProvisionScript $script:ProvisionScript -ProvisionManifest $script:ManifestPath
    $names = @($plan | ForEach-Object { $_.Name })
    Assert-True ($names -contains 'checkpoints') 'checkpoints are disabled explicitly'
    Assert-True ([array]::IndexOf($names, 'checkpoints') -lt [array]::IndexOf($names, 'gpu-adapter')) `
        'a machine that already took a checkpoint refuses the partition'
    Assert-True ([array]::IndexOf($names, 'stop-for-gpu') -lt [array]::IndexOf($names, 'gpu-adapter')) `
        'Hyper-V only attaches a GPU partition to a stopped machine'
}

Test-Case 'the GPU partition carries all four resource triples' {
    $plan = New-ReleaseVmCreatePlan -VMName 'vm' -Root 'T:\images' -GoldenDisk 'T:\images\golden.vhdx' `
        -AnswerIso 'T:\images\unattend.iso' -AnswerFile $script:AnswerFile -IsoPath 'W:\win11.iso' `
        -ProvisionScript $script:ProvisionScript -ProvisionManifest $script:ManifestPath
    $partition = @($plan | Where-Object { $_.Name -eq 'gpu-partition' })[0]
    foreach ($resource in @('VRAM', 'Encode', 'Decode', 'Compute')) {
        foreach ($bound in @('Min', 'Max', 'Optimal')) {
            $key = "$bound" + "Partition" + $resource
            Assert-True ($partition.Parameters.Contains($key)) "the partition must set $key"
            Assert-True ($partition.Parameters[$key] -gt 0) "$key must be a positive partition value"
        }
    }
    Assert-True ($partition.Parameters['MinPartitionEncode'] -le $partition.Parameters['MaxPartitionEncode']) `
        'the minimum cannot exceed the maximum'
}

Test-Case 'golden-image provisioning connects only for pinned downloads' {
    $plan = New-ReleaseVmCreatePlan -VMName 'vm' -Root 'T:\images' -GoldenDisk 'T:\images\golden.vhdx' `
        -AnswerIso 'T:\images\unattend.iso' -AnswerFile $script:AnswerFile -IsoPath 'W:\win11.iso' `
        -ProvisionScript $script:ProvisionScript -ProvisionManifest $script:ManifestPath `
        -ProvisionSwitchName 'Test-NAT'
    $names = @($plan | ForEach-Object Name)
    $connect = @($plan | Where-Object Name -eq 'provision-network')[0]
    $disconnect = @($plan | Where-Object Name -eq 'disconnect-provision-network')[0]

    Assert-Equal 'Connect-VMNetworkAdapter' $connect.Command 'provisioning needs a network path to pinned URLs'
    Assert-Equal 'Test-NAT' $connect.Parameters.SwitchName 'the build names its provisioning switch explicitly'
    Assert-True ([array]::IndexOf($names, 'provision-network') -lt [array]::IndexOf($names, 'provision')) `
        'the guest must connect before provisioning'
    Assert-True $disconnect.AlwaysRun 'the golden image is disconnected even when provisioning fails'
    Assert-Equal 'provision-network' $disconnect.RunIfCompleted 'only this build may disconnect the adapter'
}

Test-Case 'a boolean parameter is printed in the form that actually binds' {
    $step = New-ReleaseVmStep -Name 'x' -Command 'Stop-VM' -Parameters ([ordered]@{ Name = 'vm'; Force = $true })
    $line = Format-ReleaseVmStep -Step $step
    Assert-Match '-Force:\$true' $line '"-Force $true" hands $true to the next positional parameter instead'
}

Test-Case 'a step that needs the guest credential says so without carrying one' {
    $paths = Get-ReleaseVmRunPath -RunId 'cred' -Root 'T:\images'
    $plan = New-ReleaseVmRunPlan -RunPath $paths -GuestCommand 'verify.exe' -ResultDirectory 'T:\out'
    $run = @($plan | Where-Object { $_.Name -eq 'run' })[0]
    Assert-True $run.NeedsCredential 'the campaign step runs as the guest user'
    Assert-True (-not $run.Parameters.Contains('Credential')) 'a printable plan must not carry a password'
    Assert-Match '-Credential \$guestCredential' (Format-ReleaseVmStep -Step $run) `
        'the printed plan still shows where the credential goes'
}

Test-Case 'cleanup steps run after a campaign infrastructure failure' {
    $global:releaseVmCleanupObserved = $false
    function global:Invoke-FixtureFailure { throw 'fixture failure' }
    function global:Invoke-FixtureCleanup { $global:releaseVmCleanupObserved = $true }

    $plan = @(
        New-ReleaseVmStep -Name 'fail' -Command 'Invoke-FixtureFailure'
        New-ReleaseVmStep -Name 'cleanup' -Command 'Invoke-FixtureCleanup' -AlwaysRun
    )
    try { Invoke-ReleaseVmPlan -Plan $plan | Out-Null } catch { }
    Remove-Item function:\Invoke-FixtureFailure, function:\Invoke-FixtureCleanup -ErrorAction SilentlyContinue

    Assert-True $global:releaseVmCleanupObserved 'the throwaway VM and disk must be cleaned up when a run step throws'
    Remove-Variable releaseVmCleanupObserved -Scope Global -ErrorAction SilentlyContinue
}

Test-Case 'cleanup never removes a VM whose creation step failed' {
    $global:releaseVmRemoved = $false
    function global:Invoke-FixtureVmCollision { throw 'the VM already exists' }
    function global:Remove-FixtureVm { $global:releaseVmRemoved = $true }

    $plan = @(
        New-ReleaseVmStep -Name 'virtual-machine' -Command 'Invoke-FixtureVmCollision'
        New-ReleaseVmStep -Name 'remove-vm' -Command 'Remove-FixtureVm' -AlwaysRun `
            -RunIfCompleted 'virtual-machine'
    )
    try { Invoke-ReleaseVmPlan -Plan $plan | Out-Null } catch { }
    Remove-Item function:\Invoke-FixtureVmCollision, function:\Remove-FixtureVm -ErrorAction SilentlyContinue

    Assert-True (-not $global:releaseVmRemoved) 'a name collision must not delete the VM that already owned the name'
    Remove-Variable releaseVmRemoved -Scope Global -ErrorAction SilentlyContinue
}

Test-Case 'a cleanup failure is reported when the campaign itself succeeded' {
    function global:Invoke-FixtureSuccess { }
    function global:Invoke-FixtureCleanupFailure { throw 'cleanup could not remove the VM' }

    $plan = @(
        New-ReleaseVmStep -Name 'run' -Command 'Invoke-FixtureSuccess'
        New-ReleaseVmStep -Name 'remove-vm' -Command 'Invoke-FixtureCleanupFailure' -AlwaysRun
    )
    $message = ''
    try { Invoke-ReleaseVmPlan -Plan $plan | Out-Null } catch { $message = $_.Exception.Message }
    Remove-Item function:\Invoke-FixtureSuccess, function:\Invoke-FixtureCleanupFailure -ErrorAction SilentlyContinue

    Assert-Match 'cleanup could not remove the VM' $message 'failed cleanup cannot be silently reported as a clean run'
}

# ---------------------------------------------------------------------------
# Dry runs of the two host scripts
# ---------------------------------------------------------------------------

Test-Case 'New-ReleaseVm -DryRun prints the plan and executes nothing' {
    $result = Invoke-Script -Path $script:NewVmScript -Arguments @('-DryRun', '-IsoPath', 'W:\win11.iso',
        '-Root', 'T:\images')
    Assert-Equal 0 $result.ExitCode 'a dry run is not a failure'
    Assert-Match 'nothing was executed' $result.Output 'the plan must say it did nothing'
    Assert-Match 'New-VM -Name .+ -Generation 2' $result.Output 'a generation 2 machine'
    Assert-Match 'Enable-VMTPM' $result.Output 'Windows 11 requires a vTPM'
    Assert-Match 'Set-VMFirmware .+ -EnableSecureBoot' $result.Output 'Windows 11 requires Secure Boot'
    Assert-Match 'Add-VMGpuPartitionAdapter' $result.Output 'the guest gets a GPU partition'
    Assert-Match 'Set-VM .+ -GuestControlledCacheTypes:\$true' $result.Output 'a partition needs guest cache control'
    Assert-Match 'HighMemoryMappedIoSpace 32GB' $result.Output 'a partition needs a large MMIO window'
    Assert-Match 'Copy-ReleaseVmDriverStore' $result.Output 'the host driver files are staged into the guest'
}

Test-Case 'New-ReleaseVm -DryRun without an ISO still prints the plan and names the refusal' {
    $result = Invoke-Script -Path $script:NewVmScript -Arguments @('-DryRun', '-Root', 'T:\images')
    Assert-Equal 0 $result.ExitCode 'a dry run reports preconditions, it does not fail on them'
    Assert-Match 'iso-missing' $result.Output 'the missing ISO must be named'
    Assert-Match 'nothing was executed' $result.Output 'the plan is still printed, so it can be reviewed'
}

Test-Case 'Invoke-ReleaseVmRun -DryRun prints a whole run and touches no image' {
    $result = Invoke-Script -Path $script:RunScript -Arguments @('-DryRun', '-RunId', 'dry-001',
        '-Root', 'T:\images', '-Network', 'Disconnected')
    Assert-Equal 0 $result.ExitCode 'a dry run is not a failure'
    Assert-Match 'nothing was executed' $result.Output 'the plan must say it did nothing'
    Assert-Match 'New-VHD .+ -Differencing:\$true' $result.Output 'the run disk is a differencing disk'
    Assert-Match 'dry-001' $result.Output 'the run id names the machine and the disk'
    Assert-Match 'Disconnect-VMNetworkAdapter' $result.Output 'the default is no network'
    Assert-Match 'Remove-Item .+disk\.vhdx' $result.Output 'the run disk is deleted afterwards'
    Assert-NoMatch 'Checkpoint-VM' $result.Output 'a GPU-partitioned machine cannot be checkpointed'
}

Test-Case 'run cleanup is ownership-gated and does not suppress cmdlet failures' {
    $paths = Get-ReleaseVmRunPath -RunId 'cleanup' -Root 'T:\images'
    $plan = New-ReleaseVmRunPlan -RunPath $paths -GuestCommand 'verify.exe' -ResultDirectory 'T:\out'
    $stop = @($plan | Where-Object Name -eq 'stop')[0]
    $removeVm = @($plan | Where-Object Name -eq 'remove-vm')[0]
    $removeDisk = @($plan | Where-Object Name -eq 'remove-disk')[0]

    Assert-Equal 'virtual-machine' $stop.RunIfCompleted 'only a VM created by this run may be stopped'
    Assert-Equal 'virtual-machine' $removeVm.RunIfCompleted 'only a VM created by this run may be removed'
    Assert-Equal 'differencing-disk' $removeDisk.RunIfCompleted 'only a disk created by this run may be removed'
    foreach ($step in @($stop, $removeVm, $removeDisk)) {
        Assert-True (-not $step.Parameters.Contains('ErrorAction')) "$($step.Name) must surface cleanup failures"
    }
}

Test-Case 'Invoke-ReleaseVmRun -DryRun accepts a network mode per call' {
    $result = Invoke-Script -Path $script:RunScript -Arguments @('-DryRun', '-RunId', 'dry-002',
        '-Root', 'T:\images', '-Network', 'Connected')
    Assert-Match 'Connect-VMNetworkAdapter' $result.Output 'a connected run connects the adapter'
}

Test-Case 'the run script refuses its removed harness command-line contract' {
    $result = Invoke-Script -Path $script:RunScript -Arguments @('-DryRun', '-RunId', 'needs-command',
        '-Root', 'T:\images')
    Assert-Match 'guest-command-missing' $result.Output 'the dry run must name the required integration input'
    Assert-NoMatch '--output' $result.Output 'the current harness has no --output option'

    $command = 'C:\ExoSnapRun\harness\ExoSnap.Verify.exe run --run-dir C:\ExoSnapRun\out'
    $explicit = Invoke-Script -Path $script:RunScript -Arguments @('-DryRun', '-RunId', 'has-command',
        '-Root', 'T:\images', '-GuestCommand', $command)
    Assert-Match ([regex]::Escape($command)) $explicit.Output 'an explicit current harness command is preserved'
    Assert-NoMatch 'guest-command-missing' $explicit.Output 'an explicit command satisfies the precondition'
}

# ---------------------------------------------------------------------------
# The provisioning manifest
# ---------------------------------------------------------------------------

Test-Case 'guest native output never becomes part of its exit status' {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $script:VmRoot 'ReleaseVm.psm1'), [ref]$tokens, [ref]$errors)
    $blocks = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.ScriptBlockExpressionAst] -and
        $node.ScriptBlock.ToString().Contains('& cmd.exe /c $CommandLine')
    }, $true))
    Assert-Equal 1 $blocks.Count 'the guest command has one remote execution block'
    $block = $blocks[0].ScriptBlock.GetScriptBlock()
    $result = & $block 'echo guest output & exit /b 7' ([IO.Path]::GetTempPath())
    Assert-Equal 7 $result 'stdout must be displayed separately from the integer exit code'
}

Test-Case 'provisioning native output never becomes part of its exit status' {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $script:VmRoot 'ReleaseVm.psm1'), [ref]$tokens, [ref]$errors)
    $blocks = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.ScriptBlockExpressionAst] -and
        $node.ScriptBlock.ToString().Contains('& powershell.exe -NoProfile -ExecutionPolicy Bypass')
    }, $true))
    Assert-Equal 1 $blocks.Count 'provisioning has one remote execution block'
    function global:powershell.exe { $global:LASTEXITCODE = 2; 'installer output' }
    try {
        $block = $blocks[0].ScriptBlock.GetScriptBlock()
        $result = & $block 'unused-fixture.ps1' @()
        Assert-Equal 2 $result 'installer stdout must not corrupt the reboot status'
    }
    finally { Remove-Item function:\powershell.exe }
}

Test-Case 'every package in the manifest is pinned in a form provisioning can verify' {
    $manifest = Import-PowerShellDataFile -LiteralPath $script:ManifestPath
    Assert-True ($manifest.packages.Count -ge 7) 'the guest needs its whole tool set described'

    $seen = @{}
    foreach ($package in $manifest.packages) {
        $id = [string]$package.id
        Assert-True (-not $seen.ContainsKey($id)) "the manifest names '$id' twice"
        $seen[$id] = $true

        Assert-True ($package.Contains('title')) "$id has no title"
        Assert-True ($package.Contains('reason')) "$id does not say why it is installed"
        Assert-True ($package.Contains('version')) "$id has no version pin"
        Assert-Match '^[0-9][0-9A-Za-z.+-]*$' ([string]$package.version) "$id has an unusable version pin"

        switch ([string]$package.kind) {
            'winget' {
                Assert-True ($package.Contains('wingetId')) "$id is a winget package with no package id"
                Assert-Match '^[A-Za-z0-9][A-Za-z0-9.+_-]*$' ([string]$package.wingetId) "$id has an unusable winget id"
            }
            'download' {
                Assert-True ($package.Contains('urlTemplate')) "$id is a download with no URL"
                $url = [string]$package.urlTemplate
                Assert-Match '^https://' $url "$id must be downloaded over https"
                Assert-True ($package.Contains('sha256')) "$id is a download with no hash pin"
                Assert-Match '^[0-9a-f]{64}$' ([string]$package.sha256) "$id has an unusable hash pin"
                $resolved = $url.Replace('{version}', [string]$package.version)
                Assert-NoMatch '\{' $resolved "$id has a URL template with a placeholder nothing fills in"
            }
            default { throw "$id has kind '$($package.kind)', which provisioning does not implement" }
        }
    }
}

Test-Case 'the IDD pin includes its root-device installer and matches the signed driver INF' {
    $manifest = Import-PowerShellDataFile -LiteralPath $script:ManifestPath
    $idd = @($manifest.packages | Where-Object id -eq 'idd')[0]
    $nefcon = @($manifest.packages | Where-Object id -eq 'nefcon')[0]

    Assert-Equal 'Root\MttVDD' $idd.hardwareId 'the hardware id must match the pinned INF'
    Assert-True ($null -ne $nefcon) 'the driver-only archive needs a pinned root-device installer'
    Assert-Equal 'nefconw.exe' $nefcon.fileName 'provisioning uses the windowless installer'
}

Test-Case 'the generated IDD settings use the pinned release schema' {
    $provisioning = Get-Content -LiteralPath $script:ProvisionScript -Raw
    Assert-Match '<options>' $provisioning 'the pinned driver reads HDRPlus from its options element'
    Assert-Match '<HDRPlus>\$hdrValue</HDRPlus>' $provisioning 'HDR must use the pinned case-sensitive element name'
    Assert-True (-not $provisioning.Contains('<hdrplus>')) 'the obsolete top-level setting is ignored by the pinned driver'
}

Test-Case 'VB-CABLE installer failure cannot be recorded as successful provisioning' {
    $provisioning = Get-Content -LiteralPath $script:ProvisionScript -Raw
    Assert-Match 'VB-CABLE installer exited' $provisioning 'the silent installer exit code must be checked'
}

Test-Case 'fresh Windows registers the inbox App Installer before winget is required' {
    $provisioning = Get-Content -LiteralPath $script:ProvisionScript -Raw
    Assert-Match 'Add-AppxPackage.+RegisterByFamilyName.+Microsoft\.DesktopAppInstaller_8wekyb3d8bbwe' `
        $provisioning 'first logon may not have registered the inbox winget alias yet'
}

Test-Case 'the manifest fixes the virtual monitor rather than inheriting the host' {
    $manifest = Import-PowerShellDataFile -LiteralPath $script:ManifestPath
    Assert-Equal 2560 $manifest.display.width 'the guest monitor has a fixed width'
    Assert-Equal 1440 $manifest.display.height 'the guest monitor has a fixed height'
    Assert-True ($manifest.display.refreshRates -contains 60) '60 Hz is the baseline mode'
    Assert-True ($manifest.display.refreshRates -contains 144) 'a high-refresh mode is needed by the present gates'
    Assert-True ($manifest.display.Contains('hdr')) 'HDR is a selectable property of the image'
}

Test-Case 'provisioning refuses to install a package whose pin was never recorded' {
    $directory = New-TestDirectory
    $fixture = Join-Path $directory 'manifest.psd1'
    @"
@{
    schema   = 1
    packages = @(
        @{
            id          = 'presentmon'
            title       = 'fixture'
            kind        = 'download'
            urlTemplate = 'https://example.invalid/presentmon-{version}.exe'
            version     = 'PIN-REQUIRED'
            sha256      = 'PIN-REQUIRED'
            fileName    = 'PresentMon.exe'
            layout      = 'file'
            reason      = 'fixture'
        }
    )
    display  = @{ width = 2560; height = 1440; refreshRates = @(60); hdr = `$false; configDirectory = '$directory' }
    paths    = @{ tools = '$directory\tools'; staging = '$directory\staging'; hostDriverStaging = '$directory\driver' }
}
"@ | Set-Content -LiteralPath $fixture -Encoding utf8NoBOM

    $output = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $script:ProvisionScript `
        -ManifestPath $fixture -StatePath (Join-Path $directory 'state.json') -Only presentmon 2>&1 | Out-String
    $code = $LASTEXITCODE

    Assert-Equal 1 $code 'an unpinned package must stop provisioning'
    Assert-Match 'no SHA-256 pin|no version pin' $output 'the failure must say which pin is missing'
    Assert-Match 'Get-FileHash|winget show' $output 'the failure must say how to produce the pin'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $directory 'staging/downloads'))) `
        'nothing may be downloaded before its pin is verified'
}

Test-Case 'provisioning describes its steps without running any of them' {
    $output = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass `
        -File $script:ProvisionScript -ListSteps 2>&1 | Out-String
    Assert-Equal 0 $LASTEXITCODE 'listing steps always succeeds'
    foreach ($step in @('power', 'hostdriver', 'vcredist', 'pwsh', 'ffmpeg', 'presentmon', 'soundvolumeview',
            'vbcable', 'idd', 'uac')) {
        Assert-Match "(?m)^$step\s*$" $output "the recipe must have a '$step' step"
    }
}

# ---------------------------------------------------------------------------
# The answer file
# ---------------------------------------------------------------------------

Test-Case 'autounattend.xml is well-formed and installs Windows without asking anything' {
    $xml = [xml](Get-Content -LiteralPath $script:AnswerFile -Raw)
    $namespace = [System.Xml.XmlNamespaceManager]::new($xml.NameTable)
    $namespace.AddNamespace('u', 'urn:schemas-microsoft-com:unattend')

    Assert-True ($null -ne $xml.SelectSingleNode('//u:ImageInstall/u:OSImage/u:InstallTo', $namespace)) `
        'setup must be told where to install'
    Assert-True ($null -ne $xml.SelectSingleNode('//u:UserData/u:AcceptEula', $namespace)) `
        'an unattended install accepts the licence in the answer file'

    $autoLogon = $xml.SelectSingleNode('//u:AutoLogon', $namespace)
    Assert-True ($null -ne $autoLogon) 'the console session must log on by itself'
    Assert-Equal 'true' $autoLogon.Enabled 'autologon must be enabled'
    Assert-True ([int]$autoLogon.LogonCount -ge 10) `
        'the session has to come back after every restart, not only after setup'
    Assert-True (-not [string]::IsNullOrWhiteSpace($autoLogon.Username)) 'autologon needs an account'

    $oobe = $xml.SelectSingleNode('//u:OOBE', $namespace)
    Assert-True ($null -ne $oobe) 'OOBE must be answered'
    foreach ($element in @('HideEULAPage', 'HideOnlineAccountScreens', 'HideWirelessSetupInOOBE',
            'SkipMachineOOBE', 'SkipUserOOBE')) {
        Assert-Equal 'true' $oobe.$element "OOBE must not stop on $element"
    }
    Assert-Equal '3' $oobe.ProtectYourPC 'ProtectYourPC 3 is the setting that asks nothing about diagnostic data'

    $account = $xml.SelectSingleNode('//u:LocalAccounts/u:LocalAccount', $namespace)
    Assert-True ($null -ne $account) 'the guest needs a local account'
    Assert-Equal 'Administrators' $account.Group 'PowerShell Direct runs as a local administrator'
    Assert-Equal $account.Name $autoLogon.Username 'the account that logs on is the account that was created'

    $bypass = $xml.SelectNodes('//u:RunSynchronousCommand/u:Path', $namespace) | ForEach-Object { $_.InnerText }
    Assert-Match 'BypassNRO' ($bypass -join ' ') `
        'Windows 11 stops on a network screen that no unattend section can answer'
}

Test-Case 'the GPU-P NVENC probe selects an NVIDIA adapter instead of DXGI index zero' {
    $source = Get-Content -LiteralPath (Join-Path $repositoryRoot `
        'tools/probes/probe_gpup_nvenc/src/main.cpp') -Raw
    Assert-Match 'for \(UINT adapterIndex = 0;' $source 'the IDD can precede the GPU partition in DXGI order'
    Assert-Match 'VendorId != 0x10DE' $source 'the recipe stages an NVIDIA driver and NVENC library'
    Assert-NoMatch 'EnumAdapters1\(0,' $source 'adapter zero is not guaranteed to be the GPU partition'
}

Write-Host ''
Write-Host "  $($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
