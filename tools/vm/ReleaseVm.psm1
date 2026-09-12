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

# The three-part GPU partition triple, in Hyper-V's own units. What is documented is
# the range: 0 to 1000000000, where the maximum asks for the whole adapter. What it
# means in between is not documented as a proportion of anything, and the driver is
# free to normalise a requested value -- so the configuration a run was measured
# under is the one Get-VMGpuPartitionAdapter reports back afterwards, not the one
# set here. Assert-ReleaseVmGpuPartition is what reads it back.
#
# These values are what a capture-and-encode gate has been run with while the host
# kept rendering the developer's desktop. Raising them has a cost on the host side,
# not a free knob: the partition is reserved for the guest while it runs. An
# encode-heavy soak wants 500000000, and nothing else does.
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
        For an AlwaysRun or Rescue step, the name of the creation step that must have
        completed.
    .PARAMETER Rescue
        Run after the ordinary steps whether or not one of them failed, before any
        step that discards guest state. A campaign step throwing -- a guest timeout is
        the ordinary way -- is exactly when the evidence inside the machine matters,
        and it is also the case that skips every ordinary step after it.
    .PARAMETER DiscardsEvidence
        The step destroys state the evidence may still be in: removing the machine or
        its disk, and also turning it off, which loses whatever the guest had not
        written out. Such a step runs only once every rescue step has succeeded.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Command,
        [System.Collections.IDictionary] $Parameters = @{},
        [string] $Detail = '',
        [switch] $NeedsCredential,
        [switch] $AlwaysRun,
        [switch] $Rescue,
        [switch] $DiscardsEvidence,
        [string] $RunIfCompleted = ''
    )
    if ($Rescue -and $AlwaysRun) {
        throw "step '$Name' cannot be both a rescue and a cleanup step"
    }
    return @{
        Name             = $Name
        Command          = $Command
        Parameters       = $Parameters
        Detail           = $Detail
        NeedsCredential  = [bool]$NeedsCredential
        AlwaysRun        = [bool]$AlwaysRun
        Rescue           = [bool]$Rescue
        DiscardsEvidence = [bool]$DiscardsEvidence
        RunIfCompleted   = $RunIfCompleted
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
        Runs ordinary plan steps to the first failure, rescues the evidence, then
        cleans up.
    .DESCRIPTION
        The only function in this module that changes anything. A step that throws
        stops ordinary execution: those steps are ordered by dependency, and
        continuing past a failed New-VM produces a second page of errors about a
        machine that does not exist. AlwaysRun steps still execute when the resource
        creation step named by RunIfCompleted succeeded.

        Three phases, in this order, because the middle one is the whole point:
        ordinary steps, then rescue steps, then cleanup. A rescue step runs whether or
        not an ordinary step failed, so the evidence inside the machine is taken out
        before anything is allowed to discard it -- a guest timeout throws out of the
        ordinary phase, which is precisely the run whose logs are worth the most. A
        cleanup step marked DiscardsEvidence then runs only if every rescue step
        succeeded; otherwise the machine and its disk are left where they are and the
        result names them, because a resource left behind is recoverable and evidence
        is not.

        Returns what each step returned, keyed by step name. The campaign step's exit
        code is read from there rather than thrown on -- a run that found defects is a
        result and not an infrastructure error. A rescue failure is not in that class:
        a campaign whose evidence never reached the host cannot be reported as a clean
        run, whatever happened inside the guest.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [hashtable[]] $Plan,
        [System.Management.Automation.PSCredential] $Credential
    )
    $outputs = @{}
    $failure = $null
    # Which phase produced the first failure. A campaign that found defects, a
    # campaign whose evidence never left the guest, and a machine that would not go
    # away are three different things for whoever reads the result.
    $failurePhase = ''
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

    $ownerCompleted = {
        param($step)
        return (-not $step.RunIfCompleted) -or $outputs.ContainsKey($step.RunIfCompleted)
    }
    $rescued = $true
    $preserved = @()
    try {
        foreach ($step in @($Plan | Where-Object { -not ($_.AlwaysRun -or $_.Rescue) })) {
            & $runStep $step
        }
    }
    catch {
        $failure = $_
        $failurePhase = 'campaign'
    }
    finally {
        foreach ($step in @($Plan | Where-Object { $_.Rescue })) {
            if (-not (& $ownerCompleted $step)) {
                Write-Host "  $($step.Name) (skipped; '$($step.RunIfCompleted)' did not complete)"
                continue
            }
            try {
                & $runStep $step
            }
            catch {
                # Not fatal to the other rescue steps -- one unreadable directory must
                # not cost the rest -- but it does mean nothing may be discarded.
                $rescued = $false
                if ($null -eq $failure) { $failure = $_; $failurePhase = 'evidence' }
                else { Write-Warning "rescue step '$($step.Name)' failed: $($_.Exception.Message)" }
            }
        }

        foreach ($step in @($Plan | Where-Object { $_.AlwaysRun })) {
            if (-not (& $ownerCompleted $step)) {
                Write-Host "  $($step.Name) (skipped; '$($step.RunIfCompleted)' did not complete)"
                continue
            }
            if ($step.DiscardsEvidence -and -not $rescued) {
                $preserved += "$($step.Name): $(Format-ReleaseVmStep -Step $step)"
                Write-Host "  $($step.Name) (skipped; the evidence did not reach the host)"
                continue
            }
            try {
                & $runStep $step
            }
            catch {
                if ($null -eq $failure) { $failure = $_; $failurePhase = 'cleanup' }
                else { Write-Warning "cleanup step '$($step.Name)' failed: $($_.Exception.Message)" }
            }
        }
    }
    if ($preserved.Count -gt 0) {
        Write-Warning ("the evidence is still inside this run's own resources, so they were kept rather than " +
            "discarded; recover what you need and remove them by hand:`n    " + ($preserved -join "`n    "))
    }
    if ($null -ne $failure) {
        $message = "$failurePhase`: $($failure.Exception.Message)"
        if ($preserved.Count -gt 0) {
            $message += "; the evidence never reached the host, so these steps were not run and their " +
                "resources were kept: " + (($preserved | ForEach-Object { ($_ -split ':')[0] }) -join ', ')
        }
        throw $message
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
        [System.Collections.IDictionary] $Readiness,
        [switch] $RequireInteractiveGuest,
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

    $plan += New-ReleaseVmStep -Name 'gpu-partition-readback' -Command 'Assert-ReleaseVmGpuPartition' `
        -Parameters ([ordered]@{ VMName = $vm; Requested = $partitionParameters }) `
        -Detail 'the values are opaque and the platform may normalise them, so the applied ones are the record'

    $plan += Get-ReleaseVmNetworkStep -Mode $Network -VMName $vm

    $plan += New-ReleaseVmStep -Name 'guest-services' -Command 'Enable-ReleaseVmGuestServices' -Parameters ([ordered]@{
            VMName = $vm
        }) -Detail 'integration settings belong to this new VM, not to its parent disk'

    $plan += New-ReleaseVmStep -Name 'start' -Command 'Start-VM' -Parameters ([ordered]@{ Name = $vm })

    $plan += New-ReleaseVmStep -Name 'wait-for-guest' -Command 'Wait-ReleaseVmPowerShellDirect' -Parameters ([ordered]@{
            VMName = $vm; TimeoutMinutes = $BootTimeoutMinutes
        }) -NeedsCredential

    if ($RequireInteractiveGuest -or $Readiness) {
        if (-not $Readiness) { $Readiness = New-ReleaseVmReadinessRequirement -InteractiveAgent }
        $plan += New-ReleaseVmStep -Name 'guest-readiness' -Command 'Assert-ReleaseVmReadiness' `
            -Parameters ([ordered]@{ VMName = $vm; Requirement = $Readiness }) -NeedsCredential `
            -Detail 'measured in the guest, because PowerShell Direct answering proves only that the OS is up'
    }

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
        }) -NeedsCredential -Rescue -RunIfCompleted 'virtual-machine' `
        -Detail 'PowerShell Direct in the other direction: Copy-VMFile is host-to-guest only'

    $plan += New-ReleaseVmStep -Name 'stop' -Command 'Stop-VM' -Parameters ([ordered]@{
            Name = $vm; TurnOff = $true; Force = $true
        }) -AlwaysRun -RunIfCompleted 'virtual-machine' -DiscardsEvidence `
        -Detail 'turned off, not shut down: by here the evidence is already on the host'

    $plan += New-ReleaseVmStep -Name 'remove-vm' -Command 'Remove-VM' -Parameters ([ordered]@{
            Name = $vm; Force = $true
        }) -AlwaysRun -RunIfCompleted 'virtual-machine' -DiscardsEvidence

    if (-not $KeepDisk) {
        $plan += New-ReleaseVmStep -Name 'remove-disk' -Command 'Remove-Item' -Parameters ([ordered]@{
                LiteralPath = $RunPath.DifferencingDisk; Force = $true
            }) -AlwaysRun -RunIfCompleted 'differencing-disk' -DiscardsEvidence `
            -Detail 'the run leaves nothing behind but the evidence it copied out'
    }

    return $plan
}

# ---------------------------------------------------------------------------
# The commands the plans name
# ---------------------------------------------------------------------------

function New-ReleaseVmReadinessRequirement {
    <#
    .SYNOPSIS
        What one run needs a guest to have proven before its campaign starts.
    .DESCRIPTION
        Stated per run rather than assumed for every run. An install-and-uninstall
        scenario needs a reachable OS and nothing else; a capture scenario needs an
        interactive desktop and a display in a particular mode, and a harness that
        demanded one of the first would refuse runs the scenario does not need.
    .PARAMETER InteractiveAgent
        The process that will run the campaign must be in the interactive session
        that is attached to the console, on WinSta0\Default. PowerShell Direct lands
        in session 0, which owns no desktop: no monitor enumerates there, Graphics
        Capture offers only windows, and Output Duplication finds no outputs.
    .PARAMETER ExpectedUser
        The account the campaign must run as. The token decides what a capture is
        allowed to see, so a run that silently became SYSTEM is a different test.
    .PARAMETER Display
        Width, Height and RefreshHz that at least one attached display must report.
    .PARAMETER ControlChannel
        The product control channel must answer from inside the guest.
    .PARAMETER GpuBoundTo
        The host GPU identity this run partitioned. The guest adapter is held against
        it on what a partition preserves -- vendor and device ids and the driver
        version -- so a guest that quietly fell back to the Basic Render Driver stops
        the run instead of producing evidence attributed to the wrong GPU.
    #>
    [OutputType([hashtable])]
    param(
        [switch] $InteractiveAgent,
        [string] $ExpectedUser = '',
        [System.Collections.IDictionary] $Display,
        [System.Collections.IDictionary] $GpuBoundTo,
        [switch] $ControlChannel
    )
    return @{
        InteractiveAgent = [bool]$InteractiveAgent
        ExpectedUser     = $ExpectedUser
        Display          = $Display
        GpuBoundTo       = $GpuBoundTo
        ControlChannel   = [bool]$ControlChannel
    }
}

function Test-ReleaseVmReadiness {
    <#
    .SYNOPSIS
        Whether a measured guest meets a run's requirement, and what it does not meet.
    .DESCRIPTION
        Pure: it reads a receipt and a requirement and decides. The measuring is
        Get-ReleaseVmReadiness, which needs a guest; the deciding is here, where it
        can be held to its cases.

        Two rules keep it honest. A requirement the receipt carries no measurement for
        is unmet -- an older guest agent simply does not write a field this build asks
        about, and the absence of a measurement is not evidence that the state is
        good. And every unmet requirement is named at once: a campaign that takes an
        hour to reach this point cannot be debugged one round trip at a time.
    .OUTPUTS
        A hashtable with Ready and Unmet.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Receipt,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requirement
    )
    $unmet = @()
    $measured = {
        param($key)
        return $Receipt.Contains($key) -and $null -ne $Receipt[$key]
    }

    if (-not (& $measured 'osReachable')) {
        return @{ Ready = $false; Unmet = @('whether the guest is reachable was not measured') }
    }
    if (-not $Receipt['osReachable']) {
        # Nothing measured through a guest that never answered means anything, so the
        # rest of the receipt is not read at all.
        return @{ Ready = $false; Unmet = @('the guest did not answer') }
    }

    if ($Requirement.InteractiveAgent) {
        foreach ($key in @('agentSessionId', 'consoleSessionId', 'agentWindowStation', 'agentDesktop')) {
            if (-not (& $measured $key)) { $unmet += "$key was not measured" }
        }
        if ((& $measured 'agentSessionId') -and [int]$Receipt['agentSessionId'] -eq 0) {
            $unmet += 'the agent runs in session 0, which owns no desktop, so nothing can be captured from it'
        }
        elseif ((& $measured 'agentSessionId') -and (& $measured 'consoleSessionId') -and
                [int]$Receipt['agentSessionId'] -ne [int]$Receipt['consoleSessionId']) {
            $unmet += ("the agent runs in session $($Receipt['agentSessionId']), which is not the console " +
                "session $($Receipt['consoleSessionId']); a disconnected session enumerates no display")
        }
        if ((& $measured 'agentWindowStation') -and (& $measured 'agentDesktop') -and
            -not ($Receipt['agentWindowStation'] -eq 'WinSta0' -and $Receipt['agentDesktop'] -eq 'Default')) {
            $unmet += ("the agent is on $($Receipt['agentWindowStation'])\$($Receipt['agentDesktop']), " +
                'not WinSta0\Default')
        }
    }

    if ($Requirement.ExpectedUser) {
        if (-not (& $measured 'agentUser')) { $unmet += 'the account the agent runs as was not measured' }
        elseif ($Receipt['agentUser'] -ne $Requirement.ExpectedUser) {
            $unmet += "the agent runs as $($Receipt['agentUser']), not $($Requirement.ExpectedUser)"
        }
    }

    if ($Requirement.Display) {
        $wanted = "$($Requirement.Display.Width)x$($Requirement.Display.Height)@$($Requirement.Display.RefreshHz)Hz"
        if (-not (& $measured 'displays')) {
            $unmet += "the attached displays were not measured, so $wanted is unproven"
        }
        else {
            $displays = @($Receipt['displays'])
            if ($displays.Count -eq 0) {
                $unmet += "no display is attached, so $wanted cannot be shown"
            }
            else {
                $match = @($displays | Where-Object {
                        [int]$_.Width -eq [int]$Requirement.Display.Width -and
                        [int]$_.Height -eq [int]$Requirement.Display.Height -and
                        [int]$_.RefreshHz -eq [int]$Requirement.Display.RefreshHz
                    })
                if ($match.Count -eq 0) {
                    $found = ($displays | ForEach-Object { "$($_.Width)x$($_.Height)@$($_.RefreshHz)Hz" }) -join ', '
                    $unmet += "no attached display is $wanted; the guest has $found"
                }
            }
        }
    }

    if ($Requirement.GpuBoundTo) {
        if (-not (& $measured 'gpu')) {
            $unmet += 'the guest display adapter was not measured, so the GPU binding is unproven'
        }
        else {
            $binding = Test-ReleaseVmGpuBinding -HostGpu $Requirement.GpuBoundTo -GuestGpu $Receipt['gpu']
            if (-not $binding.Bound) { $unmet += $binding.Unmet }
        }
    }

    if ($Requirement.ControlChannel) {
        if (-not (& $measured 'controlChannel')) { $unmet += 'the control channel was not measured' }
        elseif (-not $Receipt['controlChannel']) { $unmet += 'the control channel did not answer inside the guest' }
    }

    return @{ Ready = ($unmet.Count -eq 0); Unmet = $unmet }
}

function Get-ReleaseVmReadiness {
    <#
    .SYNOPSIS
        Measures, inside the guest, the state a capture campaign depends on.
    .DESCRIPTION
        The measurement runs the way the campaign will, because that is the only way
        the session it reports is the session the campaign gets. A receipt taken over
        a different channel would describe a different process.

        Fields the guest cannot answer for are left out rather than guessed at;
        Test-ReleaseVmReadiness treats an absent field as unmet.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential
    )
    $measure = {
        $receipt = @{ osReachable = $true }
        $receipt['agentSessionId'] = (Get-Process -Id $PID).SessionId
        $receipt['agentUser'] = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

        if (-not ('ExoSnap.Readiness.Native' -as [type])) {
            Add-Type -Namespace 'ExoSnap.Readiness' -Name 'Native' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint WTSGetActiveConsoleSessionId();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetProcessWindowStation();
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr GetThreadDesktop(uint dwThreadId);
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint GetCurrentThreadId();
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool GetUserObjectInformationW(
    System.IntPtr hObj, int nIndex, System.Text.StringBuilder pvInfo, uint nLength, out uint lpnLengthNeeded);
'@
        }
        $name = {
            param($handle)
            $buffer = New-Object System.Text.StringBuilder 256
            $needed = 0
            # UOI_NAME = 2.
            if ([ExoSnap.Readiness.Native]::GetUserObjectInformationW($handle, 2, $buffer, 256, [ref]$needed)) {
                return $buffer.ToString()
            }
            return $null
        }
        $receipt['consoleSessionId'] = [int][ExoSnap.Readiness.Native]::WTSGetActiveConsoleSessionId()
        $receipt['agentWindowStation'] = & $name ([ExoSnap.Readiness.Native]::GetProcessWindowStation())
        $receipt['agentDesktop'] = & $name (
            [ExoSnap.Readiness.Native]::GetThreadDesktop([ExoSnap.Readiness.Native]::GetCurrentThreadId()))

        $interactive = @(Get-Process -Name 'explorer' -ErrorAction SilentlyContinue |
                Where-Object { $_.SessionId -ne 0 } | Select-Object -First 1)
        if ($interactive.Count -gt 0) { $receipt['interactiveSessionId'] = $interactive[0].SessionId }

        $adapter = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
                Where-Object { $_.PNPDeviceID -like 'PCI\*' } | Select-Object -First 1)
        if ($adapter.Count -gt 0) {
            $ids = [regex]::Match($adapter[0].PNPDeviceID, 'VEN_(?<vendor>[0-9A-F]{4})&DEV_(?<device>[0-9A-F]{4})')
            $gpu = @{ Name = $adapter[0].Name; DriverVersion = $adapter[0].DriverVersion }
            if ($ids.Success) {
                $gpu['VendorId'] = $ids.Groups['vendor'].Value
                $gpu['DeviceId'] = $ids.Groups['device'].Value
            }
            $receipt['gpu'] = $gpu
        }

        # WMI rather than the display APIs: this may be running in a session that has
        # no desktop at all, which is exactly the case being reported, and the display
        # APIs answer that with a failure rather than an empty list.
        $receipt['displays'] = @(
            Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
                Where-Object { $_.CurrentHorizontalResolution } |
                ForEach-Object {
                    @{
                        Name      = $_.Name
                        Width     = [int]$_.CurrentHorizontalResolution
                        Height    = [int]$_.CurrentVerticalResolution
                        RefreshHz = [int]$_.CurrentRefreshRate
                    }
                })
        return $receipt
    }
    try {
        return Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock $measure -ErrorAction Stop
    }
    catch {
        return @{ osReachable = $false; detail = $_.Exception.Message }
    }
}

function Assert-ReleaseVmReadiness {
    <#
    .SYNOPSIS
        Measures a guest and throws unless it meets the run requirement.
    .DESCRIPTION
        The plan step form. Throwing is right here: a campaign started on a guest that
        cannot show a picture produces a failure that reads like a product defect, and
        the run is stopped before it can.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requirement
    )
    $receipt = Get-ReleaseVmReadiness -VMName $VMName -Credential $Credential
    $verdict = Test-ReleaseVmReadiness -Receipt $receipt -Requirement $Requirement
    if (-not $verdict.Ready) {
        throw ("'$VMName' is not ready for this run:`n    " + ($verdict.Unmet -join "`n    "))
    }
    return $receipt
}

function Get-ReleaseVmInfDriverVersion {
    <#
    .SYNOPSIS
        The version an INF declares, or null when it declares none.
    .DESCRIPTION
        The DriverVer directive is "DriverVer = <date>,<version>". The version is what
        identifies a DriverStore package against the driver the adapter is running;
        the date is not unique across packages.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [AllowEmptyString()] [string] $InfText)
    $match = [regex]::Match($InfText, '(?im)^\s*DriverVer\s*=\s*[^,]+,\s*([0-9][0-9.]*)\s*$')
    if (-not $match.Success) { return $null }
    return $match.Groups[1].Value
}

function Select-ReleaseVmDriverPackage {
    <#
    .SYNOPSIS
        The DriverStore package that is the driver the host is running.
    .DESCRIPTION
        Matched by version, not by write time. A driver update leaves the previous
        package in the store and a rollback leaves the newer one, so the newest
        directory is not the active driver -- and staging the wrong package produces a
        guest whose user-mode driver does not match the host kernel-mode driver, which
        fails inside the guest as an unexplained device error.

        Refuses rather than approximates. No match and an ambiguous match are both
        reported with what was looked for and what the store holds, because staging a
        driver that is not the host driver is worse than staging none.
    .OUTPUTS
        A hashtable with Ok, Package and Detail.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $Candidate,
        [Parameter(Mandatory)] [string] $ActiveDriverVersion
    )
    if ($Candidate.Count -eq 0) {
        return @{ Ok = $false; Package = $null
            Detail = "no driver package is in the store to match against $ActiveDriverVersion" }
    }

    $matches = @($Candidate | Where-Object { $_.DriverVersion -eq $ActiveDriverVersion })
    if ($matches.Count -eq 1) {
        return @{ Ok = $true; Package = $matches[0]
            Detail = "$($matches[0].Name) declares $ActiveDriverVersion, which is what the adapter is running" }
    }
    if ($matches.Count -gt 1) {
        return @{ Ok = $false; Package = $null
            Detail = ("$($matches.Count) driver packages declare $ActiveDriverVersion and nothing here can " +
                "tell them apart: " + (($matches | ForEach-Object { $_.Name }) -join ', ')) }
    }

    $held = ($Candidate | ForEach-Object { "$($_.Name) ($($_.DriverVersion))" }) -join ', '
    return @{ Ok = $false; Package = $null
        Detail = "no driver package declares $ActiveDriverVersion, which is what the adapter is running; the store holds $held" }
}

function Compare-ReleaseVmGpuPartition {
    <#
    .SYNOPSIS
        What the adapter actually applied, against what the run asked for.
    .DESCRIPTION
        Pure. The partition values are opaque to this recipe and the platform may
        normalise them, so the configuration a run was measured under is the one read
        back afterwards. A field the adapter did not report is a difference, not
        agreement: nobody measured it.
    .OUTPUTS
        A hashtable with Matches and Differences.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requested,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Actual
    )
    $differences = @()
    foreach ($key in $Requested.Keys) {
        if (-not $Actual.Contains($key) -or $null -eq $Actual[$key]) {
            $differences += "$key`: requested $($Requested[$key]), not reported by the adapter"
            continue
        }
        if ([long]$Actual[$key] -ne [long]$Requested[$key]) {
            $differences += "$key`: requested $($Requested[$key]), actual $($Actual[$key])"
        }
    }
    return @{ Matches = ($differences.Count -eq 0); Differences = $differences }
}

