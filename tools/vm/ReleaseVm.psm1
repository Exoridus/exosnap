#Requires -Version 7.0
<#
.SYNOPSIS
    The plan behind the two host scripts that build and drive the release-verification
    guest.

.DESCRIPTION
    Everything a Hyper-V cmdlet would be called with lives here as data: a plan is an
    ordered list of steps, each one a command name plus its parameters. That shape is
    the reason -DryRun can be honest. A dry run that re-implements what the real run
    would do is a second program with its own bugs; this one prints the exact
    parameters the real run will splat, because they are the same object.

    None of these functions runs a Hyper-V cmdlet as a side effect of being called.
    The plan builders are pure, and `Invoke-ReleaseVmPlan` is the only thing that
    executes anything -- which is what makes the plans testable on a machine whose
    account is not even in Hyper-V Administrators.
#>

Set-StrictMode -Version Latest

# The three-part GPU partition triple, in Hyper-V's own units: partition values run
# from 0 to 1000000000, where the maximum is the whole adapter. 100000000 is a tenth
# of the GPU, which is what a capture-and-encode gate needs while the host keeps
# rendering the developer's desktop; the minimum is set slightly lower so the guest
# still starts when the host is briefly busier than that.
#
# Raising these is a decision with a cost on the host side, not a free knob: the
# partition is reserved for the guest while it runs. An encode-heavy soak wants
# 500000000, and nothing else does.
$script:GpuPartitionDefault = [ordered]@{
    MinPartitionVRAM     = 80000000
    MaxPartitionVRAM     = 100000000
    OptimalPartitionVRAM = 100000000

    MinPartitionEncode     = 80000000
    MaxPartitionEncode     = 100000000
    OptimalPartitionEncode = 100000000

    MinPartitionDecode     = 80000000
    MaxPartitionDecode     = 100000000
    OptimalPartitionDecode = 100000000

    MinPartitionCompute     = 80000000
    MaxPartitionCompute     = 100000000
    OptimalPartitionCompute = 100000000
}

# Well-known SID of the local "Hyper-V Administrators" group. Matched by SID rather
# than by name because the name is localized and this machine is not en-US throughout.
$script:HyperVAdministratorsSid = 'S-1-5-32-578'

$script:RunIdPattern = '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'

function Get-ReleaseVmDefault {
    <#
    .SYNOPSIS
        The recipe's fixed values, in one place both host scripts read.
    #>
    [OutputType([hashtable])]
    param()
    return @{
        Root             = 'D:\exosnap-vm'
        GoldenDiskName   = 'golden.vhdx'
        AnswerIsoName    = 'unattend.iso'
        VMName           = 'ExoSnap-Verify'
        RunVMPrefix      = 'ExoSnap-Run-'
        MemoryBytes      = 8GB
        ProcessorCount   = 4
        DiskSizeBytes    = 60GB
        GuestUserName    = 'exosnap'
        GuestRoot        = 'C:\ExoSnapRun'
        GuestProvisionRoot = 'C:\ExoSnapProvision'
        GuestDriverStaging = 'C:\HostDriverStore'
        HostDriverRepository = 'C:\Windows\System32\DriverStore\FileRepository'
        HostDriverPattern  = 'nv_dispi.inf_amd64_*'
        GpuPartition     = $script:GpuPartitionDefault
    }
}

# ---------------------------------------------------------------------------
# Steps and plans
# ---------------------------------------------------------------------------

function New-ReleaseVmStep {
    <#
    .SYNOPSIS
        One command in a plan.
    .PARAMETER NeedsCredential
        The step takes -Credential, and the credential is supplied at execution time.
        Kept out of the parameter table so a plan can be printed, logged and compared
        without carrying a password around in it.
    .PARAMETER AlwaysRun
        Run during cleanup even when an earlier ordinary step failed.
    .PARAMETER RunIfCompleted
        For an AlwaysRun step, the name of the creation step that must have completed.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Command,
        [System.Collections.IDictionary] $Parameters = @{},
        [string] $Detail = '',
        [switch] $NeedsCredential,
        [switch] $AlwaysRun,
        [string] $RunIfCompleted = ''
    )
    return @{
        Name            = $Name
        Command         = $Command
        Parameters      = $Parameters
        Detail          = $Detail
        NeedsCredential = [bool]$NeedsCredential
        AlwaysRun       = [bool]$AlwaysRun
        RunIfCompleted  = $RunIfCompleted
    }
}

function Format-ReleaseVmValue {
    <#
    .SYNOPSIS
        One parameter value, spelled the way PowerShell would accept it back.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory, ValueFromPipeline)] [AllowNull()] $Value)
    process {
        if ($null -eq $Value) { return '$null' }
        if ($Value -is [bool]) { if ($Value) { return '$true' } else { return '$false' } }
        if ($Value -is [switch]) { if ($Value.IsPresent) { return '$true' } else { return '$false' } }
        if ($Value -is [scriptblock]) {
            $text = ($Value.ToString() -replace '\s+', ' ').Trim()
            if ($text.Length -gt 60) { $text = $text.Substring(0, 57) + '...' }
            return "{ $text }"
        }
        if ($Value -is [System.Management.Automation.PSCredential]) { return '$guestCredential' }
        if ($Value -is [long] -or $Value -is [int]) {
            $number = [long]$Value
            if ($number -ge 1GB -and $number % 1GB -eq 0) { return "$($number / 1GB)GB" }
            if ($number -ge 1MB -and $number % 1MB -eq 0) { return "$($number / 1MB)MB" }
            return "$number"
        }
        if ($Value -is [array]) {
            return (@($Value | ForEach-Object { Format-ReleaseVmValue -Value $_ }) -join ',')
        }
        return "'" + ($Value.ToString() -replace "'", "''") + "'"
    }
}

function Format-ReleaseVmStep {
    <#
    .SYNOPSIS
        One step as the command line it will run.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [hashtable] $Step)
    $parts = @($Step.Command)
    foreach ($key in $Step.Parameters.Keys) {
        $value = $Step.Parameters[$key]
        # Boolean values take the colon form. Half of these parameters are switches,
        # and "-Force $true" hands $true to the next positional parameter instead of
        # to -Force -- so the printed plan would not be the plan that runs.
        if ($value -is [bool] -or $value -is [switch]) {
            $parts += "-${key}:$(Format-ReleaseVmValue -Value $value)"
            continue
        }
        $parts += "-$key"
        $parts += (Format-ReleaseVmValue -Value $value)
    }
    if ($Step.NeedsCredential) { $parts += @('-Credential', '$guestCredential') }
    return ($parts -join ' ')
}

