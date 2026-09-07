#Requires -Version 7.0
<#
.SYNOPSIS
    Windows Sandbox as the machine the install, update and packaging gates run on.

.DESCRIPTION
    The install-shaped gates used to run on the developer's real machine: their real
    config directory, their real installed product, their real machine-wide
    single-instance mutex. Three of the four runner defects the rc19 campaign found
    were consequences of that and of nothing else:

      * the accept gate removed the older installed build the decline gate needed,
        so the two could only ever run in one order and only once;
      * a killed soak left a recovery manifest under %LOCALAPPDATA%\ExoSnap and both
        MSI gates refused to start against a machine in that state;
      * a declined update left the updater holding exosnap-updater.exe, and the next
        launch could not stage its own updater over it.

    A sandbox starts clean every time, so none of those is reachable. It also has no
    UAC prompt to click: the logon command runs with administrative rights, which is
    what made the operator's part of these gates disappear rather than merely get
    shorter.

    Nothing here starts a sandbox by itself. `New-ReleaseSandboxConfiguration` writes
    the staged tree and the .wsb; `Start-ReleaseSandboxRun` launches it through the
    external-tool seam and waits for the worker's own result document. A dry run
    replaces that seam and observes the whole flow without a virtual machine.
#>

Set-StrictMode -Version Latest

# Where a mapped folder lands INSIDE the sandbox: the sandbox user's desktop,
# under the host folder's own leaf name. Fixed by Windows, not configurable, and
# the same on every machine -- which is why the guest paths can be computed here
# rather than discovered from inside the virtual machine.
$script:ReleaseSandboxDesktop = 'C:\Users\WDAGUtilityAccount\Desktop'

function Get-ReleaseSandboxGuestPath {
    <#
    .SYNOPSIS
        Where a host directory mapped into the sandbox appears inside it.
    #>
    param([Parameter(Mandatory)] [string] $HostDirectory)
    return Join-Path $script:ReleaseSandboxDesktop (Split-Path -Leaf $HostDirectory)
}

function New-ReleaseSandboxStaging {
    <#
    .SYNOPSIS
        Copies one worker and the files it needs into a fresh staging directory.
    .DESCRIPTION
        Returns @{ Ok; Detail; Directory; Files }. Copied rather than mapped from the
        repository on purpose: the sandbox may write into its mapped folder, and a
        gate that could write into the working tree is a gate that can change the
        thing it is verifying.

        A file that is named and not found is an error here rather than a mystery
        inside a virtual machine with no console attached to it.
    #>
    param(
        [Parameter(Mandatory)] [string] $Directory,
        [Parameter(Mandatory)] [string[]] $SourceFiles
    )
    if (Test-Path -LiteralPath $Directory) { Remove-Item -LiteralPath $Directory -Recurse -Force }
    New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    $copied = @()
    foreach ($source in $SourceFiles) {
        if (-not (Test-Path -LiteralPath $source)) {
            return @{ Ok = $false; Detail = "the sandbox run needs '$source', which does not exist"; Directory = $Directory; Files = @() }
        }
        $leaf = Split-Path -Leaf $source
        Copy-Item -LiteralPath $source -Destination (Join-Path $Directory $leaf) -Recurse -Force
        $copied += $leaf
    }
    return @{ Ok = $true; Detail = "$($copied.Count) file(s) staged"; Directory = $Directory; Files = $copied }
}

function New-ReleaseSandboxConfiguration {
    <#
    .SYNOPSIS
        Writes the .wsb that runs one worker script inside a clean Windows.
    .DESCRIPTION
        Returns @{ Ok; Detail; ConfigurationPath; StagingDirectory; ResultPath;
        MarkerPath }.

        The staging directory is mapped READ-WRITE and is the only channel in both
        directions: the artifacts and the worker go in, the result document and the
        per-step logs come back. There is deliberately no second mapping of the
        repository -- a gate that could reach the working tree could also change it,
        and a sandbox whose whole point is a known starting state must not inherit
        an unknown one.

        vGPU stays off. These gates install and uninstall software; none of them
        renders, and a virtual GPU is the sandbox feature most likely to differ
        between two machines.
    #>
    param(
        [Parameter(Mandatory)] [string] $StagingDirectory,
        [Parameter(Mandatory)] [string] $WorkerFileName,
        [string[]] $WorkerArguments = @(),
        [string] $PowerShellHome = $PSHOME,
        [switch] $NoNetwork
    )
    if (-not (Test-Path -LiteralPath $StagingDirectory)) {
        New-Item -ItemType Directory -Path $StagingDirectory -Force | Out-Null
    }
    $workerPath = Join-Path $StagingDirectory $WorkerFileName
    if (-not (Test-Path -LiteralPath $workerPath)) {
        return @{ Ok = $false; Detail = "the sandbox worker $WorkerFileName is not staged in $StagingDirectory" }
    }
    # PowerShell 7, mapped read-only rather than installed or copied. Windows
    # Sandbox ships Windows PowerShell 5.1 only, and every module the workers use
    # declares 7.0 -- so the alternatives were to downgrade the shared client code
    # to 5.1 (two spellings of one protocol) or to download an installer inside a
    # machine whose whole value is a known starting state.
    if ([string]::IsNullOrWhiteSpace($PowerShellHome) -or -not (Test-Path -LiteralPath $PowerShellHome)) {
        return @{ Ok = $false; Detail = "the PowerShell 7 home '$PowerShellHome' does not exist, so the sandbox has no shell to run the worker with" }
    }

    $guest = Get-ReleaseSandboxGuestPath -HostDirectory $StagingDirectory
    $guestWorker = Join-Path $guest $WorkerFileName
    $guestShell = Join-Path (Get-ReleaseSandboxGuestPath -HostDirectory $PowerShellHome) 'pwsh.exe'
    # Quoted argument by argument rather than joined once: a staged path contains
    # the campaign id and, on a machine whose user name has a space, a space.
    $quoted = @($WorkerArguments | ForEach-Object { "'" + ($_ -replace "'", "''") + "'" }) -join ' '
    $command = "cmd.exe /c `"$guestShell`" -ExecutionPolicy Bypass -NoProfile -File `"$guestWorker`" $quoted"

    $networking = if ($NoNetwork) { 'Disable' } else { 'Default' }
    $configuration = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>$networking</Networking>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$StagingDirectory</HostFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$PowerShellHome</HostFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>$([System.Security.SecurityElement]::Escape($command))</Command>
  </LogonCommand>
</Configuration>
"@
    $configurationPath = Join-Path $StagingDirectory 'release-verify.wsb'
    Set-Content -LiteralPath $configurationPath -Value $configuration -Encoding utf8NoBOM
    return @{ Ok = $true
        Detail            = "sandbox configuration for $WorkerFileName"
        ConfigurationPath = $configurationPath
        StagingDirectory  = $StagingDirectory
        GuestDirectory    = $guest
        GuestShell        = $guestShell
        ResultPath        = Join-Path $StagingDirectory 'result.json'
        MarkerPath        = Join-Path $StagingDirectory 'done.marker'
    }
}

