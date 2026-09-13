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

# The display-path measurement, as source, because it runs in two places that have
# nothing else in common: inside the interactive agent's script, and inside the
# scriptblock Get-ReleaseVmReadiness sends over PowerShell Direct. Neither can call
# into this module, so what they share has to be text.
#
# EnumDisplayDevices and EnumDisplaySettings read the display device database rather
# than the calling session's desktop, which is why they answer from session 0 -- the
# case that ruled out the display APIs here before and left the measurement on
# Win32_VideoController, which reports per adapter and cannot say which mode a
# display path is actually in.
$script:DisplayPathNative = @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential,
    CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public struct DISPLAY_DEVICEW {
    public int cb;
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public int StateFlags;
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
}
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential,
    CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public struct DEVMODEW {
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public ushort dmSpecVersion;
    public ushort dmDriverVersion;
    public ushort dmSize;
    public ushort dmDriverExtra;
    public uint dmFields;
    public int dmPositionX;
    public int dmPositionY;
    public uint dmDisplayOrientation;
    public uint dmDisplayFixedOutput;
    public short dmColor;
    public short dmDuplex;
    public short dmYResolution;
    public short dmTTOption;
    public short dmCollate;
    [System.Runtime.InteropServices.MarshalAs(
        System.Runtime.InteropServices.UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public ushort dmLogPixels;
    public uint dmBitsPerPel;
    public uint dmPelsWidth;
    public uint dmPelsHeight;
    public uint dmDisplayFlags;
    public uint dmDisplayFrequency;
    public uint dmICMMethod;
    public uint dmICMIntent;
    public uint dmMediaType;
    public uint dmDitherType;
    public uint dmReserved1;
    public uint dmReserved2;
    public uint dmPanningWidth;
    public uint dmPanningHeight;
}
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool EnumDisplayDevicesW(
    string lpDevice, uint iDevNum, ref DISPLAY_DEVICEW lpDisplayDevice, uint dwFlags);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool EnumDisplaySettingsW(string lpszDeviceName, int iModeNum, ref DEVMODEW lpDevMode);
'@

# Written as a scriptblock literal so both callers can define it as $paths and run it
# with & $paths. $Native is the type each caller declared the members above on.
$script:DisplayPathScript = @'
$paths = {
    $displayDevice = $Native.GetNestedType('DISPLAY_DEVICEW')
    $displayMode = $Native.GetNestedType('DEVMODEW')
    $new = {
        param($type)
        $value = [Activator]::CreateInstance($type)
        return $value
    }
    $found = @()
    for ($index = 0; $index -lt 64; $index++) {
        $adapter = & $new $displayDevice
        $adapter.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($adapter)
        # [NullString]::Value, not $null: PowerShell marshals $null to a string
        # parameter as an empty string, and EnumDisplayDevices("") enumerates nothing.
        if (-not $Native::EnumDisplayDevicesW([NullString]::Value, $index, [ref] $adapter, 0)) { break }
        # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP. A path that shows nothing has no mode,
        # and a requirement met by a detached path would be met by nothing visible.
        if (($adapter.StateFlags -band 0x1) -eq 0) { continue }

        $mode = & $new $displayMode
        $mode.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mode)
        # ENUM_CURRENT_SETTINGS: the mode this path is in now, not one it offers.
        if (-not $Native::EnumDisplaySettingsW($adapter.DeviceName, -1, [ref] $mode)) { continue }

        $monitor = & $new $displayDevice
        $monitor.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($monitor)
        $monitorName = if ($Native::EnumDisplayDevicesW($adapter.DeviceName, 0, [ref] $monitor, 0)) {
            $monitor.DeviceString
        } else { $null }

        $found += @{
            Device    = $adapter.DeviceName
            Adapter   = $adapter.DeviceString
            Monitor   = $monitorName
            Width     = [int] $mode.dmPelsWidth
            Height    = [int] $mode.dmPelsHeight
            RefreshHz = [int] $mode.dmDisplayFrequency
            # DISPLAY_DEVICE_PRIMARY_DEVICE.
            Primary   = (($adapter.StateFlags -band 0x4) -ne 0)
        }
    }
    # Typed, not comma-wrapped: every caller reads this through @(), which would
    # otherwise count the wrapper rather than the paths.
    return [object[]] $found
}
'@

# Well-known SID of the local "Hyper-V Administrators" group. Matched by SID rather
# than by name because the name is localized and this machine is not en-US throughout.
$script:HyperVAdministratorsSid = 'S-1-5-32-578'

$script:RunIdPattern = '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$'

function Get-ReleaseVmDefault {
    <#
    .SYNOPSIS
        The recipe's fixed values, in one place both host scripts read.
    .DESCRIPTION
        Everything here is a property of the recipe except the image root, which is a
        property of a machine: where a host has room for a 60 GB image is not
        something a repository can know. EXOSNAP_VM_ROOT names it, because callers
        that compose this recipe -- the typed transport among them -- pass a campaign
        and a guest command, not a storage layout.
    #>
    [OutputType([hashtable])]
    param()
    $root = $env:EXOSNAP_VM_ROOT
    if ([string]::IsNullOrWhiteSpace($root)) { $root = 'D:\exosnap-vm' }
    return @{
        Root             = $root
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
        # Matched with -like, which has no escape character: its only metacharacters
        # are * ? and [ ]. A doubled backslash here would demand two of them in the
        # instance path and match no adapter that exists.
        HostGpuInstancePattern = 'PCI\VEN_10DE*'
        # The root-enumerated device each virtual display profile binds to, which is
        # how a guest can be asked which profile it actually runs rather than told.
        # A profile is absent here until its device identity has been recorded the way
        # every package in provision-manifest.psd1 is pinned; 'sudovda' is absent on
        # purpose, because the image that would carry it has not been built and
        # inventing an identity would let a run claim a qualification nobody measured.
        DisplayProfileHardwareId = @{ mtt = 'Root\MttVDD' }
        # Microsoft's PCI vendor id. A GPU-P guest binds to the paravirtual device,
        # which reports this rather than the vendor whose silicon is being
        # partitioned; the vendor's kernel-mode driver never leaves the host.
        ParavirtualVendorId = '1414'
        # Where the host's driver package lands inside the guest. Copied rather than
        # installed, so the guest can be asked which package it actually holds.
        GuestDriverRepository = 'C:\Windows\System32\HostDriverStore\FileRepository'
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
        # What this image is, beside the image. A campaign whose evidence cannot name
        # the image it came from is a campaign nobody can repeat.
        Fingerprint   = [IO.Path]::Combine($Root, 'image-fingerprint.json')
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
            -Detail 'partition units out of a documented range of 1000000000; the applied values are what the read-back reports'
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
        [System.Collections.IDictionary] $RequireImageFingerprint,
        [switch] $RequireInteractiveGuest,
        [switch] $KeepDisk
    )
    if (-not $GpuPartition) { $GpuPartition = $script:GpuPartitionDefault }
    $vm = $RunPath.VMName
    $plan = @()

    if ($RequireImageFingerprint) {
        # First, before anything is created: an image that is not the one this run was
        # qualified on makes every later step a waste, and refusing here costs nothing.
        $plan += New-ReleaseVmStep -Name 'image-fingerprint' -Command 'Assert-ReleaseVmImageFingerprint' `
            -Parameters ([ordered]@{
                Expected = $RequireImageFingerprint
                FingerprintPath = [IO.Path]::Combine((Split-Path -Parent $RunPath.GoldenDisk), 'image-fingerprint.json')
            }) -Detail 'the two display-driver profiles behave differently under capture'
    }

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

    # The partition values alone, not the cmdlet's parameter set: the read-back reads
    # one adapter property per requested key and compares both sides as numbers, and
    # -VMName is neither a property of the partition nor a number.
    $plan += New-ReleaseVmStep -Name 'gpu-partition-readback' -Command 'Assert-ReleaseVmGpuPartition' `
        -Parameters ([ordered]@{ VMName = $vm; Requested = $GpuPartition }) `
        -Detail 'the values are opaque and the platform may normalise them, so the applied ones are the record'

    $plan += Get-ReleaseVmNetworkStep -Mode $Network -VMName $vm

    $plan += New-ReleaseVmStep -Name 'guest-services' -Command 'Enable-ReleaseVmGuestServices' -Parameters ([ordered]@{
            VMName = $vm
        }) -Detail 'integration settings belong to this new VM, not to its parent disk'

    $plan += New-ReleaseVmStep -Name 'start' -Command 'Start-VM' -Parameters ([ordered]@{ Name = $vm })

    $plan += New-ReleaseVmStep -Name 'wait-for-guest' -Command 'Wait-ReleaseVmPowerShellDirect' -Parameters ([ordered]@{
            VMName = $vm; TimeoutMinutes = $BootTimeoutMinutes
        }) -NeedsCredential

    $agentRoot = [IO.Path]::Combine($RunPath.GuestRoot, 'agent')
    if ($RequireInteractiveGuest -or $Readiness) {
        if (-not $Readiness) { $Readiness = New-ReleaseVmReadinessRequirement -InteractiveAgent }

        $plan += New-ReleaseVmStep -Name 'start-agent' -Command 'Start-ReleaseVmGuestAgent' `
            -Parameters ([ordered]@{ VMName = $vm; AgentRoot = $agentRoot }) -NeedsCredential `
            -Detail 'started over PowerShell Direct into the interactive session, which PowerShell Direct is not'

        $plan += New-ReleaseVmStep -Name 'guest-readiness' -Command 'Assert-ReleaseVmAgentHandshake' `
            -Parameters ([ordered]@{ VMName = $vm; AgentRoot = $agentRoot; Requirement = $Readiness }) `
            -NeedsCredential `
            -Detail 'the agent proves its own context; a receipt from PowerShell Direct describes the wrong session'
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

    if ($RequireInteractiveGuest -or $Readiness) {
        $plan += New-ReleaseVmStep -Name 'run' -Command 'Invoke-ReleaseVmInteractiveCommand' -Parameters ([ordered]@{
                VMName = $vm
                Command = $GuestCommand
                AgentRoot = $agentRoot
                WorkingDirectory = $RunPath.GuestRoot
                TimeoutMinutes = $RunTimeoutMinutes
            }) -NeedsCredential -Detail 'through the agent that proved its context, because the app needs a desktop'
    }
    else {
        $plan += New-ReleaseVmStep -Name 'run' -Command 'Invoke-ReleaseVmCommand' -Parameters ([ordered]@{
                VMName = $vm
                Command = $GuestCommand
                WorkingDirectory = $RunPath.GuestRoot
                TimeoutMinutes = $RunTimeoutMinutes
            }) -NeedsCredential -Detail 'the campaign itself, inside the guest'
    }

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
        # One attached display path has to be in this mode, and the resolution and the
        # refresh rate have to be that one path's. Asked per adapter, as it was, a
        # guest whose synthetic display runs 1024x768 and whose virtual monitor runs
        # 60 Hz answers yes to 2560x1440@144, because two adapters each report a mode
        # that no display path is actually in.
        if (-not (& $measured 'displayPaths')) {
            $unmet += "the attached display paths were not measured, so $wanted is unproven"
        }
        else {
            $displays = @($Receipt['displayPaths'])
            if ($displays.Count -eq 0) {
                $unmet += "no display path is attached, so $wanted cannot be shown"
            }
            else {
                $match = @($displays | Where-Object {
                        [int]$_.Width -eq [int]$Requirement.Display.Width -and
                        [int]$_.Height -eq [int]$Requirement.Display.Height -and
                        [int]$_.RefreshHz -eq [int]$Requirement.Display.RefreshHz
                    })
                if ($match.Count -eq 0) {
                    $found = ($displays | ForEach-Object {
                            "$($_.Device) ($($_.Adapter)) $($_.Width)x$($_.Height)@$($_.RefreshHz)Hz"
                        }) -join '; '
                    $unmet += "no attached display path is $wanted; the guest has $found"
                }
            }
        }
    }

    if ($Requirement.GpuBoundTo) {
        if (-not (& $measured 'gpu')) {
            $unmet += 'the guest display adapter was not measured, so the GPU binding is unproven'
        }
        else {
            $store = if ($Receipt.Contains('hostDriverStore') -and $null -ne $Receipt['hostDriverStore']) {
                $Receipt['hostDriverStore']
            } else { @{} }
            $binding = Test-ReleaseVmGpuBinding -HostGpu $Requirement.GpuBoundTo -GuestGpu $Receipt['gpu'] `
                -GuestDriverStore $store
            if (-not $binding.Bound) { $unmet += $binding.Unmet }
        }
    }

    if ($Requirement.ControlChannel) {
        if (-not (& $measured 'controlChannel')) { $unmet += 'the control channel was not measured' }
        elseif (-not $Receipt['controlChannel']) { $unmet += 'the control channel did not answer inside the guest' }
    }

    return @{ Ready = ($unmet.Count -eq 0); Unmet = $unmet }
}

function New-ReleaseVmGuestAgentScript {
    <#
    .SYNOPSIS
        The script the interactive agent runs inside the guest.
    .DESCRIPTION
        Started over PowerShell Direct, which is session 0 and owns no desktop -- so
        the first thing the agent does is write down the context it actually got, and
        only then run what it was asked to run. A receipt written afterwards would
        describe a context nobody checked before trusting it.

        The application under test needs a desktop; the channel that starts the agent
        cannot give it one. Everything else PowerShell Direct is good for --
        bootstrapping, file copy, starting this agent, rescue, evidence collection --
        it keeps doing.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [string] $ReceiptPath,
        [Parameter(Mandatory)] [string] $ResultPath
    )
    $defaults = Get-ReleaseVmDefault
    return @"
`$ErrorActionPreference = 'Stop'

# The context this agent got, written before anything runs in it.
`$receipt = @{ osReachable = `$true }
`$receipt['agentSessionId'] = (Get-Process -Id `$PID).SessionId
`$receipt['agentUser'] = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
if (-not ('ExoSnap.Agent.Native' -as [type])) {
    Add-Type -Namespace 'ExoSnap.Agent' -Name 'Native' -MemberDefinition @'
$($script:DisplayPathNative)
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
function Get-UserObjectName([System.IntPtr] `$handle) {
    `$buffer = New-Object System.Text.StringBuilder 256
    `$needed = 0
    if ([ExoSnap.Agent.Native]::GetUserObjectInformationW(`$handle, 2, `$buffer, 256, [ref]`$needed)) {
        return `$buffer.ToString()
    }
    return `$null
}
`$receipt['consoleSessionId'] = [int][ExoSnap.Agent.Native]::WTSGetActiveConsoleSessionId()
`$receipt['agentWindowStation'] = Get-UserObjectName ([ExoSnap.Agent.Native]::GetProcessWindowStation())
`$receipt['agentDesktop'] = Get-UserObjectName (
    [ExoSnap.Agent.Native]::GetThreadDesktop([ExoSnap.Agent.Native]::GetCurrentThreadId()))

# Which virtual display driver this image actually runs, by the device it binds to.
`$indirect = @(Get-PnpDevice -Class 'Display' -ErrorAction SilentlyContinue |
        Where-Object { `$_.InstanceId -like 'ROOT\DISPLAY\*' } | Select-Object -First 1)
if (`$indirect.Count -gt 0) {
    `$ids = @(Get-PnpDeviceProperty -InstanceId `$indirect[0].InstanceId ``
            -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue)
    `$receipt['displayDriver'] = @{
        InstanceId  = `$indirect[0].InstanceId
        Name        = `$indirect[0].FriendlyName
        HardwareIds = if (`$ids.Count -gt 0 -and `$null -ne `$ids[0].Data) { @(`$ids[0].Data) } else { @() }
    }
}

# Per display path, not per adapter: Win32_VideoController reports a mode per adapter
# and on a guest with an indirect display driver beside the synthetic one both can
# report a mode no display path is in.
`$Native = [ExoSnap.Agent.Native]
$($script:DisplayPathScript)
`$receipt['displayPaths'] = @(& `$paths)
`$adapter = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue |
        Where-Object { `$_.PNPDeviceID -like 'PCI\*' } | Select-Object -First 1)
if (`$adapter.Count -gt 0) {
    `$ids = [regex]::Match(`$adapter[0].PNPDeviceID, 'VEN_(?<vendor>[0-9A-F]{4})&DEV_(?<device>[0-9A-F]{4})')
    `$gpu = @{ Name = `$adapter[0].Name; DriverVersion = `$adapter[0].DriverVersion }
    if (`$ids.Success) {
        `$gpu['VendorId'] = `$ids.Groups['vendor'].Value
        `$gpu['DeviceId'] = `$ids.Groups['device'].Value
    }
    `$receipt['gpu'] = `$gpu
}