function Write-ReleaseVmPlan {
    <#
    .SYNOPSIS
        Prints a plan and nothing else. The output of -DryRun.
    #>
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [hashtable[]] $Plan,
        [string] $Title = 'plan'
    )
    Write-Host ''
    Write-Host "  $Title ($($Plan.Count) step(s))"
    Write-Host ''
    $index = 0
    foreach ($step in $Plan) {
        $index++
        Write-Host ("  {0,2}. {1}" -f $index, $step.Name)
        Write-Host "      $(Format-ReleaseVmStep -Step $step)"
        if ($step.Detail) { Write-Host "      # $($step.Detail)" }
    }
    Write-Host ''
}

function Invoke-ReleaseVmPlan {
    <#
    .SYNOPSIS
        Runs ordinary plan steps to the first failure, then runs cleanup steps.
    .DESCRIPTION
        The only function in this module that changes anything. A step that throws
        stops ordinary execution: those steps are ordered by dependency, and
        continuing past a failed New-VM produces a second page of errors about a
        machine that does not exist. AlwaysRun steps still execute when the resource
        creation step named by RunIfCompleted succeeded.

        Returns what each step returned, keyed by step name. The campaign step's exit
        code is read from there rather than thrown on -- a run that found defects is a
        result and not an infrastructure error.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [hashtable[]] $Plan,
        [System.Management.Automation.PSCredential] $Credential
    )
    $outputs = @{}
    $failure = $null
    $runStep = {
        param($step)
        Write-Host "  $($step.Name)"
        Write-Host "      $(Format-ReleaseVmStep -Step $step)"
        $parameters = [ordered]@{}
        foreach ($key in $step.Parameters.Keys) { $parameters[$key] = $step.Parameters[$key] }
        if ($step.NeedsCredential) {
            if (-not $Credential) {
                throw "step '$($step.Name)' needs the guest credential and none was supplied"
            }
            $parameters['Credential'] = $Credential
        }
        $outputs[$step.Name] = & $step.Command @parameters
    }

    try {
        foreach ($step in @($Plan | Where-Object { -not $_.AlwaysRun })) {
            & $runStep $step
        }
    }
    catch {
        $failure = $_
    }
    finally {
        foreach ($step in @($Plan | Where-Object { $_.AlwaysRun })) {
            if ($step.RunIfCompleted -and -not $outputs.ContainsKey($step.RunIfCompleted)) {
                Write-Host "  $($step.Name) (skipped; '$($step.RunIfCompleted)' did not complete)"
                continue
            }
            try {
                & $runStep $step
            }
            catch {
                if ($null -eq $failure) {
                    $failure = $_
                }
                else {
                    Write-Warning "cleanup step '$($step.Name)' failed: $($_.Exception.Message)"
                }
            }
        }
    }
    if ($null -ne $failure) {
        throw $failure
    }
    return $outputs
}

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

function Get-ReleaseVmHostState {
    <#
    .SYNOPSIS
        What this shell can and cannot do to Hyper-V.
    .DESCRIPTION
        Reads only. Group membership is checked by SID, and against the token this
        process actually holds -- a membership added but not yet logged into is
        exactly the case that produces "Access denied" from a cmdlet that worked for
        somebody else on the same machine.
    #>
    [OutputType([hashtable])]
    param()
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $inGroup = $principal.IsInRole([Security.Principal.SecurityIdentifier]::new($script:HyperVAdministratorsSid))
    return @{
        IsElevated             = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        InHyperVAdministrators = $inGroup
        HyperVModuleAvailable  = [bool](Get-Command -Name 'Get-VM' -ErrorAction SilentlyContinue)
        UserName               = $identity.Name
    }
}

function Test-ReleaseVmPrerequisite {
    <#
    .SYNOPSIS
        Whether this shell may run a plan, and what to do when it may not.
    .DESCRIPTION
        Every input is a parameter rather than a machine query, so the three
        documented refusals -- no Hyper-V access, ISO
        missing -- are reachable from a test on a machine in none of those states.

        Returns @{ Ok; Problems; ElevatedCommand }, where each problem carries the
        exact command that fixes it. "Run this elevated" without the command is the
        failure mode this replaces: it is the point at which somebody types
        something slightly different and debugs the difference.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $ScriptPath,
        [string[]] $ScriptArgument = @(),
        [string] $IsoPath,
        [Parameter(Mandatory)] [bool] $IsElevated,
        [Parameter(Mandatory)] [bool] $InHyperVAdministrators,
        [Parameter(Mandatory)] [bool] $HyperVModuleAvailable,
        [string] $UserName = "$env:USERDOMAIN\$env:USERNAME"
    )

    $arguments = @('-NoProfile', '-File', $ScriptPath) + $ScriptArgument
    $processCommandLine = (@($arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' ')
    $elevated = "Start-Process -FilePath pwsh -Verb RunAs -ArgumentList '" +
        ($processCommandLine -replace "'", "''") + "'"

    $problems = @()

    if (-not $HyperVModuleAvailable) {
        $problems += @{
            Id      = 'hyperv-module-missing'
            Message = 'the Hyper-V PowerShell module is not installed on this machine'
            Remedy  = "Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All   # elevated, then restart"
        }
    }
    # Either one grants the access every VM cmdlet checks, so neither alone is a
    # refusal: the group exists precisely so an unelevated shell can manage virtual
    # machines, and an elevated token carries Administrators, which has that access
    # anyway. Nothing else in the plan needs elevation -- the answer ISO is built
    # through IMAPI2FS, and the host driver store is read, never written.
    if (-not $InHyperVAdministrators -and -not $IsElevated) {
        $problems += @{
            Id      = 'no-hyperv-access'
            Message = "'$UserName' is neither in the Hyper-V Administrators group nor elevated, so every VM cmdlet will fail with Access denied"
            Remedy  = "Add-LocalGroupMember -Group 'Hyper-V Administrators' -Member '$UserName'   # elevated, then sign out and back in; or run the script elevated: $elevated"
        }
    }
    if ($PSBoundParameters.ContainsKey('IsoPath')) {
        if ([string]::IsNullOrWhiteSpace($IsoPath)) {
            $problems += @{
                Id      = 'iso-missing'
                Message = 'no Windows 11 installation ISO was named'
                Remedy  = "$([IO.Path]::GetFileName($ScriptPath)) -IsoPath <path to Win11 x64 ISO>   # download it from https://www.microsoft.com/software-download/windows11"
            }
        }
        elseif (-not (Test-Path -LiteralPath $IsoPath -PathType Leaf)) {
            $problems += @{
                Id      = 'iso-missing'
                Message = "the installation ISO '$IsoPath' does not exist"
                Remedy  = "$([IO.Path]::GetFileName($ScriptPath)) -IsoPath <path to Win11 x64 ISO>   # download it from https://www.microsoft.com/software-download/windows11"
            }
        }
    }

    return @{
        Ok              = ($problems.Count -eq 0)
        Problems        = $problems
        ElevatedCommand = $elevated
    }
}

