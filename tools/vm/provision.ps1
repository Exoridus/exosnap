<#
.SYNOPSIS
    Turns a freshly installed Windows 11 guest into the ExoSnap release-verification
    machine. Runs INSIDE the guest.

.DESCRIPTION
    Idempotent and resumable, because it cannot be neither: VB-CABLE needs a reboot
    in the middle, and the only channel into this machine is PowerShell Direct, which
    the reboot drops. Every step records itself in a state file; a second run skips
    what is already done and continues where the reboot interrupted it.

    Deliberately written for Windows PowerShell 5.1. It is the shell a clean Windows
    has, and one of the things it installs is PowerShell 7 -- a script that needed 7
    to install 7 could not run at all.

    Nothing is installed unpinned. Every package in provision-manifest.psd1 carries
    either an exact winget version (winget verifies the installer against the hash in
    its own manifest) or a URL and a SHA-256 this script compares before it runs the
    file. A pin that still reads PIN-REQUIRED aborts the step and prints the command
    that produces the real value.

    UAC is left exactly as Windows configured it. The harness needs
    ConsentPromptBehaviorAdmin = 0 for the update-accept path, and flipping it here
    would put every gate on a machine that never prompts -- including the gates whose
    subject is the prompt. The 'uac' step records the value and prints the flip.

.PARAMETER ManifestPath
    The pinned package manifest. Defaults to provision-manifest.psd1 beside this file.

.PARAMETER Only
    Run only the named steps. Without it every step runs, in order.

.PARAMETER Force
    Run steps that the state file already records as done.

.PARAMETER EnableHdr
    Configure the virtual display for HDR. Off by default: HDR changes the capture
    colour space, so the SDR gates and the HDR gates want different golden states.

.PARAMETER ListSteps
    Print the step names and exit.

.PARAMETER StatePath
    Where the resume state lives. Defaults to provision-state.json in the staging
    directory named by the manifest.

.EXAMPLE
    powershell.exe -NoProfile -File <staging directory>\provision.ps1

.EXAMPLE
    powershell.exe -NoProfile -File <staging directory>\provision.ps1 -Only idd,uac
#>
[CmdletBinding()]
param(
    [string] $ManifestPath,
    [string[]] $Only = @(),
    [switch] $Force,
    [switch] $EnableHdr,
    [switch] $ListSteps,
    [string] $StatePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Windows PowerShell 5.1 still negotiates TLS 1.0 by default, and every download
# below is https to a host that no longer offers it.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$script:PinSentinel = 'PIN-REQUIRED'
$script:RebootPending = $false

function Write-Line {
    param([string] $Text, [string] $Level = 'info')
    $prefix = switch ($Level) {
        'ok'   { '  ok   ' }
        'skip' { '  skip ' }
        'warn' { '  warn ' }
        'fail' { '  FAIL ' }
        default { '       ' }
    }
    Write-Host "$prefix$Text"
}

# ---------------------------------------------------------------------------
# Manifest and pins
# ---------------------------------------------------------------------------

function Get-Manifest {
    param([string] $Path)
    if (-not $Path) { $Path = Join-Path $PSScriptRoot 'provision-manifest.psd1' }
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "the pinned package manifest '$Path' does not exist; Copy-VMFile it into the guest next to provision.ps1"
    }
    return Import-PowerShellDataFile -LiteralPath $Path
}

function Get-Package {
    <#
    .SYNOPSIS
        One manifest entry, with every pin proven present before it is used.
    #>
    param([Parameter(Mandatory)] $Manifest, [Parameter(Mandatory)] [string] $Id)
    $package = @($Manifest.packages | Where-Object { $_.id -eq $Id })
    if ($package.Count -ne 1) {
        throw "the manifest describes $($package.Count) packages with id '$Id'; it must describe exactly one"
    }
    $package = $package[0]

    if ($package.version -eq $script:PinSentinel) {
        throw ("'$Id' has no version pin. Record the version you actually install, then commit it: " +
               "for a winget package run 'winget show --id $(if ($package.Contains('wingetId')) { $package.wingetId } else { $Id }) --versions', " +
               "for a download take the version from the release you downloaded, and put it in provision-manifest.psd1.")
    }
    if ($package.kind -eq 'download' -and $package.sha256 -eq $script:PinSentinel) {
        throw ("'$Id' has no SHA-256 pin. Download $(Resolve-PackageUrl -Package $package) once, run " +
               "'Get-FileHash -Algorithm SHA256 <file>', and put the hash in provision-manifest.psd1.")
    }
    return $package
}