# The host driver package as the guest holds it. Copied in rather than installed, so
# it is not in the driver database and pnputil does not list it; the directory and
# the DriverVer its INF declares are what both sides can be asked for.
`$repository = '$($defaults.GuestDriverRepository)'
`$package = @(Get-ChildItem -LiteralPath `$repository -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1)
if (`$package.Count -gt 0) {
    `$store = @{ Package = `$package[0].Name }
    `$inf = @(Get-ChildItem -LiteralPath `$package[0].FullName -Filter '*.inf' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1)
    if (`$inf.Count -gt 0) {
        `$declared = [regex]::Match((Get-Content -LiteralPath `$inf[0].FullName -Raw),
            '(?im)^\s*DriverVer\s*=\s*[^,]+,\s*([0-9][0-9.]*)\s*`$')
        if (`$declared.Success) { `$store['DriverVersion'] = `$declared.Groups[1].Value }
    }
    `$receipt['hostDriverStore'] = `$store
}
`$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath '$ReceiptPath' -Encoding UTF8

# Only now, and only what the host asked for. The host reads the receipt first and
# stops the run if this session is not where the campaign belongs.
`$Command = `$env:EXOSNAP_AGENT_COMMAND
if (-not `$Command) { exit 0 }
`$directory = `$env:EXOSNAP_AGENT_WORKINGDIRECTORY
if (`$directory) { Set-Location -LiteralPath `$directory }
& cmd.exe /c `$Command
@{ exitCode = `$LASTEXITCODE } | ConvertTo-Json | Set-Content -LiteralPath '$ResultPath' -Encoding UTF8
"@
}

function Test-ReleaseVmAgentHandshake {
    <#
    .SYNOPSIS
        Whether the agent proved it is where the campaign belongs.
    .DESCRIPTION
        Pure. The receipt has to come from the agent: one measured over PowerShell
        Direct describes the PowerShell Direct session, which is never the session the
        campaign will run in.

        No receipt at all is an infrastructure failure, never a verdict. A guest whose
        files all copied and whose channel answers is still not a guest that can show
        a picture, and reporting that as a product result is the accusation this
        refuses to make.
    .OUTPUTS
        A hashtable with Ok and Detail.
    #>
    [OutputType([hashtable])]
    param(
        [AllowNull()] [System.Collections.IDictionary] $Receipt,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requirement
    )
    if ($null -eq $Receipt) {
        return @{ Ok = $false
            Detail = 'the guest agent wrote no receipt, so nothing is known about the session a campaign would run in' }
    }
    $verdict = Test-ReleaseVmReadiness -Receipt $Receipt -Requirement $Requirement
    if ($verdict.Ready) { return @{ Ok = $true; Detail = 'the agent is in the session the campaign needs' } }
    return @{ Ok = $false; Detail = ($verdict.Unmet -join '; ') }
}

function Start-ReleaseVmGuestAgent {
    <#
    .SYNOPSIS
        Starts the agent in the guest's interactive session and waits for its receipt.
    .DESCRIPTION
        Registered as a scheduled task with an interactive principal and started on
        demand: that is the documented way to put a process in the logged-on session
        from a channel that is not in it. The task is the launch mechanism and proves
        nothing by itself -- the agent's own receipt is the proof, and
        Assert-ReleaseVmAgentHandshake is what reads it.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $AgentRoot,
        [int] $TimeoutSeconds = 300
    )
    $receiptPath = Join-Path $AgentRoot 'agent-receipt.json'
    $resultPath = Join-Path $AgentRoot 'agent-result.json'
    $script = New-ReleaseVmGuestAgentScript -ReceiptPath $receiptPath -ResultPath $resultPath

    return Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock {
        param($Root, $Body, $ScriptPath, $ReceiptPath, $Timeout, $UserName)
        New-Item -ItemType Directory -Path $Root -Force | Out-Null
        Set-Content -LiteralPath $ScriptPath -Value $Body -Encoding UTF8
        Remove-Item -LiteralPath $ReceiptPath -Force -ErrorAction SilentlyContinue

        $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
            -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$ScriptPath`""
        $principal = New-ScheduledTaskPrincipal -UserId $UserName -LogonType Interactive -RunLevel Highest
        Register-ScheduledTask -TaskName 'ExoSnapGuestAgent' -Action $action -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskName 'ExoSnapGuestAgent'

        $deadline = [DateTime]::UtcNow.AddSeconds($Timeout)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $ReceiptPath) {
                $receipt = @{}
                (Get-Content -LiteralPath $ReceiptPath -Raw | ConvertFrom-Json).PSObject.Properties |
                    ForEach-Object { $receipt[$_.Name] = $_.Value }
                return $receipt
            }
            Start-Sleep -Seconds 2
        }
        return $null
    } -ArgumentList $AgentRoot, $script, (Join-Path $AgentRoot 'agent.ps1'), $receiptPath, $TimeoutSeconds,
        $Credential.UserName
}

function Assert-ReleaseVmAgentHandshake {
    <#
    .SYNOPSIS
        Starts the agent and throws unless it proved the context the run needs.
    .DESCRIPTION
        The plan step form, and the rule the whole interactive path exists for: a
        reachable guest whose agent is not interactive does not pass, however many
        files copied into it successfully.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $AgentRoot,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Requirement,
        [int] $TimeoutSeconds = 300
    )
    $receipt = Start-ReleaseVmGuestAgent -VMName $VMName -Credential $Credential -AgentRoot $AgentRoot `
        -TimeoutSeconds $TimeoutSeconds
    $outcome = Test-ReleaseVmAgentHandshake -Receipt $receipt -Requirement $Requirement
    if (-not $outcome.Ok) { throw "'$VMName' cannot run this campaign: $($outcome.Detail)" }
    return $receipt
}

function Invoke-ReleaseVmInteractiveCommand {
    <#
    .SYNOPSIS
        Runs one command line in the guest's interactive session and returns its exit code.
    .DESCRIPTION
        The campaign goes through the agent that already proved its context, not over
        PowerShell Direct: that channel is session 0, and the application under test
        needs a desktop.

        The exit code is returned rather than thrown on. A campaign that found defects
        exits non-zero, and that is a result.
    #>
    param(
        [Parameter(Mandatory)] [string] $VMName,
        [Parameter(Mandatory)] [System.Management.Automation.PSCredential] $Credential,
        [Parameter(Mandatory)] [string] $Command,
        [Parameter(Mandatory)] [string] $AgentRoot,
        [string] $WorkingDirectory = 'C:\ExoSnapRun',
        [int] $TimeoutMinutes = 120
    )
    $resultPath = Join-Path $AgentRoot 'agent-result.json'
    $result = Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock {
        param($CommandLine, $Directory, $ResultPath, $TimeoutSeconds)
        Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
        [Environment]::SetEnvironmentVariable('EXOSNAP_AGENT_COMMAND', $CommandLine, 'Machine')
        [Environment]::SetEnvironmentVariable('EXOSNAP_AGENT_WORKINGDIRECTORY', $Directory, 'Machine')
        try {
            Start-ScheduledTask -TaskName 'ExoSnapGuestAgent'
            $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
            while ([DateTime]::UtcNow -lt $deadline) {
                if (Test-Path -LiteralPath $ResultPath) {
                    return (Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json).exitCode
                }
                Start-Sleep -Seconds 5
            }
            return $null
        }
        finally {
            [Environment]::SetEnvironmentVariable('EXOSNAP_AGENT_COMMAND', $null, 'Machine')
            [Environment]::SetEnvironmentVariable('EXOSNAP_AGENT_WORKINGDIRECTORY', $null, 'Machine')
        }
    } -ArgumentList $Command, $WorkingDirectory, $resultPath, ($TimeoutMinutes * 60)

    if ($null -eq $result) {
        throw "the guest agent did not finish within $TimeoutMinutes minute(s): $Command"
    }
    return $result
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
        param($driverRepository, $nativeMembers, $pathScript)
        $receipt = @{ osReachable = $true }
        $receipt['agentSessionId'] = (Get-Process -Id $PID).SessionId
        $receipt['agentUser'] = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name

        if (-not ('ExoSnap.Readiness.Native' -as [type])) {
            Add-Type -Namespace 'ExoSnap.Readiness' -Name 'Native' -MemberDefinition ($nativeMembers + @'
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
'@)
        }
        $Native = [ExoSnap.Readiness.Native]
        . ([scriptblock]::Create($pathScript))
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

        # The host driver package as the guest holds it: copied in rather than
        # installed, so it is not in the driver database and the directory plus the
        # DriverVer its INF declares are what both sides can be asked for. The
        # repository path is passed in because this block runs inside the guest, where
        # nothing of this module exists.
        $package = @(Get-ChildItem -LiteralPath $driverRepository -Directory -ErrorAction SilentlyContinue |
                Select-Object -First 1)
        if ($package.Count -gt 0) {
            $store = @{ Package = $package[0].Name }
            $inf = @(Get-ChildItem -LiteralPath $package[0].FullName -Filter '*.inf' -File -ErrorAction SilentlyContinue |
                    Select-Object -First 1)
            if ($inf.Count -gt 0) {
                $declared = [regex]::Match((Get-Content -LiteralPath $inf[0].FullName -Raw),
                    '(?im)^\s*DriverVer\s*=\s*[^,]+,\s*([0-9][0-9.]*)\s*$')
                if ($declared.Success) { $store['DriverVersion'] = $declared.Groups[1].Value }
            }
            $receipt['hostDriverStore'] = $store
        }

        # Which virtual display driver this image actually runs, by the device it
        # binds to. The profile a campaign was qualified on is a claim in the
        # manifest; this is the measurement it has to agree with.
        $indirect = @(Get-PnpDevice -Class 'Display' -ErrorAction SilentlyContinue |
                Where-Object { $_.InstanceId -like 'ROOT\DISPLAY\*' } | Select-Object -First 1)
        if ($indirect.Count -gt 0) {
            $ids = @(Get-PnpDeviceProperty -InstanceId $indirect[0].InstanceId `
                    -KeyName 'DEVPKEY_Device_HardwareIds' -ErrorAction SilentlyContinue)
            $receipt['displayDriver'] = @{
                InstanceId  = $indirect[0].InstanceId
                Name        = $indirect[0].FriendlyName
                HardwareIds = if ($ids.Count -gt 0 -and $null -ne $ids[0].Data) { @($ids[0].Data) } else { @() }
            }
        }

        # Per display path, not per adapter. Win32_VideoController answers with a mode
        # per adapter, and on a guest with an indirect display driver beside the
        # synthetic one both adapters can report a mode no display path is actually
        # in -- so a gate asking for a refresh rate is told it has one. These two
        # functions read the display device database and need no desktop, which is
        # what ruled out the display APIs here before.
        $receipt['displayPaths'] = @(& $paths)
        return $receipt
    }
    try {
        return Invoke-Command -VMName $VMName -Credential $Credential -ScriptBlock $measure -ArgumentList `
            (Get-ReleaseVmDefault).GuestDriverRepository, $script:DisplayPathNative, $script:DisplayPathScript `
            -ErrorAction Stop
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

function Get-ReleaseVmImageFingerprintDigest {
    <#
    .SYNOPSIS
        One stable digest over the facts that identify a golden image.
    .DESCRIPTION
        Order-independent: a hashtable has none, and a digest that depended on
        enumeration order would report drift between two readings of the same image.
        Keys are sorted and the pairs are joined with separators that cannot occur in
        a key, so two different fact sets cannot collide by concatenation.
    #>
    [OutputType([string])]
    param([Parameter(Mandatory)] [System.Collections.IDictionary] $Fingerprint)
    $pairs = @($Fingerprint.Keys | Sort-Object | ForEach-Object { "$_=$($Fingerprint[$_])" })
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($pairs -join "`n")
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return 'sha256:' + [System.BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function Test-ReleaseVmImageFingerprint {
    <#
    .SYNOPSIS
        Whether the image a run is about to use is the image the run was qualified on.
    .DESCRIPTION
        Pure, and it names every drifted field at once: rebuilding a golden image once
        per discovered difference is not a workflow.

        A fact the image never recorded is drift rather than agreement. An older
        image simply does not carry a field a later build of this recipe compares,
        and the absence of a record is not evidence that the two agree.
    .OUTPUTS
        A hashtable with Matches and Differences.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Expected,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Actual
    )
    $differences = @()
    foreach ($key in @($Expected.Keys | Sort-Object)) {
        if (-not $Actual.Contains($key) -or $null -eq $Actual[$key]) {
            $differences += "$key`: the run needs $($Expected[$key]); the image has not recorded it"
            continue
        }
        if ("$($Actual[$key])" -ne "$($Expected[$key])") {
            $differences += "$key`: the run needs $($Expected[$key]); the image is $($Actual[$key])"
        }
    }
    return @{ Matches = ($differences.Count -eq 0); Differences = $differences }
}

function Get-ReleaseVmManifestFingerprint {
    <#
    .SYNOPSIS
        The part of an image fingerprint the provisioning manifest decides.
    .DESCRIPTION
        The display driver profile, the monitor the gates assert against, and a digest
        over every package pin. The rest of a fingerprint -- the Windows build, the
        host GPU driver the guest driver was staged from -- is measured, not declared,
        and is added by whoever builds or checks the image.
    #>
    [OutputType([hashtable])]
    param([Parameter(Mandatory)] [System.Collections.IDictionary] $Manifest)
    $pins = @($Manifest.packages | Sort-Object { $_.id } | ForEach-Object {
            $version = if ($_.Contains('version')) { $_.version } else { '' }
            $sha = if ($_.Contains('sha256')) { $_.sha256 } else { '' }
            "$($_.id)|$($_.kind)|$version|$sha"
        })
    $modes = ($Manifest.display.refreshRates | ForEach-Object { "$_" }) -join ','
    return @{
        displayProfile = $Manifest.displayProfile
        displayMode    = "$($Manifest.display.width)x$($Manifest.display.height)@$modes"
        packagePins    = Get-ReleaseVmImageFingerprintDigest -Fingerprint @{ pins = ($pins -join "`n") }
    }
}

function Assert-ReleaseVmImageFingerprint {
    <#
    .SYNOPSIS
        Refuses a run whose image is not the image the run was qualified on.
    .DESCRIPTION
        The plan step form. Evidence attributed to the wrong image is worse than no
        evidence: the two display-driver profiles this recipe knows behave differently
        under capture, and a campaign that cannot name the one it ran on cannot be
        repeated or compared.
    #>
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Expected,
        [Parameter(Mandatory)] [string] $FingerprintPath
    )
    if (-not (Test-Path -LiteralPath $FingerprintPath)) {
        throw ("this image records no fingerprint at '$FingerprintPath', so it cannot be told apart from any " +
            'other image. Rebuild it, or write the fingerprint of the image that is there.')
    }
    $actual = @{}
    (Get-Content -LiteralPath $FingerprintPath -Raw | ConvertFrom-Json).PSObject.Properties |
        ForEach-Object { $actual[$_.Name] = $_.Value }

    $drift = Test-ReleaseVmImageFingerprint -Expected $Expected -Actual $actual
    if (-not $drift.Matches) {
        throw ("the golden image is not the image this run was qualified on:`n    " +
            ($drift.Differences -join "`n    "))
    }
    return $actual
}

function Write-ReleaseVmImageFingerprint {
    <#
    .SYNOPSIS
        Records what an image is, beside the image.
    .DESCRIPTION
        Written when the image is built and read by every run afterwards. The digest
        is stored alongside the facts so a changed fingerprint file is visible as
        such, rather than only as a comparison failing somewhere later.
    #>
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Fingerprint,
        [Parameter(Mandatory)] [string] $Path
    )
    $document = [ordered]@{}
    foreach ($key in @($Fingerprint.Keys | Sort-Object)) { $document[$key] = $Fingerprint[$key] }
    $document['digest'] = Get-ReleaseVmImageFingerprintDigest -Fingerprint $Fingerprint

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    Set-Content -LiteralPath $Path -Value ($document | ConvertTo-Json -Depth 4) -Encoding UTF8
    return $Path
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

function Test-ReleaseVmDisplayProfile {
    <#
    .SYNOPSIS
        Whether a run may claim the virtual display profile it was qualified on.
    .DESCRIPTION
        Three answers, not two, because the interesting case is neither. A scenario
        qualified on one virtual display driver and run on another is not a failing
        scenario -- it is a scenario nobody ran, and reporting it as a failure would
        attribute an image difference to the product.

          qualified      the guest runs the device the qualified profile names.
          not-qualified  it runs a different one, and both are named.
          unverifiable   the profile has no recorded device identity, or the guest's
                         display driver was not measured. Nothing can confirm it, so
                         nothing claims it.

        A profile with no pinned device identity is the state this recipe is in for
        the profile the capture work was qualified on: reconciling it is an image
        rebuild with the package pinned, not a comparison that can be made here.
    .OUTPUTS
        A hashtable with Qualified, Verdict and Detail.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [AllowEmptyString()] [AllowNull()] [string] $Required,
        [Parameter(Mandatory)] [AllowNull()] [System.Collections.IDictionary] $Measured,
        [System.Collections.IDictionary] $KnownProfile
    )
    if (-not $KnownProfile) { $KnownProfile = (Get-ReleaseVmDefault).DisplayProfileHardwareId }

    if ([string]::IsNullOrWhiteSpace($Required)) {
        return @{ Qualified = $false; Verdict = 'unverifiable'
                  Detail = 'no qualified display profile is declared, so no run can be held to one' }
    }
    if (-not $KnownProfile.Contains($Required)) {
        return @{ Qualified = $false; Verdict = 'unverifiable'
                  Detail = ("the qualified display profile '$Required' has no recorded device identity in this " +
                      'recipe; pin it the way every provisioned package is pinned before a run may claim it') }
    }

    $hardwareIds = @()
    if ($null -ne $Measured -and $Measured.Contains('HardwareIds') -and $null -ne $Measured['HardwareIds']) {
        $hardwareIds = @($Measured['HardwareIds'])
    }
    if ($hardwareIds.Count -eq 0) {
        return @{ Qualified = $false; Verdict = 'unverifiable'
                  Detail = "the guest's virtual display driver was not measured, so '$Required' is unproven" }
    }

    $expected = "$($KnownProfile[$Required])"
    if ($hardwareIds | Where-Object { "$_" -eq $expected }) {
        return @{ Qualified = $true; Verdict = 'qualified'; Detail = "the guest runs $expected, the '$Required' profile" }
    }
    return @{ Qualified = $false; Verdict = 'not-qualified'
              Detail = ("the guest runs $($hardwareIds -join ', '), not $expected, so it is not the " +
                  "'$Required' profile this scenario was qualified on") }
}