function Write-ReleaseVmPrerequisite {
    <#
    .SYNOPSIS
        Prints a prerequisite verdict.
    #>
    param([Parameter(Mandatory)] [hashtable] $Verdict)
    if ($Verdict.Ok) {
        Write-Host '  preconditions: Hyper-V access (group or elevation), ISO present.'
        return
    }
    Write-Host ''
    Write-Host '  preconditions NOT met:'
    foreach ($problem in $Verdict.Problems) {
        Write-Host "    [$($problem.Id)] $($problem.Message)"
        Write-Host "        $($problem.Remedy)"
    }
    Write-Host ''
}

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

function Get-ReleaseVmPath {
    <#
    .SYNOPSIS
        Where the golden image and its answer ISO live.
    #>
    [OutputType([hashtable])]
    param([string] $Root)
    $defaults = Get-ReleaseVmDefault
    if (-not $Root) { $Root = $defaults.Root }
    # Composed rather than joined. Join-Path asks the provider, and the provider
    # refuses a drive that is not mounted on THIS machine -- so a plan for an image
    # root on a drive the developer has and a reviewer does not could not be printed.
    return @{
        Root          = $Root
        GoldenDisk    = [IO.Path]::Combine($Root, $defaults.GoldenDiskName)
        AnswerIso     = [IO.Path]::Combine($Root, $defaults.AnswerIsoName)
        RunRoot       = [IO.Path]::Combine($Root, 'runs')
    }
}

function Get-ReleaseVmRunPath {
    <#
    .SYNOPSIS
        Everything one run is named after its id.
    .DESCRIPTION
        A run id becomes a directory name, a VHDX name and a virtual machine name, so
        it is validated rather than sanitized: silently rewriting an id would make the
        run directory on the host and the id inside the qualification record disagree,
        and the record is the thing a release is promoted on.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $RunId,
        [string] $Root
    )
    if ($RunId -notmatch $script:RunIdPattern) {
        throw ("the run id '$RunId' is not usable as a directory, disk and virtual machine name. " +
               'Use letters, digits, dot, dash and underscore, starting with a letter or digit.')
    }
    $paths = Get-ReleaseVmPath -Root $Root
    $defaults = Get-ReleaseVmDefault
    $runDirectory = [IO.Path]::Combine($paths.RunRoot, $RunId)
    return @{
        RunId             = $RunId
        Root              = $paths.Root
        GoldenDisk        = $paths.GoldenDisk
        RunDirectory      = $runDirectory
        DifferencingDisk  = [IO.Path]::Combine($runDirectory, 'disk.vhdx')
        VMName            = "$($defaults.RunVMPrefix)$RunId"
        GuestRoot         = $defaults.GuestRoot
        GuestArtifacts    = [IO.Path]::Combine($defaults.GuestRoot, 'artifacts')
        GuestHarness      = [IO.Path]::Combine($defaults.GuestRoot, 'harness')
        GuestResults      = [IO.Path]::Combine($defaults.GuestRoot, 'out')
    }
}

function Get-ReleaseVmNetworkStep {
    <#
    .SYNOPSIS
        The one network cmdlet a run mode maps to.
    .DESCRIPTION
        Three modes and no fourth:

          Disconnected  the default. An install or update gate that reaches the
                        internet is not measuring the artifact it was given.
          HostOnly      an internal switch. The harness can talk to the guest over
                        IP; nothing else can.
          Connected     the external switch. Only for gates whose subject IS the
                        network, such as a package manager fetching from a feed.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [ValidateSet('Connected', 'Disconnected', 'HostOnly')] [string] $Mode,
        [Parameter(Mandatory)] [string] $VMName,
        [string] $ExternalSwitchName = 'Default Switch',
        [string] $HostOnlySwitchName = 'ExoSnap-HostOnly'
    )
    switch ($Mode) {
        'Disconnected' {
            return New-ReleaseVmStep -Name 'network' -Command 'Disconnect-VMNetworkAdapter' `
                -Parameters ([ordered]@{ VMName = $VMName }) `
                -Detail 'no network: the guest sees only what was copied into it'
        }
        'HostOnly' {
            return New-ReleaseVmStep -Name 'network' -Command 'Connect-VMNetworkAdapter' `
                -Parameters ([ordered]@{ VMName = $VMName; SwitchName = $HostOnlySwitchName }) `
                -Detail "internal switch '$HostOnlySwitchName': host and guest, nothing else"
        }
        default {
            return New-ReleaseVmStep -Name 'network' -Command 'Connect-VMNetworkAdapter' `
                -Parameters ([ordered]@{ VMName = $VMName; SwitchName = $ExternalSwitchName }) `
                -Detail "external switch '$ExternalSwitchName': the guest reaches the internet"
        }
    }
}

# ---------------------------------------------------------------------------
# Plan builders
# ---------------------------------------------------------------------------