function Resolve-PackageUrl {
    param([Parameter(Mandatory)] $Package)
    return $Package.urlTemplate.Replace('{version}', [string]$Package.version)
}

# ---------------------------------------------------------------------------
# Resume state
# ---------------------------------------------------------------------------

function Get-State {
    param([Parameter(Mandatory)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return @{} }
    $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    # An empty file is what a restart in the middle of a write leaves behind, and
    # the restart this script asks for is a normal part of provisioning. It says
    # nothing went wrong, so it is not worth a warning.
    if ([string]::IsNullOrWhiteSpace($raw)) { return @{} }
    try {
        $parsed = $raw | ConvertFrom-Json
    }
    catch {
        Write-Line "the resume state at $Path is unreadable; every step will run again" 'warn'
        return @{}
    }
    $state = @{}
    foreach ($property in $parsed.PSObject.Properties) { $state[$property.Name] = $property.Value }
    return $state
}

function Save-State {
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] [hashtable] $State)
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    # Written beside the target and renamed, because the step that follows this one
    # can restart the guest: a half-written file would lose every completed step.
    $temporary = "$Path.tmp"
    ($State | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

# ---------------------------------------------------------------------------
# Download, verify, extract
# ---------------------------------------------------------------------------

function Get-VerifiedDownload {
    <#
    .SYNOPSIS
        Downloads one pinned file and returns its path, or throws.
    .DESCRIPTION
        The hash comparison is the whole point, so it happens before the file is
        usable to anything: a mismatch deletes the download rather than leaving a
        file on disk that a later step might pick up.
    #>
    param(
        [Parameter(Mandatory)] $Package,
        [Parameter(Mandatory)] [string] $Directory
    )
    if (-not (Test-Path -LiteralPath $Directory)) {
        New-Item -ItemType Directory -Path $Directory -Force | Out-Null
    }
    $url = Resolve-PackageUrl -Package $Package
    $target = Join-Path $Directory (Split-Path -Leaf ([Uri]$url).AbsolutePath)

    if (-not (Test-Path -LiteralPath $target)) {
        Write-Line "downloading $url"
        # BITS and Invoke-WebRequest both work; Invoke-WebRequest is the one that is
        # present on a machine with no network policy configured yet.
        Invoke-WebRequest -Uri $url -OutFile $target -UseBasicParsing
    }

    $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = ([string]$Package.sha256).ToLowerInvariant()
    if ($actual -ne $expected) {
        Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
        throw ("$($Package.id): the file at $url hashes $actual, the manifest pins $expected. " +
               'Either the release was replaced or the download is not what it claims to be; provisioning stops here.')
    }
    Write-Line "verified $($Package.id) $($Package.version) against its SHA-256 pin" 'ok'
    return $target
}

function Expand-VerifiedArchive {
    param([Parameter(Mandatory)] [string] $ArchivePath, [Parameter(Mandatory)] [string] $Destination)
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Recurse -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Destination -Force
    return $Destination
}

function Get-WingetPath {
    <#
    .SYNOPSIS
        winget.exe, resolved by path rather than by PATH.
    .DESCRIPTION
        winget reaches PATH through an app-execution alias that belongs to the
        interactive user's profile. A PowerShell Direct session frequently does not
        have it, and "winget is not recognized" then reads as "the guest has no
        package manager" when it only means the alias was not in scope.
    #>
    $command = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $package = Get-AppxPackage -AllUsers -Name Microsoft.DesktopAppInstaller -ErrorAction SilentlyContinue |
        Sort-Object Version -Descending | Select-Object -First 1
    if ($package) {
        # Windows 11 provisions App Installer in the image, but registration for a
        # newly created local account may lag its first logon. Force the documented
        # registration path so provisioning never requires opening the Store or a
        # console once just to make the winget alias appear.
        Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe `
            -ErrorAction Stop
        $command = Get-Command winget.exe -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
        $packagedExecutable = Join-Path $package.InstallLocation 'winget.exe'
        if (Test-Path -LiteralPath $packagedExecutable -PathType Leaf) { return $packagedExecutable }
    }

    $candidates = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WindowsApps" -Filter 'winget.exe' -ErrorAction SilentlyContinue
    if ($candidates) { return $candidates[0].FullName }
    $installed = Get-ChildItem -Path "$env:ProgramFiles\WindowsApps" -Filter 'winget.exe' -Recurse -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($installed) { return $installed.FullName }
    throw ('winget.exe was not found and the inbox Microsoft.DesktopAppInstaller package could not be registered. ' +
           'Use Windows 11 installation media that includes App Installer; do not bootstrap an unpinned Store package.')
}

function Install-WingetPackage {
    param([Parameter(Mandatory)] $Package)
    $winget = Get-WingetPath
    $arguments = @(
        'install', '--id', $Package.wingetId, '--version', [string]$Package.version,
        '--exact', '--silent', '--accept-package-agreements', '--accept-source-agreements',
        '--disable-interactivity'
    )
    Write-Line "winget install $($Package.wingetId) $($Package.version)"
    # winget draws progress bars with block characters. There is no console on the
    # other side of PowerShell Direct, so they arrive as pages of mojibake that bury
    # the one line that matters. Keep the output and show it only when it explains
    # a failure.
    $output = & $winget @arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    # 0x8A15002B: already installed at that version. A second provisioning run must
    # not turn that into a failure.
    if ($code -ne 0 -and $code -ne -1978335189) {
        Write-Host $output
        throw "winget exited $code installing $($Package.wingetId) $($Package.version)"
    }
}

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------

function Invoke-PowerStep {
    param($Manifest)
    # A guest that sleeps stops answering PowerShell Direct, and a guest whose
    # display powers off stops producing frames for Output Duplication. Both look
    # exactly like a hung gate from the host.
    & powercfg.exe /change standby-timeout-ac 0
    & powercfg.exe /change monitor-timeout-ac 0
    & powercfg.exe /change hibernate-timeout-ac 0
    & powercfg.exe /hibernate off
    & powercfg.exe /setactive SCHEME_MIN
    return 'sleep, hibernate and display-off disabled; high performance scheme active'
}

function Copy-ReleaseDriverFile {
    <#
    .SYNOPSIS
        Copies one driver file unless the destination already holds those bytes.
    .DESCRIPTION
        Returns whether anything was written. Once the GPU partition is attached the
        guest loads these files, and a loaded DLL cannot be overwritten -- but a
        second provisioning pass has nothing to write anyway. Comparing first turns
        "the file is in use" from a failure into a no-op, and leaves a genuine
        mismatch as the error it is.
    #>
    param([Parameter(Mandatory)] [string] $Source, [Parameter(Mandatory)] [string] $Destination)
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
        if ($sourceHash -eq $destinationHash) { return $false }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    return $true
}

function Copy-ReleaseDriverTree {
    <#
    .SYNOPSIS
        Mirrors a staged driver package into the guest, file by file.
    .DESCRIPTION
        Not a recursive Copy-Item over a removed directory: removing the package
        fails outright once one file in it is loaded, and re-copying identical bytes
        is what a resumed run does.
    #>
    param([Parameter(Mandatory)] [string] $Source, [Parameter(Mandatory)] [string] $Destination)
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }
    foreach ($item in Get-ChildItem -LiteralPath $Source -Recurse -File) {
        $relative = $item.FullName.Substring($Source.Length).TrimStart('')
        $target = Join-Path $Destination $relative
        $directory = Split-Path -Parent $target
        if ($directory -and -not (Test-Path -LiteralPath $directory)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        [void](Copy-ReleaseDriverFile -Source $item.FullName -Destination $target)
    }
}

function Invoke-HostDriverStep {
    param($Manifest)
    <#
        The GPU partition presents the host's adapter, so the guest must run the
        HOST's driver files -- not a driver it downloaded. Windows looks for them in
        System32\HostDriverStore, which is why they are copied rather than installed.

        Copy-VMFile cannot write into System32 or into the guest's DriverStore, so
        the host stages everything under one ordinary directory and this moves it.
    #>
    $staging = $Manifest.paths.hostDriverStaging
    if (-not (Test-Path -LiteralPath $staging)) {
        throw ("no staged host driver files at $staging. Run New-ReleaseVm.ps1 -Only stage-driver from the host " +
               'before provisioning, or the guest has no GPU driver.')
    }

    $repository = Join-Path $staging 'FileRepository'
    $targetRepository = Join-Path $env:SystemRoot 'System32\HostDriverStore\FileRepository'
    if (Test-Path -LiteralPath $repository) {
        if (-not (Test-Path -LiteralPath $targetRepository)) {
            New-Item -ItemType Directory -Path $targetRepository -Force | Out-Null
        }
        foreach ($package in Get-ChildItem -LiteralPath $repository -Directory) {
            $destination = Join-Path $targetRepository $package.Name
            Copy-ReleaseDriverTree -Source $package.FullName -Destination $destination
        }
    }

    $system32Source = Join-Path $staging 'System32'
    $copied = 0
    if (Test-Path -LiteralPath $system32Source) {
        $target = Join-Path $env:SystemRoot 'System32'
        foreach ($file in Get-ChildItem -LiteralPath $system32Source -File) {
            if (Copy-ReleaseDriverFile -Source $file.FullName -Destination (Join-Path $target $file.Name)) { $copied++ }
        }
    }

    $packages = @(Get-ChildItem -LiteralPath $targetRepository -Directory -ErrorAction SilentlyContinue).Count
    return "$packages driver package(s) in HostDriverStore, $copied file(s) into System32"
}

function Invoke-VcredistStep {
    param($Manifest)
    Install-WingetPackage -Package (Get-Package -Manifest $Manifest -Id 'vcredist')
    return 'Visual C++ 2015-2022 x64 runtime installed'
}

function Invoke-PwshStep {
    param($Manifest)
    Install-WingetPackage -Package (Get-Package -Manifest $Manifest -Id 'pwsh')
    return 'PowerShell 7 installed'
}

function Invoke-FfmpegStep {
    param($Manifest)
    Install-WingetPackage -Package (Get-Package -Manifest $Manifest -Id 'ffmpeg')
    return 'ffprobe and ffmpeg installed'
}

function Invoke-PresentmonStep {
    param($Manifest)
    $package = Get-Package -Manifest $Manifest -Id 'presentmon'
    $tools = $Manifest.paths.tools
    $downloaded = Get-VerifiedDownload -Package $package -Directory (Join-Path $Manifest.paths.staging 'downloads')
    if (-not (Test-Path -LiteralPath $tools)) { New-Item -ItemType Directory -Path $tools -Force | Out-Null }
    $destination = Join-Path $tools $package.fileName
    Copy-Item -LiteralPath $downloaded -Destination $destination -Force
    return "PresentMon $($package.version) at $destination"
}

function Invoke-SoundvolumeviewStep {
    param($Manifest)
    $package = Get-Package -Manifest $Manifest -Id 'soundvolumeview'
    $downloaded = Get-VerifiedDownload -Package $package -Directory (Join-Path $Manifest.paths.staging 'downloads')
    $extracted = Expand-VerifiedArchive -ArchivePath $downloaded -Destination (Join-Path $Manifest.paths.staging 'soundvolumeview')
    $tools = $Manifest.paths.tools
    if (-not (Test-Path -LiteralPath $tools)) { New-Item -ItemType Directory -Path $tools -Force | Out-Null }
    $source = Get-ChildItem -LiteralPath $extracted -Filter $package.fileName -Recurse | Select-Object -First 1
    if (-not $source) { throw "the SoundVolumeView archive contains no $($package.fileName)" }
    $destination = Join-Path $tools $package.fileName
    Copy-Item -LiteralPath $source.FullName -Destination $destination -Force
    return "SoundVolumeView $($package.version) at $destination"
}

function Invoke-VbcableStep {
    param($Manifest)
    $package = Get-Package -Manifest $Manifest -Id 'vbcable'
    $downloaded = Get-VerifiedDownload -Package $package -Directory (Join-Path $Manifest.paths.staging 'downloads')
    $extracted = Expand-VerifiedArchive -ArchivePath $downloaded -Destination (Join-Path $Manifest.paths.staging 'vbcable')
    $installer = Get-ChildItem -LiteralPath $extracted -Filter $package.installer -Recurse | Select-Object -First 1
    if (-not $installer) { throw "the VB-CABLE archive contains no $($package.installer)" }

    # -i -h is VB-Audio's documented silent install. It stages a driver package;
    # the endpoint only appears after the guest restarts, which is why this step
    # sets the reboot flag rather than verifying the device now.
    & $installer.FullName -i -h
    $code = $LASTEXITCODE
    if ($code -ne 0 -and $code -ne 3010) {
        throw "VB-CABLE installer exited $code"
    }
    $script:RebootPending = $true
    return "VB-CABLE $($package.version) installed; the endpoint appears after the next restart"
}

function New-VirtualDisplayConfiguration {
    <#
    .SYNOPSIS
        The virtual monitor's mode list, as the driver's settings file.
    .DESCRIPTION
        Element names belong to the pinned driver version, not to this repository.
        Verify them against that release when the version pin is recorded; a mode
        list the driver silently ignores produces a 1024x768 monitor and a pile of
        gates that fail for a reason nobody looks for.
    #>
    param([Parameter(Mandatory)] $Display, [bool] $Hdr)
    $rates = ($Display.refreshRates | ForEach-Object { "        <refresh_rate>$_</refresh_rate>" }) -join "`r`n"
    $hdrValue = if ($Hdr) { 'true' } else { 'false' }
    return @"
<?xml version="1.0" encoding="UTF-8"?>
<vdd_settings>
  <monitors>
    <count>1</count>
  </monitors>
  <resolutions>
    <resolution>
      <width>$($Display.width)</width>
      <height>$($Display.height)</height>
$rates
    </resolution>
  </resolutions>
  <options>
    <HDRPlus>$hdrValue</HDRPlus>
  </options>
</vdd_settings>
"@
}

function Invoke-IddStep {
    param($Manifest)
    $package = Get-Package -Manifest $Manifest -Id 'idd'
    $nefconPackage = Get-Package -Manifest $Manifest -Id 'nefcon'
    $downloaded = Get-VerifiedDownload -Package $package -Directory (Join-Path $Manifest.paths.staging 'downloads')
    $extracted = Expand-VerifiedArchive -ArchivePath $downloaded -Destination (Join-Path $Manifest.paths.staging 'idd')
    $nefconDownload = Get-VerifiedDownload -Package $nefconPackage -Directory (Join-Path $Manifest.paths.staging 'downloads')
    $nefconRoot = Expand-VerifiedArchive -ArchivePath $nefconDownload -Destination (Join-Path $Manifest.paths.staging 'nefcon')

    $configDirectory = $Manifest.display.configDirectory
    if (-not (Test-Path -LiteralPath $configDirectory)) {
        New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
    }
    $hdr = if ($EnableHdr) { $true } else { [bool]$Manifest.display.hdr }
    New-VirtualDisplayConfiguration -Display $Manifest.display -Hdr $hdr |
        Set-Content -LiteralPath (Join-Path $configDirectory 'vdd_settings.xml') -Encoding UTF8

    $inf = Get-ChildItem -LiteralPath $extracted -Filter '*.inf' -Recurse | Select-Object -First 1
    if (-not $inf) { throw 'the virtual display driver archive contains no .inf' }

    $nefcon = Get-ChildItem -LiteralPath (Join-Path $nefconRoot 'x64') -Filter $nefconPackage.fileName |
        Select-Object -First 1
    if (-not $nefcon) { throw "the NefCon archive contains no x64\$($nefconPackage.fileName)" }

    # The driver-only archive contains no installer and pnputil cannot create a
    # root-enumerated device. NefCon's devcon-compatible command creates the node,
    # stages the signed INF and binds it in one operation.
    & $nefcon.FullName install $inf.FullName $package.hardwareId --no-duplicates --remove-duplicates
    $code = $LASTEXITCODE
    if ($code -eq 3010) {
        $script:RebootPending = $true
    }
    elseif ($code -ne 0) {
        throw "NefCon exited $code installing $($inf.Name) for $($package.hardwareId)"
    }

    $modes = ($Manifest.display.refreshRates | ForEach-Object { "$_ Hz" }) -join ', '
    return ("virtual display $($Manifest.display.width)x$($Manifest.display.height) ($modes), " +
            "HDR $(if ($hdr) { 'on' } else { 'off' })")
}

function Invoke-UacStep {
    param($Manifest)
    <#
        Read, never written. The update-accept gate needs elevation without a prompt
        (ConsentPromptBehaviorAdmin = 0) and the update-decline gate needs the product
        to see a declined prompt; a golden image that had already been flipped would
        make the second gate unreachable and nobody would notice, because a machine
        that never prompts also never fails.
    #>
    $path = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System'
    $value = (Get-ItemProperty -LiteralPath $path -Name 'ConsentPromptBehaviorAdmin').ConsentPromptBehaviorAdmin
    Write-Line "ConsentPromptBehaviorAdmin = $value (Windows default is 5)"
    Write-Line 'the harness flips it per run and puts it back:'
    Write-Line "  Set-ItemProperty -LiteralPath '$path' -Name ConsentPromptBehaviorAdmin -Value 0   # elevate silently"
    Write-Line "  Set-ItemProperty -LiteralPath '$path' -Name ConsentPromptBehaviorAdmin -Value $value   # restore"
    return "ConsentPromptBehaviorAdmin left at $value"
}

$script:Steps = @(
    @{ Name = 'power';           Action = ${function:Invoke-PowerStep} }
    @{ Name = 'hostdriver';      Action = ${function:Invoke-HostDriverStep} }
    @{ Name = 'vcredist';        Action = ${function:Invoke-VcredistStep} }
    @{ Name = 'pwsh';            Action = ${function:Invoke-PwshStep} }
    @{ Name = 'ffmpeg';          Action = ${function:Invoke-FfmpegStep} }
    @{ Name = 'presentmon';      Action = ${function:Invoke-PresentmonStep} }
    @{ Name = 'soundvolumeview'; Action = ${function:Invoke-SoundvolumeviewStep} }
    @{ Name = 'vbcable';         Action = ${function:Invoke-VbcableStep} }
    @{ Name = 'idd';             Action = ${function:Invoke-IddStep} }
    @{ Name = 'uac';             Action = ${function:Invoke-UacStep} }
)

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

if ($ListSteps) {
    $script:Steps | ForEach-Object { Write-Output $_.Name }
    exit 0
}

$manifest = Get-Manifest -Path $ManifestPath
if (-not $StatePath) { $StatePath = Join-Path $manifest.paths.staging 'provision-state.json' }
$state = Get-State -Path $StatePath

$selected = $script:Steps
if ($Only.Count -gt 0) {
    $unknown = @($Only | Where-Object { $_ -notin $script:Steps.Name })
    if ($unknown.Count -gt 0) {
        throw "unknown step(s): $($unknown -join ', '). Known steps: $($script:Steps.Name -join ', ')"
    }
    $selected = @($script:Steps | Where-Object { $Only -contains $_.Name })
}

Write-Host ''
Write-Host "ExoSnap release-verification guest provisioning ($($selected.Count) step(s))"
Write-Host ''

$failed = 0
foreach ($step in $selected) {
    $name = $step.Name
    if (-not $Force -and $state.ContainsKey($name) -and $state[$name].state -eq 'DONE') {
        Write-Line "$name -- $($state[$name].detail)" 'skip'
        continue
    }
    Write-Host "  $name"
    try {
        $detail = & $step.Action -Manifest $manifest
        $state[$name] = @{ state = 'DONE'; at = (Get-Date).ToString('o'); detail = "$detail" }
        Save-State -Path $StatePath -State $state
        Write-Line "$name -- $detail" 'ok'
    }
    catch {
        $state[$name] = @{ state = 'FAILED'; at = (Get-Date).ToString('o'); detail = "$($_.Exception.Message)" }
        Save-State -Path $StatePath -State $state
        Write-Line "$name -- $($_.Exception.Message)" 'fail'
        $failed++
        break
    }
}

Write-Host ''
if ($failed -gt 0) {
    Write-Host "  provisioning stopped on a failed step. Fix it and run this script again; " -NoNewline
    Write-Host 'the completed steps are skipped.'
    exit 1
}
if ($script:RebootPending) {
    Write-Host '  a driver was staged that needs a restart. Restart the guest, then run this script again.'
    exit 2
}
Write-Host '  provisioning complete.'
exit 0
