#Requires -Version 7.0
<#
.SYNOPSIS
    The elevated half of REL-PKG-CHOCO-001: pack, install, uninstall, restore.

.DESCRIPTION
    Chocolatey installs machine-wide and every step here ends in msiexec. A silent
    msiexec run from an unelevated process does not raise a prompt at all -- it
    fails with 1603 and "no credential elevation is possible" -- so the whole
    rehearsal has to happen inside ONE elevated child rather than as a sequence of
    elevated calls, each of which would ask again.

    This script does the doing and records what it observed; the non-elevated
    runner reads the JSON and decides. That split is what keeps a gate's verdict
    out of a process nobody can see the output of.

    Nothing in packaging/chocolatey is modified. The package is copied to a
    temporary directory and only the copy is pointed at the local MSI, because the
    tracked checksum describes a file that does not exist until the release is
    published.

    The machine is left as it was found: the release MSI is reinstalled at the end,
    which every later gate in the campaign depends on.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageSource,
    [Parameter(Mandatory)] [string] $MsiPath,
    [Parameter(Mandatory)] [string] $MsiSha256,
    [Parameter(Mandatory)] [string] $EvidenceDirectory,
    [Parameter(Mandatory)] [string] $ResultPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseScenarios.ps1')

$installDirectory = 'C:\Program Files\Codexo\ExoSnap'
$vendorDirectory = 'C:\Program Files\Codexo'
$installedExe = Join-Path $installDirectory 'exosnap.exe'
$productKey = 'HKLM:\SOFTWARE\Codexo\ExoSnap'
$vendorKey = 'HKLM:\SOFTWARE\Codexo'
$shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\ExoSnap.lnk'
$chocoLibrary = 'C:\ProgramData\chocolatey\lib\exosnap'
$userConfig = Join-Path $env:LOCALAPPDATA 'ExoSnap'

$script:Steps = @()

function New-Step {
    param([Parameter(Mandatory)] [string] $Name)
    return [ordered]@{ name = $Name; ok = $true; detail = ''; assertions = @(); failedAssertions = @() }
}

function Add-Assertion {
    <#
    .SYNOPSIS
        Records one assertion on a step, passing or failing.
    .DESCRIPTION
        Both outcomes are recorded, not only the failures: a step that passed three
        assertions and a step that ran none look identical in a result that keeps
        only what went wrong, and the runner's verdict counts them.
    #>
    param(
        [Parameter(Mandatory)] $Step,
        [Parameter(Mandatory)] [string] $Text,
        [Parameter(Mandatory)] [bool] $Condition
    )
    $Step.assertions += $Text
    if (-not $Condition) {
        $Step.ok = $false
        $Step.failedAssertions += $Text
    }
}

function Complete-Step {
    param([Parameter(Mandatory)] $Step)
    $script:Steps += [pscustomobject]$Step
    return $Step.ok
}

function Invoke-Recorded {
    <#
    .SYNOPSIS
        Runs one external command to completion, captures everything it said into
        the evidence directory, and returns its exit code.
    .DESCRIPTION
        Start-Process -Wait rather than the call operator, because msiexec.exe is a
        GUI-subsystem executable: `& msiexec.exe /qn ...` returns the moment the
        process is created, and $LASTEXITCODE then describes nothing. Every
        assertion after an install would run against a machine the installer had
        not finished changing.
    #>
    param(
        [Parameter(Mandatory)] [string] $LogName,
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [string[]] $Arguments
    )
    $log = Join-Path $EvidenceDirectory $LogName
    $errorLog = "$log.err"
    Set-Content -LiteralPath $log -Value "$FilePath $($Arguments -join ' ')" -Encoding utf8NoBOM
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput "$log.out" -RedirectStandardError $errorLog
    foreach ($part in @("$log.out", $errorLog)) {
        if (Test-Path -LiteralPath $part) {
            Add-Content -LiteralPath $log -Value (Get-Content -LiteralPath $part -Raw)
            Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        }
    }
    return $process.ExitCode
}