function New-ReleaseVmCreatePlan {
    <#
    .SYNOPSIS
        The plan that produces the golden image.
    .DESCRIPTION
        Five phases, because three of them cannot run while the machine is in the
        state the previous one leaves it in:

          create   the machine, its disk, its firmware, both DVD drives
          install  boot once and let the answer file install Windows unattended
          gpu      the GPU partition, which Hyper-V only accepts on a stopped VM
          driver   the host's own display driver files, copied in one by one
          provision the guest script, over PowerShell Direct

        Checkpoints are disabled in the create phase rather than later: a virtual
        machine with a GPU partition cannot be checkpointed at all, and a machine
        that took an automatic checkpoint before the partition was added refuses the
        partition afterwards.
    #>
    [OutputType([hashtable[]])]
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [string] $Root,
        [Parameter(Mandatory)] [string] $GoldenDisk,
        [Parameter(Mandatory)] [string] $AnswerIso,
        [Parameter(Mandatory)] [string] $AnswerFile,
        [Parameter(Mandatory)] [AllowEmptyString()] [string] $IsoPath,
        [Parameter(Mandatory)] [string] $ProvisionScript,
        [Parameter(Mandatory)] [string] $ProvisionManifest,
        [string] $HostDriverPackage,
        [string] $ProvisionSwitchName = 'Default Switch',
        [long] $MemoryBytes = 8GB,
        [int] $ProcessorCount = 4,
        [long] $DiskSizeBytes = 60GB,
        [System.Collections.IDictionary] $GpuPartition,
        [string[]] $Phase = @('create', 'install', 'gpu', 'driver', 'provision'),
        [int] $InstallTimeoutMinutes = 90
    )
    if (-not $GpuPartition) { $GpuPartition = $script:GpuPartitionDefault }
    $defaults = Get-ReleaseVmDefault
    $plan = @()

    if ($Phase -contains 'create') {
        $plan += New-ReleaseVmStep -Name 'image-root' -Command 'New-Item' -Parameters ([ordered]@{
                ItemType = 'Directory'; Path = $Root; Force = $true
            }) -Detail 'the golden image and every run disk live here'

        $plan += New-ReleaseVmStep -Name 'answer-iso' -Command 'New-ReleaseVmAnswerIso' -Parameters ([ordered]@{
                AnswerFile = $AnswerFile; Destination = $AnswerIso
            }) -Detail 'Windows Setup searches removable media for autounattend.xml, so it arrives as a second DVD'

        $plan += New-ReleaseVmStep -Name 'golden-disk' -Command 'New-VHD' -Parameters ([ordered]@{
                Path = $GoldenDisk; SizeBytes = $DiskSizeBytes; Dynamic = $true
            }) -Detail 'dynamic: the file grows to what Windows and the tool set actually occupy'

        $plan += New-ReleaseVmStep -Name 'virtual-machine' -Command 'New-VM' -Parameters ([ordered]@{
                Name = $VMName; Generation = 2; MemoryStartupBytes = $MemoryBytes; VHDPath = $GoldenDisk
            }) -Detail 'generation 2: UEFI, Secure Boot and a vTPM, all three required by Windows 11'

        $plan += New-ReleaseVmStep -Name 'processor' -Command 'Set-VMProcessor' -Parameters ([ordered]@{
                VMName = $VMName; Count = $ProcessorCount
            })

        $plan += New-ReleaseVmStep -Name 'memory' -Command 'Set-VMMemory' -Parameters ([ordered]@{
                VMName = $VMName; DynamicMemoryEnabled = $false; StartupBytes = $MemoryBytes
            }) -Detail 'static: a GPU partition maps guest memory, and dynamic memory and that mapping disagree'

        $plan += New-ReleaseVmStep -Name 'key-protector' -Command 'Set-VMKeyProtector' -Parameters ([ordered]@{
                VMName = $VMName; NewLocalKeyProtector = $true
            }) -Detail 'the vTPM needs a key protector before it can be enabled'

        $plan += New-ReleaseVmStep -Name 'tpm' -Command 'Enable-VMTPM' -Parameters ([ordered]@{ VMName = $VMName })

        $plan += New-ReleaseVmStep -Name 'secure-boot' -Command 'Set-VMFirmware' -Parameters ([ordered]@{
                VMName = $VMName; EnableSecureBoot = 'On'; SecureBootTemplate = 'MicrosoftWindows'
            })

        $plan += New-ReleaseVmStep -Name 'install-media' -Command 'Add-VMDvdDrive' -Parameters ([ordered]@{
                VMName = $VMName; Path = $IsoPath
            })

        $plan += New-ReleaseVmStep -Name 'answer-media' -Command 'Add-VMDvdDrive' -Parameters ([ordered]@{
                VMName = $VMName; Path = $AnswerIso
            })

        $plan += New-ReleaseVmStep -Name 'boot-order' -Command 'Set-ReleaseVmBootFromDvd' -Parameters ([ordered]@{
                VMName = $VMName; IsoPath = $IsoPath
            }) -Detail 'boot the installation DVD, not the empty disk'

        $plan += New-ReleaseVmStep -Name 'checkpoints' -Command 'Set-VM' -Parameters ([ordered]@{
                Name = $VMName; AutomaticCheckpointsEnabled = $false; CheckpointType = 'Disabled'
            }) -Detail 'a GPU-partitioned machine cannot be checkpointed; disable it before there is one'

        $plan += New-ReleaseVmStep -Name 'guest-services' -Command 'Enable-ReleaseVmGuestServices' -Parameters ([ordered]@{
                VMName = $VMName
            }) -Detail 'Copy-VMFile is this service; without it nothing reaches the guest'
    }

    if ($Phase -contains 'install') {
        $plan += New-ReleaseVmStep -Name 'confirm-install-console' -Command 'Read-Host' -Parameters ([ordered]@{
                Prompt = "Open the console for VM '$VMName' in Hyper-V Manager now. Press Enter here to start it, then immediately press a key in that VM console at the DVD boot prompt"
            }) -Detail 'one-time operator boot confirmation; the answer file takes over after Windows Setup starts'

        $plan += New-ReleaseVmStep -Name 'start-install' -Command 'Start-VM' -Parameters ([ordered]@{ Name = $VMName })

        $plan += New-ReleaseVmStep -Name 'wait-install' -Command 'Wait-ReleaseVmPowerShellDirect' -Parameters ([ordered]@{
                VMName = $VMName; TimeoutMinutes = $InstallTimeoutMinutes
            }) -NeedsCredential -Detail 'the answer file installs Windows and logs the console session on; this waits for it'

    }

    if ($Phase -contains 'gpu') {
        # In the gpu phase, not the install phase: -Phase gpu on a machine somebody
        # left running is the normal way to resume an interrupted build, and it used
        # to fail on the first cmdlet because the stop belonged to the phase before.
        $plan += New-ReleaseVmStep -Name 'stop-for-gpu' -Command 'Stop-ReleaseVmIfRunning' -Parameters ([ordered]@{
                VMName = $VMName
            }) -Detail 'Hyper-V only attaches a GPU partition to a stopped machine'

        $plan += New-ReleaseVmStep -Name 'mmio-space' -Command 'Set-VM' -Parameters ([ordered]@{
                Name = $VMName
                GuestControlledCacheTypes = $true
                LowMemoryMappedIoSpace = 1GB
                HighMemoryMappedIoSpace = 32GB
            }) -Detail 'a partitioned adapter needs the guest to control cache types and a large MMIO window'

        $plan += New-ReleaseVmStep -Name 'gpu-adapter' -Command 'Add-VMGpuPartitionAdapter' -Parameters ([ordered]@{
                VMName = $VMName
            })

        $partitionParameters = [ordered]@{ VMName = $VMName }
        foreach ($key in $GpuPartition.Keys) { $partitionParameters[$key] = $GpuPartition[$key] }
        $plan += New-ReleaseVmStep -Name 'gpu-partition' -Command 'Set-VMGpuPartitionAdapter' `
            -Parameters $partitionParameters `
            -Detail 'partition units out of 1000000000; this is a tenth of the adapter'
    }

    if ($Phase -contains 'driver') {
        $plan += New-ReleaseVmStep -Name 'start-for-driver' -Command 'Start-VM' -Parameters ([ordered]@{ Name = $VMName })

        $plan += New-ReleaseVmStep -Name 'wait-for-driver' -Command 'Wait-ReleaseVmPowerShellDirect' -Parameters ([ordered]@{
                VMName = $VMName; TimeoutMinutes = 15
            }) -NeedsCredential

        $plan += New-ReleaseVmStep -Name 'stage-driver' -Command 'Copy-ReleaseVmDriverStore' -Parameters ([ordered]@{
                VMName = $VMName
                HostDriverPackage = $HostDriverPackage
                Destination = $defaults.GuestDriverStaging
            }) -Detail 'the guest must run the host adapter''s own driver files, one Copy-VMFile per file'
    }

    if ($Phase -contains 'provision') {
        $plan += New-ReleaseVmStep -Name 'provision-network' -Command 'Connect-VMNetworkAdapter' `
            -Parameters ([ordered]@{ VMName = $VMName; SwitchName = $ProvisionSwitchName }) `
            -Detail "temporary access through '$ProvisionSwitchName' for hash-pinned provisioning downloads"

        $plan += New-ReleaseVmStep -Name 'copy-provisioning' -Command 'Copy-ReleaseVmFileSet' -Parameters ([ordered]@{
                VMName = $VMName
                Path = @($ProvisionScript, $ProvisionManifest)
                Destination = $defaults.GuestProvisionRoot
            })

        $plan += New-ReleaseVmStep -Name 'provision' -Command 'Invoke-ReleaseVmProvisioning' -Parameters ([ordered]@{
                VMName = $VMName
                ProvisionRoot = $defaults.GuestProvisionRoot
            }) -NeedsCredential -Detail 'runs provision.ps1 in the guest; exit code 2 means it needs a restart and another pass'

        $plan += New-ReleaseVmStep -Name 'disconnect-provision-network' `
            -Command 'Disconnect-VMNetworkAdapter' -Parameters ([ordered]@{ VMName = $VMName }) `
            -AlwaysRun -RunIfCompleted 'provision-network' `
            -Detail 'the frozen image and ordinary campaigns start disconnected'
    }

    return $plan
}

function New-ReleaseVmRunPlan {
    <#
    .SYNOPSIS
        The plan for one campaign run.
    .DESCRIPTION
        A differencing disk is the whole design: the golden image is never written to,
        so every run starts from the same bytes, and the only thing a run can leave
        behind is a file this plan deletes at the end.

        The in-guest command is a string rather than a scriptblock. It names a program
        that is built elsewhere and does not exist yet as a runnable, and a plan that
        could only be printed once its subject existed would be useless for building
        the thing.
    #>
    [OutputType([hashtable[]])]
    param(
        [Parameter(Mandatory)] [hashtable] $RunPath,
        [Parameter(Mandatory)] [string] $GuestCommand,
        [Parameter(Mandatory)] [string] $ResultDirectory,
        [string] $ArtifactDirectory,
        [string] $HarnessDirectory,
        [ValidateSet('Connected', 'Disconnected', 'HostOnly')] [string] $Network = 'Disconnected',
        [long] $MemoryBytes = 8GB,
        [int] $ProcessorCount = 4,
        [System.Collections.IDictionary] $GpuPartition,
        [int] $BootTimeoutMinutes = 15,
        [int] $RunTimeoutMinutes = 120,
        [switch] $KeepDisk
    )
    if (-not $GpuPartition) { $GpuPartition = $script:GpuPartitionDefault }
    $vm = $RunPath.VMName
    $plan = @()

    $plan += New-ReleaseVmStep -Name 'run-directory' -Command 'New-Item' -Parameters ([ordered]@{
            ItemType = 'Directory'; Path = $RunPath.RunDirectory; Force = $true
        })

    $plan += New-ReleaseVmStep -Name 'differencing-disk' -Command 'New-VHD' -Parameters ([ordered]@{
            Path = $RunPath.DifferencingDisk; ParentPath = $RunPath.GoldenDisk; Differencing = $true
        }) -Detail 'the golden image is the parent and is never written to'

    $plan += New-ReleaseVmStep -Name 'virtual-machine' -Command 'New-VM' -Parameters ([ordered]@{
            Name = $vm; Generation = 2; MemoryStartupBytes = $MemoryBytes; VHDPath = $RunPath.DifferencingDisk
        }) -Detail 'no -SwitchName: the adapter starts disconnected and the network step decides'

    $plan += New-ReleaseVmStep -Name 'processor' -Command 'Set-VMProcessor' -Parameters ([ordered]@{
            VMName = $vm; Count = $ProcessorCount
        })

    $plan += New-ReleaseVmStep -Name 'memory' -Command 'Set-VMMemory' -Parameters ([ordered]@{
            VMName = $vm; DynamicMemoryEnabled = $false; StartupBytes = $MemoryBytes
        })

    $plan += New-ReleaseVmStep -Name 'checkpoints' -Command 'Set-VM' -Parameters ([ordered]@{
            Name = $vm; AutomaticCheckpointsEnabled = $false; CheckpointType = 'Disabled'
        })

    $plan += New-ReleaseVmStep -Name 'key-protector' -Command 'Set-VMKeyProtector' -Parameters ([ordered]@{
            VMName = $vm; NewLocalKeyProtector = $true
        })

    $plan += New-ReleaseVmStep -Name 'tpm' -Command 'Enable-VMTPM' -Parameters ([ordered]@{ VMName = $vm })

    $plan += New-ReleaseVmStep -Name 'secure-boot' -Command 'Set-VMFirmware' -Parameters ([ordered]@{
            VMName = $vm; EnableSecureBoot = 'On'; SecureBootTemplate = 'MicrosoftWindows'
        }) -Detail 'the installed Windows was set up under Secure Boot and will not start without it'

    $plan += New-ReleaseVmStep -Name 'mmio-space' -Command 'Set-VM' -Parameters ([ordered]@{
            Name = $vm
            GuestControlledCacheTypes = $true
            LowMemoryMappedIoSpace = 1GB
            HighMemoryMappedIoSpace = 32GB
        })

    $plan += New-ReleaseVmStep -Name 'gpu-adapter' -Command 'Add-VMGpuPartitionAdapter' -Parameters ([ordered]@{
            VMName = $vm
        })

    $partitionParameters = [ordered]@{ VMName = $vm }
    foreach ($key in $GpuPartition.Keys) { $partitionParameters[$key] = $GpuPartition[$key] }
    $plan += New-ReleaseVmStep -Name 'gpu-partition' -Command 'Set-VMGpuPartitionAdapter' -Parameters $partitionParameters

    $plan += Get-ReleaseVmNetworkStep -Mode $Network -VMName $vm

    $plan += New-ReleaseVmStep -Name 'guest-services' -Command 'Enable-ReleaseVmGuestServices' -Parameters ([ordered]@{
            VMName = $vm
        }) -Detail 'integration settings belong to this new VM, not to its parent disk'

    $plan += New-ReleaseVmStep -Name 'start' -Command 'Start-VM' -Parameters ([ordered]@{ Name = $vm })

    $plan += New-ReleaseVmStep -Name 'wait-for-guest' -Command 'Wait-ReleaseVmPowerShellDirect' -Parameters ([ordered]@{
            VMName = $vm; TimeoutMinutes = $BootTimeoutMinutes
        }) -NeedsCredential

    if ($ArtifactDirectory) {
        $plan += New-ReleaseVmStep -Name 'copy-artifacts' -Command 'Copy-ReleaseVmDirectory' -Parameters ([ordered]@{
                VMName = $vm; Source = $ArtifactDirectory; Destination = $RunPath.GuestArtifacts
            }) -Detail 'the bytes under test, copied rather than shared'
    }
    if ($HarnessDirectory) {
        $plan += New-ReleaseVmStep -Name 'copy-harness' -Command 'Copy-ReleaseVmDirectory' -Parameters ([ordered]@{
                VMName = $vm; Source = $HarnessDirectory; Destination = $RunPath.GuestHarness
            })
    }

    $plan += New-ReleaseVmStep -Name 'run' -Command 'Invoke-ReleaseVmCommand' -Parameters ([ordered]@{
            VMName = $vm
            Command = $GuestCommand
            WorkingDirectory = $RunPath.GuestRoot
            TimeoutMinutes = $RunTimeoutMinutes
        }) -NeedsCredential -Detail 'the campaign itself, inside the guest'

    $plan += New-ReleaseVmStep -Name 'collect' -Command 'Copy-ReleaseVmDirectoryBack' -Parameters ([ordered]@{
            VMName = $vm; Source = $RunPath.GuestResults; Destination = $ResultDirectory
        }) -NeedsCredential -Detail 'PowerShell Direct in the other direction: Copy-VMFile is host-to-guest only'

    $plan += New-ReleaseVmStep -Name 'stop' -Command 'Stop-VM' -Parameters ([ordered]@{
            Name = $vm; TurnOff = $true; Force = $true
        }) -AlwaysRun -RunIfCompleted 'virtual-machine' `
        -Detail 'turned off, not shut down: the evidence is already on the host'

    $plan += New-ReleaseVmStep -Name 'remove-vm' -Command 'Remove-VM' -Parameters ([ordered]@{
            Name = $vm; Force = $true
        }) -AlwaysRun -RunIfCompleted 'virtual-machine'

    if (-not $KeepDisk) {
        $plan += New-ReleaseVmStep -Name 'remove-disk' -Command 'Remove-Item' -Parameters ([ordered]@{
                LiteralPath = $RunPath.DifferencingDisk; Force = $true
            }) -AlwaysRun -RunIfCompleted 'differencing-disk' `
            -Detail 'the run leaves nothing behind but the evidence it copied out'
    }

    return $plan
}

# ---------------------------------------------------------------------------
# The commands the plans name
# ---------------------------------------------------------------------------

function Get-ReleaseVmHostDriverPackage {
    <#
    .SYNOPSIS
        The host's active display driver package directory.
    .DESCRIPTION
        Newest by write time when several are present: a driver update leaves the
        previous package in the store, and copying the older one into the guest
        produces a guest whose driver does not match the host's kernel-mode driver.
    #>
    [OutputType([string])]
    param(
        [string] $Repository,
        [string] $Pattern
    )
    $defaults = Get-ReleaseVmDefault
    if (-not $Repository) { $Repository = $defaults.HostDriverRepository }
    if (-not $Pattern) { $Pattern = $defaults.HostDriverPattern }
    $candidates = @(Get-ChildItem -LiteralPath $Repository -Directory -Filter $Pattern -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending)
    if ($candidates.Count -eq 0) { return $null }
    return $candidates[0].FullName
}

function New-ReleaseVmAnswerIso {
    <#
    .SYNOPSIS
        A one-file ISO carrying autounattend.xml.
    .DESCRIPTION
        Windows Setup looks for an answer file at the root of removable media. A
        second VHDX is not removable and a generation 2 machine has no floppy
        controller, so the answer file travels as its own DVD.

        Built with the image-mastering API that ships with Windows rather than with
        oscdimg, so the recipe does not require the Windows ADK on the host.
    #>
    param(
        [Parameter(Mandatory)] [string] $AnswerFile,
        [Parameter(Mandatory)] [string] $Destination,
        [string] $VolumeName = 'UNATTEND'
    )
    if (-not (Test-Path -LiteralPath $AnswerFile -PathType Leaf)) {
        throw "the answer file '$AnswerFile' does not exist"
    }
    if (-not ('ExoSnap.ReleaseVm.IsoWriter' -as [type])) {
        Add-Type -TypeDefinition @'
namespace ExoSnap.ReleaseVm {
    public static class IsoWriter {
        public static void Write(string path, object stream, int blockSize, int totalBlocks) {
            var source = (System.Runtime.InteropServices.ComTypes.IStream)stream;
            var buffer = new byte[blockSize];
            var read = System.Runtime.InteropServices.Marshal.AllocHGlobal(4);
            try {
                using (var output = System.IO.File.Open(path, System.IO.FileMode.Create)) {
                    while (totalBlocks-- > 0) {
                        source.Read(buffer, blockSize, read);
                        output.Write(buffer, 0, System.Runtime.InteropServices.Marshal.ReadInt32(read));
                    }
                    output.Flush();
                }
            }
            finally {
                System.Runtime.InteropServices.Marshal.FreeHGlobal(read);
            }
        }
    }
}
'@
    }

    # The image is built from a directory, so the answer file gets one of its own:
    # adding tools/vm would put provision.ps1 and this module on the DVD as well.
    $staging = Join-Path ([IO.Path]::GetTempPath()) ('exosnap-answer-' + [guid]::NewGuid().ToString('n'))
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    try {
        Copy-Item -LiteralPath $AnswerFile -Destination (Join-Path $staging 'autounattend.xml') -Force

        $image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
        # 1 ISO9660 | 2 Joliet | 4 UDF. Setup reads ISO9660; Joliet keeps the name
        # readable when the ISO is mounted on the host to check it.
        $image.FileSystemsToCreate = 3
        $image.VolumeName = $VolumeName
        $image.Root.AddTree($staging, $false)

        $result = $image.CreateResultImage()
        $directory = Split-Path -Parent $Destination
        if ($directory -and -not (Test-Path -LiteralPath $directory)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        [ExoSnap.ReleaseVm.IsoWriter]::Write($Destination, $result.ImageStream, $result.BlockSize, $result.TotalBlocks)
    }
    finally {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }
    return $Destination
}

function Stop-ReleaseVmIfRunning {
    <#
    .SYNOPSIS
        Stops a machine that is running, and says nothing about one that is not.
    .DESCRIPTION
        Idempotent so a phase can be re-run: `Stop-VM` on a machine that is already
        off is an error, which would end a resumed build on its first step.
    #>
    param([Parameter(Mandatory)] [string] $VMName)
    $vm = Get-VM -Name $VMName -ErrorAction Stop
    if ($vm.State -eq 'Off') { return }
    Stop-VM -Name $VMName -Force
}

function Set-ReleaseVmBootFromDvd {
    <#
    .SYNOPSIS
        Makes the installation DVD the first boot device.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [string] $IsoPath
    )
    $drive = Get-VMDvdDrive -VMName $VMName | Where-Object { $_.Path -eq $IsoPath } | Select-Object -First 1
    if (-not $drive) { throw "no DVD drive on '$VMName' holds '$IsoPath'" }
    Set-VMFirmware -VMName $VMName -FirstBootDevice $drive
}

function Wait-ReleaseVmPowerShellDirect {
    <#
    .SYNOPSIS
        Blocks until the guest answers on PowerShell Direct.
    .DESCRIPTION
        The only completion signal the host has. A guest that is still installing, or
        that stopped on a screen the answer file did not cover, is indistinguishable
        from a slow one -- so this reports the elapsed time on failure rather than a
        bare timeout, and the developer opens the console to see which of the two it
        is.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [int] $TimeoutMinutes = 15,
        [int] $PollSeconds = 10
    )
    $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
    $started = [DateTime]::UtcNow
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $answer = Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock { $env:COMPUTERNAME } -ErrorAction Stop
            if ($answer) { return $answer }
        }
        catch {
            # Expected while the guest is still installing or still booting: there is
            # no state to distinguish "not yet" from "never" other than the deadline.
        }
        Start-Sleep -Seconds $PollSeconds
    }
    $elapsed = [int]([DateTime]::UtcNow - $started).TotalMinutes
    throw ("'$VMName' did not answer on PowerShell Direct within $elapsed minute(s). " +
           'Open its console: an unattended install that stopped on a prompt looks exactly like a slow one.')
}

function Copy-ReleaseVmFileSet {
    <#
    .SYNOPSIS
        Copies named files into one guest directory.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [string[]] $Path,
        [Parameter(Mandatory)] [string] $Destination
    )
    foreach ($file in $Path) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "'$file' does not exist" }
        Copy-VMFile -Name $VMName -SourcePath $file -DestinationPath (Join-Path $Destination (Split-Path -Leaf $file)) `
            -CreateFullPath -FileSource Host -Force
    }
}

