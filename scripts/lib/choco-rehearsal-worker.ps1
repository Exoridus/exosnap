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

    WHAT IS PUT BACK AND WHAT IS NOT. The release MSI is reinstalled in a `finally`
    block, whatever happened before it, because every later gate in the campaign
    expects ExoSnap installed and a half-finished rehearsal is exactly when that
    matters most. The Visual C++ redistributable is NOT put back: `vcredist140` is
    a declared Chocolatey dependency of this package, so the install can install or
    upgrade it, and downgrading a machine's C++ runtime to undo that would be worse
    than the change. Its version is recorded before and after so the verdict can
    say what happened.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PackageSource,
    [Parameter(Mandatory)] [string] $MsiPath,
    [Parameter(Mandatory)] [string] $MsiSha256,
    # The CALLER's %LOCALAPPDATA%\ExoSnap. Never read from this process's own
    # environment: an elevated child started from a different account resolves
    # LOCALAPPDATA to that account, and the uninstall would then be judged against
    # a directory ExoSnap has never written to.
    [Parameter(Mandatory)] [string] $UserConfigDirectory,
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
# Chocolatey's own root, not a hardcoded ProgramData path: a machine can install
# it elsewhere, and a "the lib directory is gone" assertion against a path
# Chocolatey never used passes for the wrong reason.
$chocoRoot = if ([string]::IsNullOrWhiteSpace($env:ChocolateyInstall)) { 'C:\ProgramData\chocolatey' }
else { $env:ChocolateyInstall }
$chocoLibrary = Join-Path $chocoRoot 'lib/exosnap'
# The community feed is required, not optional: the package declares a
# vcredist140 dependency that a directory source alone cannot resolve, and the
# install then fails with a dependency error rather than a packaging finding.
$chocoSourceSuffix = ';https://community.chocolatey.org/api/v2/'

$script:Steps = @()
$script:Observations = @()

function New-Step {
    <#
    .SYNOPSIS
        Start a step, saying whether it asserts the package or builds the test.
    .DESCRIPTION
        `Kind` is mandatory and has no default. The host refuses to draw a verdict
        from a step that does not say, because an unclassified step is
        indistinguishable from either kind -- and reading it as a product
        assertion would let this worker's own setup problem be reported as a
        defect in the package.

        product:   what the rehearsal asserts about the Chocolatey package.
        bootstrap: packing, resolving a source, putting the machine back. A
                   failure here measured nothing about the package.
    #>
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [ValidateSet('product', 'bootstrap')] [string] $Kind
    )
    return [ordered]@{
        name             = $Name
        ok               = $true
        detail           = ''
        kind             = $Kind
        assertions       = @()
        failedAssertions = @()
    }
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

