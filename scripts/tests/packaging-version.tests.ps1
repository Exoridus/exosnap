#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the packaging version-drift gate: scripts/check-packaging-version.ps1
    and the three validators behind it.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case is the real packaging tree, copied into a temporary repository root,
    with exactly one version literal edited. The gate is worth having only if each of
    those literals on its own turns it red -- a partial bump is precisely the mistake
    it exists to catch, and a check that only notices when everything moved together
    would notice nothing.

    Nothing here touches the network, a package feed, or the repository's own
    packaging files.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot

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

$cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not parse the project version from the root CMakeLists.txt.'
}
$script:Version = $Matches[1]

function New-FixtureRoot {
    <#
    .SYNOPSIS
        A repository root the gate accepts, as the base for each case.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) "packaging-version-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path (Join-Path $root 'scripts') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'packaging') -Force | Out-Null
    foreach ($name in @('check-packaging-version.ps1', 'validate-chocolatey-package.ps1',
            'validate-winget-manifest.ps1', 'validate-scoop-manifest.ps1')) {
        Copy-Item -LiteralPath (Join-Path $scriptRoot $name) -Destination (Join-Path $root 'scripts') -Force
    }
    foreach ($name in @('chocolatey', 'winget', 'scoop')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "packaging/$name") `
            -Destination (Join-Path $root "packaging/$name") -Recurse -Force
    }
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') `
        -Value "project(exosnap VERSION $script:Version LANGUAGES C CXX)`n" -NoNewline
    return $root
}

function Edit-FixtureFile {
    param([string] $Root, [string] $Relative, [string] $Pattern, [string] $Replacement)
    $path = Join-Path $Root $Relative
    $text = Get-Content -LiteralPath $path -Raw
    $updated = [Regex]::Replace($text, $Pattern, $Replacement)
    Assert-True ($updated -ne $text) "fixture mutation '$Pattern' matched nothing in $Relative"
    Set-Content -LiteralPath $path -Value $updated -NoNewline
}