function Copy-ReleaseVmDirectory {
    <#
    .SYNOPSIS
        Copies a host directory tree into the guest, file by file.
    .DESCRIPTION
        Copy-VMFile has no recursive form and no directory form: one call, one file.
        The tree is walked here so the guest keeps the same relative layout, which is
        what lets a harness path computed on the host still be correct in the guest.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [string] $Source,
        [Parameter(Mandatory)] [string] $Destination
    )
    $root = (Resolve-Path -LiteralPath $Source).Path
    $files = @(Get-ChildItem -LiteralPath $root -File -Recurse)
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\')
        Copy-VMFile -Name $VMName -SourcePath $file.FullName -DestinationPath (Join-Path $Destination $relative) `
            -CreateFullPath -FileSource Host -Force
    }
    return $files.Count
}

function Copy-ReleaseVmDriverStore {
    <#
    .SYNOPSIS
        Stages the host display driver into the guest.
    .DESCRIPTION
        Two parts, and the guest needs both: the driver package from the host's
        DriverStore, which ends up under System32\HostDriverStore, and the user-mode
        libraries beside it in System32 (nvEncodeAPI64.dll among them, which is the
        one an NVENC gate fails without).

        Everything lands in an ordinary directory. Copy-VMFile writes as the guest's
        integration service and cannot create files under System32; provision.ps1
        moves them there from inside.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [AllowNull()] [string] $HostDriverPackage,
        [Parameter(Mandatory)] [string] $Destination,
        [string] $System32 = 'C:\Windows\System32',
        [string] $LibraryPattern = 'nv*.dll'
    )
    if ([string]::IsNullOrWhiteSpace($HostDriverPackage) -or -not (Test-Path -LiteralPath $HostDriverPackage)) {
        throw ("no host display driver package was found. Look for one under " +
               "$((Get-ReleaseVmDefault).HostDriverRepository) matching $((Get-ReleaseVmDefault).HostDriverPattern) " +
               'and pass it with -HostDriverPackage.')
    }
    $packageName = Split-Path -Leaf $HostDriverPackage
    $repositoryTarget = Join-Path (Join-Path $Destination 'FileRepository') $packageName
    $copied = Copy-ReleaseVmDirectory -VMName $VMName -Source $HostDriverPackage -Destination $repositoryTarget

    $libraryTarget = Join-Path $Destination 'System32'
    $libraries = @(Get-ChildItem -LiteralPath $System32 -Filter $LibraryPattern -File -ErrorAction SilentlyContinue)
    foreach ($library in $libraries) {
        Copy-VMFile -Name $VMName -SourcePath $library.FullName `
            -DestinationPath (Join-Path $libraryTarget $library.Name) -CreateFullPath -FileSource Host -Force
    }
    return "$copied driver file(s) and $($libraries.Count) library file(s)"
}