function Test-ReleaseVmGpuBinding {
    <#
    .SYNOPSIS
        Whether the adapter the guest sees is the host GPU this run partitioned.
    .DESCRIPTION
        Compared on what a GPU partition preserves and nothing else: the PCI vendor
        and device ids, and the driver version, which in the guest comes from the host
        driver package staged into it. A mismatched driver version is the classic
        GPU-P failure and the reason the package selection above matches on the active
        driver rather than on a timestamp.

        The adapter LUID is deliberately not compared. It identifies an adapter within
        one operating system, the guest assigns its own, and requiring them to agree
        would fail every correct run. An unmeasured fact is unbound rather than
        assumed: a guest that reported no adapter proves nothing.
    .OUTPUTS
        A hashtable with Bound and Unmet.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $HostGpu,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $GuestGpu
    )
    $unmet = @()
    foreach ($field in @('VendorId', 'DeviceId', 'DriverVersion')) {
        $name = $field.Substring(0, 1).ToLowerInvariant() + $field.Substring(1)
        $hasHost = $HostGpu.Contains($field) -and $null -ne $HostGpu[$field]
        $hasGuest = $GuestGpu.Contains($field) -and $null -ne $GuestGpu[$field]
        if (-not $hasHost) { $unmet += "the host $name was not measured"; continue }
        if (-not $hasGuest) { $unmet += "the guest $name was not measured"; continue }
        if ("$($HostGpu[$field])" -ne "$($GuestGpu[$field])") {
            $unmet += "$name`: host $($HostGpu[$field]), guest $($GuestGpu[$field])"
        }
    }
    return @{ Bound = ($unmet.Count -eq 0); Unmet = $unmet }
}