function Get-ExoSnapArpEntry {
    <#
    .SYNOPSIS
        The Add/Remove Programs entry for ExoSnap, or $null.
    .DESCRIPTION
        Both registry views are searched. WiX writes the 64-bit one for this
        package today, and a lookup that only knows the view it expects would
        report a clean uninstall for an entry it never looked at.
    #>
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $properties = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            if ($null -eq $properties) { continue }
            $name = $properties.PSObject.Properties.Name -contains 'DisplayName' ? "$($properties.DisplayName)" : ''
            if ($name -notlike 'ExoSnap*') { continue }
            return [pscustomobject]@{
                ProductCode    = $key.PSChildName
                DisplayName    = $name
                DisplayVersion = $properties.PSObject.Properties.Name -contains 'DisplayVersion' ?
                "$($properties.DisplayVersion)" : ''
            }
        }
    }
    return $null
}

function Measure-DirectoryContent {
    <#
    .SYNOPSIS
        File count and total bytes of a directory tree, or $null when it is absent.
    .DESCRIPTION
        The user's configuration must survive an uninstall untouched, and "the
        directory still exists" does not say that -- an uninstall that emptied it
        would satisfy it. Count and bytes together do.
    #>
    param([Parameter(Mandatory)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $files = @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue)
    $bytes = 0
    foreach ($file in $files) { $bytes += $file.Length }
    return [pscustomobject]@{ files = $files.Count; bytes = $bytes }
}

function Get-ProductValue {
    param([Parameter(Mandatory)] [string] $Name)
    if (-not (Test-Path -LiteralPath $productKey)) { return $null }
    $properties = Get-ItemProperty -LiteralPath $productKey -ErrorAction SilentlyContinue
    if ($null -eq $properties -or $properties.PSObject.Properties.Name -notcontains $Name) { return $null }
    return $properties.$Name
}

$workDirectory = Join-Path ([IO.Path]::GetTempPath()) "exosnap-choco-rehearsal-$([guid]::NewGuid().ToString('n'))"
$aborted = $false

try {
    New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $workDirectory -Force | Out-Null

    # ---- prepare: the package copy, pointed at the local MSI ----
    $step = New-Step -Name 'prepare'
    $trackedInstall = Join-Path $PackageSource 'tools/chocolateyinstall.ps1'
    $trackedBefore = (Get-FileHash -LiteralPath $trackedInstall -Algorithm SHA256).Hash
    $copy = New-ReleaseChocolateyPackageCopy -SourceDirectory $PackageSource -DestinationDirectory $workDirectory `
        -MsiPath $MsiPath -Sha256 $MsiSha256
    Add-Assertion -Step $step -Text 'the package copy could be rewritten' -Condition ([bool]$copy.Ok)
    if ($copy.Ok) {
        $rewritten = Get-Content -LiteralPath $copy.InstallScript -Raw
        Add-Assertion -Step $step -Text 'url64bit points at the local MSI' `
            -Condition ($rewritten -match [regex]::Escape($MsiPath))
        Add-Assertion -Step $step -Text 'checksum64 is the local MSI sha256' `
            -Condition ($rewritten -match [regex]::Escape($MsiSha256))
    }
    else {
        $step.detail = "$($copy.Detail)"
    }
    Add-Assertion -Step $step -Text 'the tracked chocolateyinstall.ps1 was not modified' `
        -Condition ((Get-FileHash -LiteralPath $trackedInstall -Algorithm SHA256).Hash -eq $trackedBefore)
    if (-not (Complete-Step -Step $step)) { $aborted = $true }

    # ---- pack ----
    if (-not $aborted) {
        $step = New-Step -Name 'pack'
        $nuspec = Join-Path $copy.PackageDirectory 'exosnap.nuspec'
        $code = Invoke-Recorded -LogName 'choco-pack.log' -FilePath 'choco' `
            -Arguments @('pack', $nuspec, '--out', $workDirectory)
        Add-Assertion -Step $step -Text "choco pack exits 0 (was $code)" -Condition ($code -eq 0)
        $packages = @(Get-ChildItem -LiteralPath $workDirectory -Filter '*.nupkg' -File -ErrorAction SilentlyContinue)
        Add-Assertion -Step $step -Text "exactly one .nupkg was produced (was $($packages.Count))" `
            -Condition ($packages.Count -eq 1)
        if ($packages.Count -eq 1) { $step.detail = $packages[0].Name }
        if (-not (Complete-Step -Step $step)) { $aborted = $true }
    }

    # ---- removeExisting: an installed same-version product blocks the install ----
    # msiexec refuses a package whose ProductVersion is already installed (1638),
    # so the rehearsal cannot begin against the release it is rehearsing without
    # taking that install off first. What it removed is recorded, because the
    # restore step has to put the same thing back.
    $configBefore = $null
    if (-not $aborted) {
        $step = New-Step -Name 'removeExisting'
        $configBefore = Measure-DirectoryContent -Path $userConfig
        $existing = Get-ExoSnapArpEntry
        if ($null -eq $existing) {
            $step.detail = 'no ExoSnap was installed'
        }
        else {
            $step.detail = "$($existing.DisplayName) $($existing.DisplayVersion) ($($existing.ProductCode))"
            $code = Invoke-Recorded -LogName 'msiexec-remove.log' -FilePath 'msiexec.exe' `
                -Arguments @('/x', $existing.ProductCode, '/qn', '/norestart', '/l*v',
                (Join-Path $EvidenceDirectory 'msiexec-remove-verbose.log'))
            Add-Assertion -Step $step -Text "msiexec /x exits 0 (was $code)" -Condition ($code -eq 0)
            Add-Assertion -Step $step -Text 'the ARP entry is gone before the package install' `
                -Condition ($null -eq (Get-ExoSnapArpEntry))
        }
        if (-not (Complete-Step -Step $step)) { $aborted = $true }
    }

    # ---- install through Chocolatey ----
    if (-not $aborted) {
        $step = New-Step -Name 'install'
        $code = Invoke-Recorded -LogName 'choco-install.log' -FilePath 'choco' `
            -Arguments @('install', 'exosnap', '--source', $workDirectory, '-y', '--no-progress')
        Add-Assertion -Step $step -Text "choco install exits 0 (was $code)" -Condition ($code -eq 0)
        Add-Assertion -Step $step -Text "$installedExe exists" -Condition (Test-Path -LiteralPath $installedExe)
        $installed = Get-ExoSnapArpEntry
        Add-Assertion -Step $step -Text 'an ExoSnap ARP entry exists' -Condition ($null -ne $installed)
        if ($null -ne $installed) { $step.detail = "$($installed.DisplayName) $($installed.DisplayVersion)" }
        Add-Assertion -Step $step -Text "$productKey reports installed=1" `
            -Condition ("$(Get-ProductValue -Name 'installed')" -eq '1')
        Add-Assertion -Step $step -Text "$productKey carries an InstallPath" `
            -Condition (-not [string]::IsNullOrWhiteSpace("$(Get-ProductValue -Name 'InstallPath')"))
        Add-Assertion -Step $step -Text 'the start-menu shortcut exists' -Condition (Test-Path -LiteralPath $shortcut)
        Add-Assertion -Step $step -Text 'the Chocolatey lib directory exists' `
            -Condition (Test-Path -LiteralPath $chocoLibrary)
        if (-not (Complete-Step -Step $step)) { $aborted = $true }
    }

    # ---- uninstall, and the residue that must not survive it ----
    if (-not $aborted) {
        $step = New-Step -Name 'uninstall'
        $code = Invoke-Recorded -LogName 'choco-uninstall.log' -FilePath 'choco' `
            -Arguments @('uninstall', 'exosnap', '-y')
        Add-Assertion -Step $step -Text "choco uninstall exits 0 (was $code)" -Condition ($code -eq 0)
        Add-Assertion -Step $step -Text "$installedExe is gone" -Condition (-not (Test-Path -LiteralPath $installedExe))
        Add-Assertion -Step $step -Text "$installDirectory is gone" -Condition (-not (Test-Path -LiteralPath $installDirectory))
        # The vendor directory and the vendor key are asserted too: a measured
        # uninstall of this MSI removes both, so leaving either behind is a
        # regression rather than acceptable residue.
        Add-Assertion -Step $step -Text "$vendorDirectory is gone" -Condition (-not (Test-Path -LiteralPath $vendorDirectory))
        Add-Assertion -Step $step -Text 'the ARP entry is gone' -Condition ($null -eq (Get-ExoSnapArpEntry))
        Add-Assertion -Step $step -Text 'the start-menu shortcut is gone' -Condition (-not (Test-Path -LiteralPath $shortcut))
        Add-Assertion -Step $step -Text "$productKey is gone" -Condition (-not (Test-Path -LiteralPath $productKey))
        Add-Assertion -Step $step -Text "$vendorKey is gone" -Condition (-not (Test-Path -LiteralPath $vendorKey))
        Add-Assertion -Step $step -Text 'the Chocolatey lib directory is gone' `
            -Condition (-not (Test-Path -LiteralPath $chocoLibrary))
        # The user's own configuration is not the installer's to remove. Compared by
        # file count and total bytes rather than by existence: an uninstall that
        # emptied the directory would pass an existence check.
        $configAfter = Measure-DirectoryContent -Path $userConfig
        $configUnchanged = ($null -eq $configBefore -and $null -eq $configAfter) -or
            ($null -ne $configBefore -and $null -ne $configAfter -and
            $configBefore.files -eq $configAfter.files -and $configBefore.bytes -eq $configAfter.bytes)
        $before = if ($null -eq $configBefore) { 'absent' } else { "$($configBefore.files) files / $($configBefore.bytes) bytes" }
        $after = if ($null -eq $configAfter) { 'absent' } else { "$($configAfter.files) files / $($configAfter.bytes) bytes" }
        Add-Assertion -Step $step -Text "$userConfig is untouched ($before -> $after)" -Condition $configUnchanged
        [void](Complete-Step -Step $step)
    }

    # ---- restore: the campaign's later gates expect the release installed ----
    # Run whatever happened above, including a failed install: a rehearsal that
    # aborted halfway is exactly the case where the machine most needs putting
    # back, and leaving it uninstalled would fail every gate after this one for a
    # reason that belongs here.
    $step = New-Step -Name 'restore'
    $code = Invoke-Recorded -LogName 'msiexec-restore.log' -FilePath 'msiexec.exe' `
        -Arguments @('/i', $MsiPath, '/qn', '/norestart', '/l*v',
        (Join-Path $EvidenceDirectory 'msiexec-restore-verbose.log'))
    Add-Assertion -Step $step -Text "msiexec /i exits 0 (was $code)" -Condition ($code -eq 0)
    $restored = Get-ExoSnapArpEntry
    # DisplayVersion is deliberately not compared against the release tag: an MSI
    # ProductVersion cannot carry a prerelease suffix, so an rc build legitimately
    # reports the plain three-part version here.
    Add-Assertion -Step $step -Text 'the ExoSnap ARP entry is back' -Condition ($null -ne $restored)
    Add-Assertion -Step $step -Text "$installedExe is back" -Condition (Test-Path -LiteralPath $installedExe)
    if ($null -ne $restored) { $step.detail = "$($restored.DisplayName) $($restored.DisplayVersion)" }
    [void](Complete-Step -Step $step)
}
catch {
    $step = New-Step -Name 'worker'
    Add-Assertion -Step $step -Text "the worker completed without throwing: $($_.Exception.Message)" -Condition $false
    [void](Complete-Step -Step $step)
}
finally {
    Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
    $failedSteps = @($script:Steps | Where-Object { -not $_.ok })
    $result = [ordered]@{
        ok           = ($failedSteps.Count -eq 0)
        msiPath      = $MsiPath
        msiSha256    = $MsiSha256
        completedUtc = [DateTime]::UtcNow.ToString('o')
        steps        = @($script:Steps)
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $ResultPath) -Force | Out-Null
    Set-Content -LiteralPath $ResultPath -Value ($result | ConvertTo-Json -Depth 10) -Encoding utf8NoBOM
}

# The exit code says the worker RAN, not that the rehearsal passed: the verdict is
# the runner's to make from the JSON, and an exit code that also carried the
# verdict would let a worker that never wrote a result look like a failed step.
exit 0