function Start-ReleaseSandboxRun {
    <#
    .SYNOPSIS
        Runs one staged sandbox worker and waits for its result document.
    .DESCRIPTION
        WindowsSandbox.exe returns as soon as the virtual machine is asked for, not
        when the work inside it finishes, so completion is read from the worker's
        own marker file. That is the only honest signal available: a sandbox that
        crashed, was closed by hand or never started leaves no marker, and this
        reports UNVERIFIED for it rather than reading a partial result as a verdict.

        Returns @{ Ok; Result; Detail }, where `Result` is the parsed result.json
        when the worker wrote one and $null otherwise.
    #>
    param(
        [Parameter(Mandatory)] $Tool,
        [Parameter(Mandatory)] $Configuration,
        [int] $TimeoutMinutes = 30
    )
    Remove-Item -LiteralPath $Configuration.MarkerPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $Configuration.ResultPath -Force -ErrorAction SilentlyContinue

    $launch = Invoke-ReleaseTool -Tool $Tool -Arguments @($Configuration.ConfigurationPath) -TimeoutSeconds 120
    if ($launch.ExitCode -ne 0) {
        return @{ Ok = $false; Result = $null
            Detail   = "WindowsSandbox.exe exited $($launch.ExitCode) without starting the rehearsal: $($launch.Output)"
        }
    }

    $deadline = [DateTime]::UtcNow.AddMinutes($TimeoutMinutes)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $Configuration.MarkerPath) { break }
        Start-Sleep -Seconds 2
    }
    if (-not (Test-Path -LiteralPath $Configuration.MarkerPath)) {
        return @{ Ok = $false; Result = $null
            Detail   = "the sandbox worker did not finish within $TimeoutMinutes minute(s); no result was written"
        }
    }
    if (-not (Test-Path -LiteralPath $Configuration.ResultPath)) {
        return @{ Ok = $false; Result = $null
            Detail   = 'the sandbox worker signalled completion but wrote no result document'
        }
    }
    try {
        $result = Get-Content -LiteralPath $Configuration.ResultPath -Raw | ConvertFrom-Json
    }
    catch {
        return @{ Ok = $false; Result = $null; Detail = "the sandbox result document is not valid JSON: $($_.Exception.Message)" }
    }
    return @{ Ok = $true; Result = $result; Detail = 'the sandbox worker finished and wrote its result' }
}

function Get-ReleaseSandboxStepVerdict {
    <#
    .SYNOPSIS
        Turns a worker's step list into one verdict.
    .DESCRIPTION
        The same rule the Chocolatey rehearsal already applies, in one place so
        every sandbox-hosted gate reads the same: a step that FAILED is a FAIL, a
        step the worker never reached is UNVERIFIED, and only a run in which every
        required step passed is a PASS. `Ok = $true` on a step is not enough on its
        own -- the set of steps has to be complete, because a worker that stopped
        early leaves a list of nothing but passes.
    #>
    param(
        $Result,
        [Parameter(Mandatory)] [string[]] $RequiredSteps
    )
    if ($null -eq $Result) {
        return @{ Result = 'UNVERIFIED'; Message = 'the sandbox worker produced no result document' }
    }
    $steps = @()
    if ($null -ne $Result.PSObject.Properties['steps']) { $steps = @($Result.steps) }
    $byName = @{}
    foreach ($step in $steps) { $byName["$($step.name)"] = $step }

    $failed = @()
    $missing = @()
    foreach ($name in $RequiredSteps) {
        if (-not $byName.ContainsKey($name)) { $missing += $name; continue }
        if (-not $byName[$name].ok) { $failed += "$name : $($byName[$name].detail)" }
    }
    if ($failed.Count -gt 0) {
        return @{ Result = 'FAIL'; Message = "$($failed.Count) step(s) failed in the sandbox: $($failed -join ' | ')" }
    }
    if ($missing.Count -gt 0) {
        return @{ Result = 'UNVERIFIED'
            Message      = "the sandbox worker never reached $($missing.Count) required step(s): $($missing -join ', ')"
        }
    }
    return @{ Result = 'PASS'; Message = "all $($RequiredSteps.Count) sandbox step(s) passed" }
}