function Invoke-ReleaseVmProvisioning {
    <#
    .SYNOPSIS
        Runs provision.ps1 in the guest and reports what it wants next.
    .DESCRIPTION
        Exit code 2 is not a failure: provisioning stages a driver that only appears
        after a restart, and says so. This restarts the guest and runs it again, once.
        A second request to restart is a defect in the recipe, not a state to loop on.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $ProvisionRoot,
        [string[]] $Argument = @()
    )
    $script = Join-Path $ProvisionRoot 'provision.ps1'
    for ($pass = 1; $pass -le 2; $pass++) {
        $code = Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock {
            param($ScriptPath, $Arguments)
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ScriptPath @Arguments | Out-Host
            return $LASTEXITCODE
        } -ArgumentList $script, $Argument

        if ($code -eq 0) { return "provisioning complete after $pass pass(es)" }
        if ($code -ne 2) { throw "provision.ps1 exited $code in '$VMName'" }
        if ($pass -eq 2) { throw "provision.ps1 asked for a second restart in '$VMName'; that is a defect in the recipe" }

        Restart-VM -Name $VMName -Force -Wait:$false
        Start-Sleep -Seconds 10
        Wait-ReleaseVmPowerShellDirect -VMName $VMName -Credential $Credential -TimeoutMinutes 15 | Out-Null
    }
}

