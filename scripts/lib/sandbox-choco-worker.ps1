#Requires -Version 7.0
<#
.SYNOPSIS
    The Chocolatey rehearsal, executed inside a clean Windows Sandbox.

.DESCRIPTION
    A thin bootstrap around scripts/lib/choco-rehearsal-worker.ps1, which already
    packs a rewritten copy of the package, removes the installed ExoSnap, installs
    the package, uninstalls it and reinstalls the release MSI. What changes here is
    only WHERE that happens.

    On the developer's machine the rehearsal is the one gate that installs software:
    it uninstalls their ExoSnap, may upgrade their Visual C++ redistributable, and
    puts the release MSI back from a finally block so the machine is usable
    afterwards. In a sandbox none of that is a concession -- the machine is discarded
    when the run ends, so the rehearsal measures the package's real effect on a
    machine instead of its effect on a machine that already had everything.

    The MSI is installed first for the same reason it is reinstalled last on a real
    machine: the rehearsal asserts that the package's uninstall leaves
    %LOCALAPPDATA%\ExoSnap alone, and that directory has to exist and have content
    before "unchanged" means anything.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $StagingDirectory,
    [Parameter(Mandatory)] [string] $PackageSource,
    [Parameter(Mandatory)] [string] $MsiPath,
    [Parameter(Mandatory)] [string] $MsiSha256,
    [Parameter(Mandatory)] [string] $EvidenceDirectory,
    [Parameter(Mandatory)] [string] $ResultPath,
    [Parameter(Mandatory)] [string] $MarkerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$logDirectory = Join-Path $StagingDirectory 'logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null

function Write-BootstrapFailure {
    param([Parameter(Mandatory)] [string] $Step, [Parameter(Mandatory)] [string] $Detail)
    # Written in the rehearsal worker's own document shape so the host reads one
    # format. kind is 'bootstrap' for all of them, which is what these are: the
    # release MSI, Chocolatey itself and this script's own failures are the test
    # environment being built, and none of them measured the package. Without the
    # field the host cannot attribute the failure and refuses to draw a verdict --
    # which is correct, but it is not the same as saying what actually happened.
    $document = [pscustomobject]@{
        finishedUtc = [DateTime]::UtcNow.ToString('o')
        fatal       = $Detail
        steps       = @([pscustomobject]@{ name = $Step; ok = $false; detail = $Detail; kind = 'bootstrap' })
    }
    Set-Content -LiteralPath $ResultPath -Value ($document | ConvertTo-Json -Depth 12) -Encoding utf8NoBOM
}

try {
    # ExoSnap first: the uninstall assertion is about a user configuration
    # directory that already exists, and a sandbox has none until the product has
    # run once.
    $installLog = Join-Path $logDirectory 'install-release-msi.log'
    $install = Start-Process -FilePath 'msiexec.exe' -Wait -PassThru `
        -ArgumentList "/i `"$MsiPath`" /qn /l*v `"$installLog`""
    if ($install.ExitCode -ne 0) {
        Write-BootstrapFailure -Step 'sandbox-install-release' `
            -Detail "msiexec exited $($install.ExitCode) installing the release MSI; see logs/install-release-msi.log"
        return
    }

    $userConfig = Join-Path $env:LOCALAPPDATA 'ExoSnap'
    if (-not (Test-Path -LiteralPath $userConfig)) {
        # One launch, ended immediately: the directory is created on first run, and
        # the rehearsal needs something to compare file by file. --smoke-test exits
        # on its own, so nothing is left running and no window takes the desktop.
        $exe = 'C:\Program Files\Codexo\ExoSnap\exosnap.exe'
        if (Test-Path -LiteralPath $exe) {
            $smoke = Start-Process -FilePath $exe -ArgumentList '--smoke-test' -PassThru -Wait
            [void]$smoke
        }
    }
    if (-not (Test-Path -LiteralPath $userConfig)) {
        New-Item -ItemType Directory -Path $userConfig -Force | Out-Null
    }

    if ($null -eq (Get-Command choco -ErrorAction SilentlyContinue)) {
        $bootstrapLog = Join-Path $logDirectory 'install-chocolatey.log'
        try {
            Set-ExecutionPolicy Bypass -Scope Process -Force
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $script = (New-Object Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1')
            Set-Content -LiteralPath $bootstrapLog -Value 'downloaded the Chocolatey installer' -Encoding utf8NoBOM
            Invoke-Expression $script
        }
        catch {
            Write-BootstrapFailure -Step 'sandbox-install-chocolatey' `
                -Detail "Chocolatey could not be installed in the sandbox: $($_.Exception.Message)"
            return
        }
        $env:PATH = "$env:PATH;$env:ALLUSERSPROFILE\chocolatey\bin"
    }
    if ($null -eq (Get-Command choco -ErrorAction SilentlyContinue)) {
        Write-BootstrapFailure -Step 'sandbox-install-chocolatey' -Detail 'choco is still not on PATH after the bootstrap'
        return
    }

    & (Join-Path $StagingDirectory 'choco-rehearsal-worker.ps1') `
        -PackageSource $PackageSource -MsiPath $MsiPath -MsiSha256 $MsiSha256 `
        -UserConfigDirectory $userConfig -EvidenceDirectory $EvidenceDirectory -ResultPath $ResultPath
}
catch {
    Write-BootstrapFailure -Step 'sandbox-choco-worker' -Detail "$($_.Exception.Message)"
}
finally {
    Set-Content -LiteralPath $MarkerPath -Value ([DateTime]::UtcNow.ToString('o')) -Encoding utf8NoBOM
}