function Invoke-Gate {
    param([string] $Root)
    $output = & pwsh -NoProfile -NonInteractive -File (Join-Path $Root 'scripts/check-packaging-version.ps1') 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Test-Drift {
    <#
    .SYNOPSIS
        One edited literal must turn the gate red and say which file it was in.
    #>
    param(
        [string] $Name,
        [string] $Relative,
        [string] $Pattern,
        [string] $Replacement,
        [string] $ExpectedText
    )
    Test-Case $Name {
        $root = New-FixtureRoot
        Edit-FixtureFile -Root $root -Relative $Relative -Pattern $Pattern -Replacement $Replacement
        $result = Invoke-Gate -Root $root
        Assert-True ($result.ExitCode -ne 0) "drift in $Relative must fail the gate: $($result.Output)"
        Assert-True ($result.Output -match [Regex]::Escape($ExpectedText)) `
            "the failure must name what drifted ('$ExpectedText'): $($result.Output)"
    }
}

Write-Host 'check-packaging-version.ps1'

$script:Other = if ($script:Version -eq '9.9.9') { '9.9.8' } else { '9.9.9' }

Test-Case 'the packaging tree in this repository agrees with its CMake version' {
    $result = Invoke-Gate -Root (New-FixtureRoot)
    Assert-True ($result.ExitCode -eq 0) "the tracked packaging tree must pass: $($result.Output)"
}

Test-Drift -Name 'a Chocolatey nuspec version left behind fails the gate' `
    -Relative 'packaging/chocolatey/exosnap.nuspec' `
    -Pattern "(?m)^(\s*<version>)$([Regex]::Escape($script:Version))(</version>)" `
    -Replacement "`${1}$script:Other`${2}" -ExpectedText 'Chocolatey'

Test-Drift -Name 'a Chocolatey icon URL left behind fails the gate' `
    -Relative 'packaging/chocolatey/exosnap.nuspec' `
    -Pattern "(?m)(<iconUrl>[^<]*@v)$([Regex]::Escape($script:Version))" `
    -Replacement "`${1}$script:Other" -ExpectedText 'iconUrl'

Test-Drift -Name 'a Chocolatey download URL left behind fails the gate' `
    -Relative 'packaging/chocolatey/tools/chocolateyinstall.ps1' `
    -Pattern "(?m)(url64bit\s*=\s*'[^']*)$([Regex]::Escape($script:Version))" `
    -Replacement "`${1}$script:Other" -ExpectedText 'url64bit'

Test-Drift -Name 'a WinGet PackageVersion left behind fails the gate' `
    -Relative "packaging/winget/manifests/c/Codexo/ExoSnap/$script:Version/Codexo.ExoSnap.yaml" `
    -Pattern "(?m)^(PackageVersion:\s*)$([Regex]::Escape($script:Version))\s*$" `
    -Replacement "`${1}$script:Other" -ExpectedText 'PackageVersion'

Test-Drift -Name 'a WinGet installer URL left behind fails the gate' `
    -Relative "packaging/winget/manifests/c/Codexo/ExoSnap/$script:Version/Codexo.ExoSnap.installer.yaml" `
    -Pattern "(?m)(InstallerUrl:\s*\S*?)$([Regex]::Escape($script:Version))" `
    -Replacement "`${1}$script:Other" -ExpectedText 'InstallerUrl'

Test-Drift -Name 'a WinGet DisplayVersion left behind fails the gate' `
    -Relative "packaging/winget/manifests/c/Codexo/ExoSnap/$script:Version/Codexo.ExoSnap.installer.yaml" `
    -Pattern "(?m)^(\s*DisplayVersion:\s*)$([Regex]::Escape($script:Version))\s*$" `
    -Replacement "`${1}$script:Other" -ExpectedText 'DisplayVersion'

Test-Drift -Name 'a WinGet release-notes URL left behind fails the gate' `
    -Relative "packaging/winget/manifests/c/Codexo/ExoSnap/$script:Version/Codexo.ExoSnap.locale.en-US.yaml" `
    -Pattern "(?m)(ReleaseNotesUrl:\s*\S*?)$([Regex]::Escape($script:Version))" `
    -Replacement "`${1}$script:Other" -ExpectedText 'ReleaseNotesUrl'

Test-Drift -Name 'a Scoop version left behind fails the gate' `
    -Relative 'packaging/scoop/exosnap.json' `
    -Pattern "(?m)^(\s*`"version`":\s*`")$([Regex]::Escape($script:Version))(`")" `
    -Replacement "`${1}$script:Other`${2}" -ExpectedText 'Scoop'

Test-Drift -Name 'a Scoop extract_dir left behind fails the gate' `
    -Relative 'packaging/scoop/exosnap.json' `
    -Pattern "(?m)(`"extract_dir`":\s*`"ExoSnap-)$([Regex]::Escape($script:Version))" `
    -Replacement "`${1}$script:Other" -ExpectedText 'extract_dir'

Test-Case 'a second WinGet version directory is refused rather than guessed at' {
    # WinGet keys a submission on the directory. Two of them means the bump copied
    # instead of moving, and the validator used to pick whichever was there alone.
    $root = New-FixtureRoot
    $manifests = Join-Path $root 'packaging/winget/manifests/c/Codexo/ExoSnap'
    Copy-Item -LiteralPath (Join-Path $manifests $script:Version) `
        -Destination (Join-Path $manifests $script:Other) -Recurse -Force
    $result = Invoke-Gate -Root $root
    Assert-True ($result.ExitCode -ne 0) "two version directories must fail: $($result.Output)"
    Assert-True ($result.Output -match 'exactly one version directory') `
        "the failure must say what is wrong: $($result.Output)"
}

Test-Case 'a Scoop autoupdate block frozen on one version fails the gate' {
    # An autoupdate template with a literal version stops the published bucket
    # refreshing itself, and nothing else in the repository would ever notice.
    $root = New-FixtureRoot
    Edit-FixtureFile -Root $root -Relative 'packaging/scoop/exosnap.json' `
        -Pattern '\$version' -Replacement $script:Version
    $result = Invoke-Gate -Root $root
    Assert-True ($result.ExitCode -ne 0) "a frozen autoupdate template must fail: $($result.Output)"
    Assert-True ($result.Output -match 'autoupdate') "the failure must name autoupdate: $($result.Output)"
}

Test-Case 'the Chocolatey checksum placeholder does not fail the version gate' {
    # Between a bump and the published release there is no MSI to hash. If the gate
    # tripped on the placeholder it would be red for the whole of every release
    # cycle, and it would be turned off.
    $root = New-FixtureRoot
    Edit-FixtureFile -Root $root -Relative 'packaging/chocolatey/tools/chocolateyinstall.ps1' `
        -Pattern "(?m)(^\s*checksum64\s*=\s*')[0-9a-f]{64}(')" -Replacement "`${1}$('0' * 64)`${2}"
    $result = Invoke-Gate -Root $root
    Assert-True ($result.ExitCode -eq 0) "the placeholder must not fail the version gate: $($result.Output)"
}

Write-Host ''
Write-Host "  $($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