function Invoke-ReleaseVmCommand {
    <#
    .SYNOPSIS
        Runs one command line inside the guest and returns its exit code.
    .DESCRIPTION
        The command is a string because its subject is a program built elsewhere. It
        runs through cmd so that a command line written the way a person would type it
        keeps working, and the exit code is returned rather than thrown on: a campaign
        that found defects exits non-zero, and that is a result, not an error.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $Command,
        [string] $WorkingDirectory = 'C:\ExoSnapRun',
        [int] $TimeoutMinutes = 120
    )
    $job = Invoke-Command -VMName $VMName -Credential $Credential -AsJob -ScriptBlock {
        param($CommandLine, $Directory)
        Set-Location -LiteralPath $Directory
        & cmd.exe /c $CommandLine | Out-Host
        return $LASTEXITCODE
    } -ArgumentList $Command, $WorkingDirectory

    if (-not (Wait-Job -Job $job -Timeout ($TimeoutMinutes * 60))) {
        Stop-Job -Job $job
        Remove-Job -Job $job -Force
        throw "the guest command did not finish within $TimeoutMinutes minute(s): $Command"
    }
    $result = Receive-Job -Job $job
    Remove-Job -Job $job -Force
    return $result
}

function Copy-ReleaseVmDirectoryBack {
    <#
    .SYNOPSIS
        Copies a guest directory tree onto the host.
    .DESCRIPTION
        Copy-VMFile only goes host to guest, so the evidence comes back over a
        PowerShell Direct session instead. The session is closed in a finally block:
        a session left open holds the virtual machine, and the next step turns it off.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $Source,
        [Parameter(Mandatory)] [string] $Destination
    )
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }
    $session = New-PSSession -VMName $VMName -Credential $Credential
    try {
        $present = Invoke-Command -Session $session -ScriptBlock {
            param($Path) Test-Path -LiteralPath $Path
        } -ArgumentList $Source
        if (-not $present) {
            throw ("the guest wrote nothing to '$Source'. The campaign either never started or wrote " +
                   'its results somewhere else; there is no run to report on.')
        }
        Copy-Item -FromSession $session -LiteralPath $Source -Destination $Destination -Recurse -Force
    }
    finally {
        Remove-PSSession -Session $session -ErrorAction SilentlyContinue
    }
    return $Destination
}