function Test-ReleaseVmPartitionProvenance {
    <#
    .SYNOPSIS
        Whether the partition this run created came from the host GPU it measured.
    .DESCRIPTION
        Entirely a host-side question, and the only one of the GPU facts that can be
        answered by comparing two PCI identities: both sides are read in the same
        operating system, off the same device. Hyper-V reports the partition adapter's
        InstancePath as the physical adapter's PnP path with the separators rewritten
        and an interface GUID appended, so the host path is normalised to that shape
        and has to appear in it.

        This is the assertion that says which physical GPU a campaign ran on. It says
        nothing about the guest, which never sees these ids.
    .OUTPUTS
        A hashtable with Bound and Unmet.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $HostGpu,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Partition
    )
    $unmet = @()
    $hasHost = $HostGpu.Contains('InstancePath') -and -not [string]::IsNullOrWhiteSpace("$($HostGpu['InstancePath'])")
    $hasPartition = $Partition.Contains('InstancePath') -and -not [string]::IsNullOrWhiteSpace("$($Partition['InstancePath'])")
    if (-not $hasHost) { $unmet += 'the host adapter instance path was not measured' }
    if (-not $hasPartition) { $unmet += 'the partition adapter instance path was not measured' }
    if ($unmet.Count -eq 0) {
        # PCI\VEN_10DE&DEV_2C05&SUBSYS_...\9B3B... is reported on the partition as
        # \\?\PCI#VEN_10DE&DEV_2C05&SUBSYS_...#9B3B...#{guid}\GPUPARAV.
        $normalised = "$($HostGpu['InstancePath'])".Replace('\', '#')
        if ("$($Partition['InstancePath'])".IndexOf($normalised, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $unmet += ("the partition is on $($Partition['InstancePath']), which does not name the measured " +
                "host adapter $($HostGpu['InstancePath'])")
        }
    }
    return @{ Bound = ($unmet.Count -eq 0); Unmet = $unmet }
}

function Test-ReleaseVmGpuBinding {
    <#
    .SYNOPSIS
        Whether the guest is on this run's GPU partition, running this run's driver.
    .DESCRIPTION
        Deliberately NOT a comparison of PCI ids across the two operating systems.
        What a GPU-P guest binds to is the paravirtual device: it reports Microsoft's
        vendor id and a Microsoft inbox driver version, because the vendor's
        kernel-mode driver stays on the host. A rule that required the host's vendor
        and device ids to appear in the guest would refuse every correct campaign and
        accept none.

        Two independent things are asked instead:

          the adapter    the guest is on a paravirtual device that presents the host
                         adapter. The GPU-P device carries the host GPU's friendly
                         name; the Basic Render Driver, which is the same vendor and
                         the fallback this is meant to catch, does not.
          the driver     the user-mode driver package staged into the guest is the
                         package the host selected, at the version the host adapter is
                         running. This is the pair the classic GPU-P failure shows up
                         in, and the bridge between the two operating systems that
                         Windows does expose identically on both sides.

        An unmeasured fact is unbound rather than assumed: a guest that reported no
        adapter, or no staged package, proves nothing.
    .OUTPUTS
        A hashtable with Bound and Unmet.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $HostGpu,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $GuestGpu,
        [System.Collections.IDictionary] $GuestDriverStore
    )
    $unmet = @()
    $defaults = Get-ReleaseVmDefault

    $guestVendor = if ($GuestGpu.Contains('VendorId')) { "$($GuestGpu['VendorId'])" } else { $null }
    if ([string]::IsNullOrWhiteSpace($guestVendor)) {
        $unmet += 'the guest vendorId was not measured'
    }
    elseif ($guestVendor.TrimStart('0').PadLeft(1, '0') -ne $defaults.ParavirtualVendorId.TrimStart('0')) {
        $unmet += ("the guest adapter is vendor $guestVendor, not the paravirtual " +
            "$($defaults.ParavirtualVendorId): this guest is not on a GPU partition")
    }

    $hostName = if ($HostGpu.Contains('Name')) { "$($HostGpu['Name'])" } else { $null }
    $guestName = if ($GuestGpu.Contains('Name')) { "$($GuestGpu['Name'])" } else { $null }
    if ([string]::IsNullOrWhiteSpace($hostName)) { $unmet += 'the host adapter name was not measured' }
    elseif ([string]::IsNullOrWhiteSpace($guestName)) { $unmet += 'the guest adapter name was not measured' }
    elseif ($hostName -ne $guestName) {
        $unmet += "the guest adapter presents itself as '$guestName', not as the partitioned '$hostName'"
    }

    $store = if ($null -ne $GuestDriverStore) { $GuestDriverStore } else { @{} }
    foreach ($field in @('Package', 'DriverVersion')) {
        $name = $field.Substring(0, 1).ToLowerInvariant() + $field.Substring(1)
        $expected = if ($HostGpu.Contains($field)) { "$($HostGpu[$field])" } else { $null }
        $actual = if ($store.Contains($field)) { "$($store[$field])" } else { $null }
        if ([string]::IsNullOrWhiteSpace($expected)) { $unmet += "the host driver $name was not measured"; continue }
        if ([string]::IsNullOrWhiteSpace($actual)) { $unmet += "the staged driver $name was not measured in the guest"; continue }
        if ($expected -ne $actual) { $unmet += "staged driver $name`: host $expected, guest $actual" }
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
    param([string] $InstancePathPattern = (Get-ReleaseVmDefault).HostGpuInstancePattern)
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

    # The DriverStore package the running driver came from, by name. This is the half
    # of the GPU binding the guest can be held to: the package is copied into the
    # guest verbatim, so both operating systems can be asked the same question.
    #
    # Only with a version in hand. Get-ReleaseVmHostDriverPackage falls back to
    # measuring the adapter when it is given none, which is this function, and an
    # adapter that reported no driver version would recurse instead of answering.
    if (-not [string]::IsNullOrWhiteSpace("$($identity['DriverVersion'])")) {
        $package = Get-ReleaseVmHostDriverPackage -ActiveDriverVersion "$($identity['DriverVersion'])"
        if ($package) { $identity['Package'] = Split-Path -Leaf $package }
    }

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

        What lands in the destination is the CONTENT of the guest directory, not a
        directory named after it. The destination is a run's evidence directory and is
        already named after the run; adding the guest's own path as a level under it
        puts the result document one directory below where the run said its evidence
        is, and below where a caller that composed the run looks for it.
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
        # -Path with a wildcard rather than -LiteralPath on the directory: the latter
        # copies the directory itself, which is one level too deep.
        Copy-Item -FromSession $session -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
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
    'Get-ReleaseVmImageFingerprintDigest'
    'Test-ReleaseVmImageFingerprint'
    'Get-ReleaseVmManifestFingerprint'
    'Assert-ReleaseVmImageFingerprint'
    'Write-ReleaseVmImageFingerprint'
    'Get-ReleaseVmInfDriverVersion'
    'Select-ReleaseVmDriverPackage'
    'Compare-ReleaseVmGpuPartition'
    'Test-ReleaseVmGpuBinding'
    'Test-ReleaseVmPartitionProvenance'
    'Test-ReleaseVmDisplayProfile'
    'Get-ReleaseVmHostGpu'
    'Assert-ReleaseVmGpuPartition'
    'New-ReleaseVmGuestAgentScript'
    'Test-ReleaseVmAgentHandshake'
    'Start-ReleaseVmGuestAgent'
    'Assert-ReleaseVmAgentHandshake'
    'Invoke-ReleaseVmInteractiveCommand'
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