function Get-ReleaseVmHostGpu {
    <#
    .SYNOPSIS
        The identity of the display adapter a run partitions.
    .DESCRIPTION
        Enough to say afterwards which physical GPU a campaign ran on: the PnP
        instance path, the adapter LUID, the PCI ids, and the driver the adapter is
        actually running together with the DriverStore package that driver came from.

        The LUID is recorded for the host record, not for comparison against the
        guest: it identifies an adapter within one operating system.
    #>
    [OutputType([hashtable])]
    param([string] $InstancePathPattern = 'PCI\\VEN_10DE*')
    $device = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
            Where-Object { $_.PNPDeviceID -like $InstancePathPattern } | Select-Object -First 1)
    if ($device.Count -eq 0) { return @{ Measured = $false; Detail = "no display adapter matches $InstancePathPattern" } }
    $adapter = $device[0]

    $identity = @{
        Measured      = $true
        InstancePath  = $adapter.PNPDeviceID
        Name          = $adapter.Name
        DriverVersion = $adapter.DriverVersion
    }
    $ids = [regex]::Match($adapter.PNPDeviceID, 'VEN_(?<vendor>[0-9A-F]{4})&DEV_(?<device>[0-9A-F]{4})(&SUBSYS_(?<subsys>[0-9A-F]{8}))?')
    if ($ids.Success) {
        $identity['VendorId'] = $ids.Groups['vendor'].Value
        $identity['DeviceId'] = $ids.Groups['device'].Value
        if ($ids.Groups['subsys'].Success) { $identity['SubsystemId'] = $ids.Groups['subsys'].Value }
    }

    $signed = @(Get-CimInstance -ClassName Win32_PnPSignedDriver -ErrorAction SilentlyContinue |
            Where-Object { $_.DeviceID -eq $adapter.PNPDeviceID } | Select-Object -First 1)
    if ($signed.Count -gt 0) {
        $identity['InfName'] = $signed[0].InfName
        if ($signed[0].DriverVersion) { $identity['DriverVersion'] = $signed[0].DriverVersion }
    }

    $luid = @(Get-PnpDeviceProperty -InstanceId $adapter.PNPDeviceID -KeyName 'DEVPKEY_Device_LUID' -ErrorAction SilentlyContinue)
    if ($luid.Count -gt 0 -and $null -ne $luid[0].Data) { $identity['AdapterLuid'] = "$($luid[0].Data)" }

    return $identity
}