function New-ReleaseVmCredential {
    <#
    .SYNOPSIS
        The guest's local administrator credential.
    .DESCRIPTION
        The password is a constant of the recipe, in autounattend.xml and here. The
        guest is disposable, local-only and has Remote Desktop off; treating this as a
        secret would mean a prompt in the middle of an unattended run.
    #>
    [OutputType([System.Management.Automation.PSCredential])]
    param(
        [string] $UserName,
        [string] $Password = 'ExoSnapVerify!1'
    )
    if (-not $UserName) { $UserName = (Get-ReleaseVmDefault).GuestUserName }
    return [System.Management.Automation.PSCredential]::new(
        $UserName, (ConvertTo-SecureString $Password -AsPlainText -Force))
}

function Enable-ReleaseVmGuestServices {
    param([Parameter(Mandatory)] [string] $VMName)

    # Integration-service names are localized; the component ID is stable.
    $services = @(Get-VMIntegrationService -VMName $VMName -ErrorAction Stop | Where-Object {
            $_.Id -match '\\6C09BB55-D683-4DA0-8931-C9BF705F6480$'
        })
    if ($services.Count -ne 1) { throw "VM '$VMName' has no unique guest file-transfer integration service" }
    Enable-VMIntegrationService -VMName $VMName -Name $services[0].Name -ErrorAction Stop
}

Export-ModuleMember -Function @(
    'Enable-ReleaseVmGuestServices'
    'Get-ReleaseVmDefault'
    'Get-ReleaseVmPath'
    'Get-ReleaseVmRunPath'
    'Get-ReleaseVmHostState'
    'Get-ReleaseVmHostDriverPackage'
    'Get-ReleaseVmNetworkStep'
    'Test-ReleaseVmPrerequisite'
    'Write-ReleaseVmPrerequisite'
    'New-ReleaseVmStep'
    'New-ReleaseVmCreatePlan'
    'New-ReleaseVmRunPlan'
    'New-ReleaseVmCredential'
    'New-ReleaseVmAnswerIso'
    'Format-ReleaseVmValue'
    'Format-ReleaseVmStep'
    'Write-ReleaseVmPlan'
    'Invoke-ReleaseVmPlan'
    'Set-ReleaseVmBootFromDvd'
    'Stop-ReleaseVmIfRunning'
    'Wait-ReleaseVmPowerShellDirect'
    'Copy-ReleaseVmFileSet'
    'Copy-ReleaseVmDirectory'
    'Copy-ReleaseVmDirectoryBack'
    'Copy-ReleaseVmDriverStore'
    'Invoke-ReleaseVmProvisioning'
    'Invoke-ReleaseVmCommand'
)