function Add-Observation {
    <#
    .SYNOPSIS
        Records something the rehearsal saw but does not require.
    .DESCRIPTION
        The empty manufacturer folder and its registry key are in this class. WiX
        generates no RemoveFolder row for the manufacturer folder -- it owns no
        component -- so an uninstall that leaves the empty parent behind is
        within what the package promises, even though this machine was measured
        clean. Asserting on it would fail a correct package on the next machine.
    #>
    param([Parameter(Mandatory)] [string] $Text)
    $script:Observations += $Text
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

        Every argument is quoted here rather than at the call sites. Start-Process
        joins ArgumentList with spaces and quotes nothing, so one unquoted path
        containing a space silently becomes two arguments.
    #>
    param(
        [Parameter(Mandatory)] [string] $LogName,
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [string[]] $Arguments
    )
    $quoted = @($Arguments | ForEach-Object {
            if ("$_".StartsWith('-') -or "$_".StartsWith('/')) { "$_" } else { '"{0}"' -f ("$_" -replace '"', '\"') }
        })
    $log = Join-Path $EvidenceDirectory $LogName
    $errorLog = "$log.err"
    Set-Content -LiteralPath $log -Value "$FilePath $($quoted -join ' ')" -Encoding utf8NoBOM
    $process = Start-Process -FilePath $FilePath -ArgumentList $quoted -Wait -PassThru -NoNewWindow `
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
        The Add/Remove Programs entry for ExoSnap, or $null. Throws on ambiguity.
    .DESCRIPTION
        Both registry views are searched, because a lookup that only knows the view
        it expects reports a clean uninstall for an entry it never looked at.

        `DisplayName -like 'ExoSnap*'` alone is not an identification -- it matches
        "ExoSnap Helper" by anyone -- so Publisher and a GUID-shaped key (the MSI
        ProductCode) are required too. Two matches abort the whole rehearsal before
        any msiexec runs: picking one and uninstalling it is how an unrelated
        product disappears from a developer's machine.
    #>
    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    # Not $matches: that name is PowerShell's automatic regex-capture variable.
    $found = @()
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($key in @(Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            if ($key.PSChildName -notmatch '^\{[0-9A-Fa-f-]{36}\}$') { continue }
            $properties = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            if ($null -eq $properties) { continue }
            $names = $properties.PSObject.Properties.Name
            $displayName = if ($names -contains 'DisplayName') { "$($properties.DisplayName)" } else { '' }
            $publisher = if ($names -contains 'Publisher') { "$($properties.Publisher)" } else { '' }
            if ($displayName -notlike 'ExoSnap*' -or $publisher -ne 'Codexo') { continue }
            $found += [pscustomobject]@{
                ProductCode    = $key.PSChildName
                DisplayName    = $displayName
                Publisher      = $publisher
                DisplayVersion = if ($names -contains 'DisplayVersion') { "$($properties.DisplayVersion)" } else { '' }
                InstallLocation = if ($names -contains 'InstallLocation') { "$($properties.InstallLocation)" } else { '' }
            }
        }
    }
    if ($found.Count -gt 1) {
        throw ("$($found.Count) ExoSnap products are registered " +
            "($(($found | ForEach-Object { "$($_.DisplayName) $($_.DisplayVersion) $($_.ProductCode)" }) -join ', ')); " +
            'refusing to guess which one to remove')
    }
    if ($found.Count -eq 0) { return $null }
    return $found[0]
}

function Get-DirectoryManifest {
    <#
    .SYNOPSIS
        Relative path -> size for every file under a directory, or $null.
    .DESCRIPTION
        A count and a byte total say THAT something changed; a manifest says WHICH
        file did, which is the difference between a finding somebody can act on and
        one somebody has to reproduce.
    #>
    param([Parameter(Mandatory)] [string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $manifest = @{}
    $root = (Get-Item -LiteralPath $Path).FullName
    foreach ($file in @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        $manifest[$file.FullName.Substring($root.Length).TrimStart('\')] = $file.Length
    }
    return $manifest
}

function Compare-DirectoryManifest {
    <#
    .SYNOPSIS
        The differences between two manifests, as readable lines.
    #>
    param($Before, $After)
    if ($null -eq $Before -and $null -eq $After) { return @() }
    if ($null -eq $Before) { return @('the directory did not exist before and does now') }
    if ($null -eq $After) { return @('the directory was removed') }
    $differences = @()
    foreach ($name in @($Before.Keys | Sort-Object)) {
        if (-not $After.ContainsKey($name)) { $differences += "removed: $name" }
        elseif ($After[$name] -ne $Before[$name]) {
            $differences += "changed: $name ($($Before[$name]) -> $($After[$name]) bytes)"
        }
    }
    foreach ($name in @($After.Keys | Sort-Object)) {
        if (-not $Before.ContainsKey($name)) { $differences += "added: $name" }
    }
    return $differences
}

function Get-ChocolateyPackageVersion {
    <#
    .SYNOPSIS
        The installed version of one Chocolatey package, or '' when it has none.
    #>
    param([Parameter(Mandatory)] [string] $Id)
    try {
        $listed = & choco list --exact $Id --limit-output 2>$null
        foreach ($line in @($listed)) {
            $parts = "$line" -split '\|'
            if ($parts.Count -ge 2 -and $parts[0] -eq $Id) { return "$($parts[1])" }
        }
    }
    catch { }
    return ''
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
$restoreRan = $false
$restoreExitCode = $null
$vcredistBefore = ''
$vcredistAfter = ''
$configBefore = $null
$copy = $null
# Whether anything on this machine has been changed yet. The restore below is
# unconditional in the sense that it always REPORTS, but reinstalling a product
# that was never removed fails with 1638 and would turn "nothing happened" into a
# failed step.
$machineTouched = $false

try {
    New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $workDirectory -Force | Out-Null
    $vcredistBefore = Get-ChocolateyPackageVersion -Id 'vcredist140'

    # ---- prepare: the package copy, pointed at the local MSI ----
    $step = New-Step -Name 'prepare' -Kind 'bootstrap'
    $configBefore = Get-DirectoryManifest -Path $UserConfigDirectory
    Add-Assertion -Step $step -Text "$UserConfigDirectory exists and is not empty" `
        -Condition ($null -ne $configBefore -and $configBefore.Count -gt 0)
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
        $step = New-Step -Name 'pack' -Kind 'bootstrap'
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
    if (-not $aborted) {
        $step = New-Step -Name 'removeExisting' -Kind 'bootstrap'
        $existing = Get-ExoSnapArpEntry
        if ($null -eq $existing) {
            $step.detail = 'no ExoSnap was installed'
        }
        else {
            $step.detail = "$($existing.DisplayName) $($existing.DisplayVersion) ($($existing.ProductCode))"
            $machineTouched = $true
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
        $step = New-Step -Name 'install' -Kind 'product'
        $machineTouched = $true
        $code = Invoke-Recorded -LogName 'choco-install.log' -FilePath 'choco' `
            -Arguments @('install', 'exosnap', '--source', "$workDirectory$chocoSourceSuffix", '-y', '--no-progress')
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
        $step = New-Step -Name 'uninstall' -Kind 'product'
        $code = Invoke-Recorded -LogName 'choco-uninstall.log' -FilePath 'choco' `
            -Arguments @('uninstall', 'exosnap', '-y')
        Add-Assertion -Step $step -Text "choco uninstall exits 0 (was $code)" -Condition ($code -eq 0)
        Add-Assertion -Step $step -Text "$installedExe is gone" -Condition (-not (Test-Path -LiteralPath $installedExe))
        Add-Assertion -Step $step -Text "$installDirectory is gone" -Condition (-not (Test-Path -LiteralPath $installDirectory))
        Add-Assertion -Step $step -Text 'the ARP entry is gone' -Condition ($null -eq (Get-ExoSnapArpEntry))
        Add-Assertion -Step $step -Text 'the start-menu shortcut is gone' -Condition (-not (Test-Path -LiteralPath $shortcut))
        Add-Assertion -Step $step -Text "$productKey is gone" -Condition (-not (Test-Path -LiteralPath $productKey))
        Add-Assertion -Step $step -Text 'the Chocolatey lib directory is gone' `
            -Condition (-not (Test-Path -LiteralPath $chocoLibrary))
        # The empty manufacturer folder and key: recorded, never required. See
        # Add-Observation for why the package does not promise their removal.
        Add-Observation -Text ("$vendorDirectory after uninstall: " +
            $(if (Test-Path -LiteralPath $vendorDirectory) { 'still present (empty parent, not owned by the package)' } else { 'gone' }))
        Add-Observation -Text ("$vendorKey after uninstall: " +
            $(if (Test-Path -LiteralPath $vendorKey) { 'still present (empty parent key)' } else { 'gone' }))
        # The user's own configuration is not the installer's to remove. Compared
        # file by file rather than by totals, so a changed file is named.
        $configAfter = Get-DirectoryManifest -Path $UserConfigDirectory
        $differences = @(Compare-DirectoryManifest -Before $configBefore -After $configAfter)
        $summary = if ($differences.Count -eq 0) { 'unchanged' } else { $differences -join '; ' }
        Add-Assertion -Step $step -Text "$UserConfigDirectory is untouched ($summary)" `
            -Condition ($differences.Count -eq 0)
        [void](Complete-Step -Step $step)
    }
}
catch {
    $step = New-Step -Name 'worker' -Kind 'bootstrap'
    Add-Assertion -Step $step -Text "the worker completed without throwing: $($_.Exception.Message)" -Condition $false
    [void](Complete-Step -Step $step)
}
finally {
    # ---- restore: the campaign's later gates expect the release installed ----
    # In `finally`, and exactly once. A rehearsal that threw halfway -- an ambiguous
    # ARP entry, a copy that could not be written, a killed choco -- is precisely
    # the case where the machine is most likely to be left without ExoSnap, and a
    # restore that only runs on the happy path is a restore that never runs when it
    # is needed.
    try {
        $step = New-Step -Name 'restore' -Kind 'bootstrap'
        if ($machineTouched) {
            $restoreExitCode = Invoke-Recorded -LogName 'msiexec-restore.log' -FilePath 'msiexec.exe' `
                -Arguments @('/i', $MsiPath, '/qn', '/norestart', '/l*v',
                (Join-Path $EvidenceDirectory 'msiexec-restore-verbose.log'))
            $restoreRan = $true
            Add-Assertion -Step $step -Text "msiexec /i exits 0 (was $restoreExitCode)" `
                -Condition ($restoreExitCode -eq 0)
            $restored = Get-ExoSnapArpEntry
            # DisplayVersion is deliberately not compared against the release tag: an
            # MSI ProductVersion cannot carry a prerelease suffix, so an rc build
            # legitimately reports the plain three-part version here.
            Add-Assertion -Step $step -Text 'the ExoSnap ARP entry is back' -Condition ($null -ne $restored)
            Add-Assertion -Step $step -Text "$installedExe is back" -Condition (Test-Path -LiteralPath $installedExe)
            # The package manager's standard answer to "where is this installed".
            # Asserted because it is written from a property the package sets, so a
            # silent loss of it would otherwise only surface in someone else's tool.
            Add-Assertion -Step $step -Text 'the ARP entry names its install location' `
                -Condition ($null -ne $restored -and -not [string]::IsNullOrWhiteSpace($restored.InstallLocation))
            if ($null -ne $restored) { $step.detail = "$($restored.DisplayName) $($restored.DisplayVersion)" }
        }
        else {
            # Reported rather than skipped: an absent `restore` step reads as "the
            # worker stopped early", which is a different and untrue statement. And
            # reinstalling a product that was never removed fails with 1638.
            $step.detail = 'nothing was installed or removed, so there was nothing to put back'
            $restoreRan = $true
            $restoreExitCode = 0
        }
        [void](Complete-Step -Step $step)
    }
    catch {
        $step = New-Step -Name 'restore' -Kind 'bootstrap'
        Add-Assertion -Step $step -Text "the release MSI could not be reinstalled: $($_.Exception.Message)" `
            -Condition $false
        [void](Complete-Step -Step $step)
    }

    $vcredistAfter = Get-ChocolateyPackageVersion -Id 'vcredist140'
    Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
    $failedSteps = @($script:Steps | Where-Object { -not $_.ok })
    $result = [ordered]@{
        ok              = ($failedSteps.Count -eq 0)
        msiPath         = $MsiPath
        msiSha256       = $MsiSha256
        restoreRan      = $restoreRan
        restoreExitCode = $restoreExitCode
        vcredistBefore  = $vcredistBefore
        vcredistAfter   = $vcredistAfter
        observations    = @($script:Observations)
        completedUtc    = [DateTime]::UtcNow.ToString('o')
        steps           = @($script:Steps)
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $ResultPath) -Force | Out-Null
    Set-Content -LiteralPath $ResultPath -Value ($result | ConvertTo-Json -Depth 10) -Encoding utf8NoBOM
}

# The exit code says the worker RAN, not that the rehearsal passed: the verdict is
# the runner's to make from the JSON, and an exit code that also carried the
# verdict would let a worker that never wrote a result look like a failed step.
exit 0