function Assert-ReleaseVmGpuPartition {
    <#
    .SYNOPSIS
        Reads the applied partition back and throws unless it is what was requested.
    .DESCRIPTION
        The plan step form. A run whose partition the platform quietly normalised was
        measured under a configuration nobody recorded, so the difference is named and
        the run stops rather than producing evidence attributed to the wrong setup.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requested
    )
    $adapter = @(Get-VMGpuPartitionAdapter -VMName $VMName -ErrorAction Stop)
    if ($adapter.Count -eq 0) { throw "'$VMName' has no GPU partition adapter to read back" }

    $actual = [ordered]@{}
    foreach ($key in $Requested.Keys) {
        $property = $adapter[0].PSObject.Properties[$key]
        if ($property -and $null -ne $property.Value) { $actual[$key] = $property.Value }
    }

    $comparison = Compare-ReleaseVmGpuPartition -Requested $Requested -Actual $actual
    if (-not $comparison.Matches) {
        throw ("the GPU partition on '$VMName' is not what this run asked for:`n    " +
            ($comparison.Differences -join "`n    "))
    }
    return $actual
}

function Get-ReleaseVmHostDriverPackage {
    <#
    .SYNOPSIS
        The host's active display driver package directory.
    .DESCRIPTION
        The package whose INF declares the version the adapter is actually running.
        Not the newest directory by write time: an update leaves the previous package
        in the store and a rollback leaves the newer one, so the clock says nothing
        about which driver is loaded, and staging the wrong one produces a guest whose
        user-mode driver does not match the host kernel-mode driver.

        Returns null when there is no unambiguous match. Copy-ReleaseVmDriverStore
        refuses on null rather than staging something approximate.
    #>
    [OutputType([string])]
    param(
        [string] $Repository,
        [string] $Pattern,
        [string] $ActiveDriverVersion
    )
    $defaults = Get-ReleaseVmDefault
    if (-not $Repository) { $Repository = $defaults.HostDriverRepository }
    if (-not $Pattern) { $Pattern = $defaults.HostDriverPattern }
    if (-not $ActiveDriverVersion) {
        $gpu = Get-ReleaseVmHostGpu
        if (-not $gpu.Measured) { return $null }
        $ActiveDriverVersion = $gpu.DriverVersion
    }

    $candidates = @(Get-ChildItem -LiteralPath $Repository -Directory -Filter $Pattern -ErrorAction SilentlyContinue |
            ForEach-Object {
                $inf = @(Get-ChildItem -LiteralPath $_.FullName -Filter '*.inf' -File -ErrorAction SilentlyContinue |
                        Select-Object -First 1)
                @{
                    Name          = $_.Name
                    FullName      = $_.FullName
                    DriverVersion = if ($inf.Count -gt 0) {
                        Get-ReleaseVmInfDriverVersion -InfText (Get-Content -LiteralPath $inf[0].FullName -Raw)
                    } else { $null }
                }
            })

    $selected = Select-ReleaseVmDriverPackage -Candidate $candidates -ActiveDriverVersion $ActiveDriverVersion
    if (-not $selected.Ok) {
        Write-Warning "no host display driver package could be bound: $($selected.Detail)"
        return $null
    }
    return $selected.Package.FullName
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
    'Get-ReleaseVmInfDriverVersion'
    'Select-ReleaseVmDriverPackage'
    'Compare-ReleaseVmGpuPartition'
    'Test-ReleaseVmGpuBinding'
    'Get-ReleaseVmHostGpu'
    'Assert-ReleaseVmGpuPartition'
    'New-ReleaseVmReadinessRequirement'
    'Test-ReleaseVmReadiness'
    'Get-ReleaseVmReadiness'
    'Assert-ReleaseVmReadiness'
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
