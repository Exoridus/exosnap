#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the Chocolatey package validator.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case copies scripts/validate-chocolatey-package.ps1 and the real
    packaging/chocolatey/ tree into a throwaway repository root under the temp
    directory, mutates one thing, and runs the validator against that copy.
    Nothing in this repository is read for anything but the copy, and nothing
    is written to it.

    Every rule is asserted in both directions. A guard that has only ever been
    seen green proves nothing, so each case that must fail is paired with the
    unmutated baseline that must pass. The baseline substitutes a well-formed
    hash for the checksum the tree carries between a version bump and the
    published release, which is exactly the value the degenerate-checksum case
    puts back.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot
$validator = Join-Path $scriptRoot 'validate-chocolatey-package.ps1'
$realChocoRoot = Join-Path $repoRoot 'packaging/chocolatey'

# A well-formed hash that is not a run of one character, standing in for the
# hash of a published MSI.
$script:PlausibleSha256 = '0123456789abcdef' * 4

$cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not parse the project version from the root CMakeLists.txt.'
}
$script:Version = $Matches[1]

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

function New-FixtureRoot {
    <#
    .SYNOPSIS
        A repository root the validator accepts, as the base for each case.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) "validate-chocolatey-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path (Join-Path $root 'scripts') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $root 'packaging') -Force | Out-Null

    Copy-Item -LiteralPath $validator -Destination (Join-Path $root 'scripts') -Force
    Copy-Item -LiteralPath $realChocoRoot -Destination (Join-Path $root 'packaging/chocolatey') -Recurse -Force
    Set-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') `
        -Value "project(exosnap VERSION $script:Version LANGUAGES C CXX)`n" -NoNewline

    # The tree deliberately carries a placeholder until the release is published;
    # every case except the degenerate-checksum one starts from a real-shaped hash.
    Edit-FixtureFile -Root $root -Relative 'tools/chocolateyinstall.ps1' `
        -Pattern "(?m)(^\s*checksum64\s*=\s*')[0-9a-f]{64}(')" `
        -Replacement "`${1}$script:PlausibleSha256`${2}"

    return $root
}

function Get-FixturePath {
    param([string] $Root, [string] $Relative)
    return (Join-Path $Root "packaging/chocolatey/$Relative")
}

function Edit-FixtureFile {
    param([string] $Root, [string] $Relative, [string] $Pattern, [string] $Replacement)
    $path = Get-FixturePath -Root $Root -Relative $Relative
    $text = Get-Content -LiteralPath $path -Raw
    $updated = [Regex]::Replace($text, $Pattern, $Replacement)
    Assert-True ($updated -ne $text) "fixture mutation '$Pattern' matched nothing in $Relative"
    Set-Content -LiteralPath $path -Value $updated -NoNewline
}

function Invoke-Validator {
    param([string] $Root, [string[]] $ExtraArgs = @())
    $pwshArgs = @('-NoProfile', '-NonInteractive', '-File', (Join-Path $Root 'scripts/validate-chocolatey-package.ps1')) + $ExtraArgs
    $output = & pwsh @pwshArgs 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Test-Rejects {
    <#
    .SYNOPSIS
        Mutate the baseline, expect a non-zero exit and a message that names the
        problem rather than some unrelated rule tripping first.
    #>
    param([string] $Name, [scriptblock] $Mutate, [string] $ExpectedText, [string[]] $ExtraArgs = @())
    Test-Case $Name {
        $root = New-FixtureRoot
        try {
            & $Mutate $root
            $result = Invoke-Validator -Root $root -ExtraArgs (@('-Version', $script:Version) + $ExtraArgs)
            Assert-True ($result.ExitCode -ne 0) "expected a failure exit code, got 0. Output:`n$($result.Output)"
            Assert-True ($result.Output -like "*$ExpectedText*") `
                "expected the report to mention '$ExpectedText'. Output:`n$($result.Output)"
        }
        finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
    }
}

Write-Host 'validate-chocolatey-package.ps1'

Test-Case 'accepts the shipped package once the checksum is a published hash' {
    $root = New-FixtureRoot
    try {
        $result = Invoke-Validator -Root $root -ExtraArgs @('-Version', $script:Version)
        Assert-True ($result.ExitCode -eq 0) "expected exit 0. Output:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'defaults the target version to the CMake project version' {
    $root = New-FixtureRoot
    try {
        $result = Invoke-Validator -Root $root
        Assert-True ($result.ExitCode -eq 0) "expected exit 0. Output:`n$($result.Output)"
        Assert-True ($result.Output -like "*$script:Version*") 'expected the resolved version in the report'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Rejects 'rejects the all-zero placeholder checksum' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'tools/chocolateyinstall.ps1' `
        -Pattern "(?m)(^\s*checksum64\s*=\s*')[0-9a-f]{64}(')" -Replacement "`${1}$('0' * 64)`${2}"
} 'placeholder for an unpublished release'

Test-Case 'rejects the placeholder even when a manifest agrees with it' {
    # The manifest cross-check is what -RequireManifest governs. A manifest that
    # happens to carry the same placeholder must not buy the package a pass.
    $root = New-FixtureRoot
    try {
        Edit-FixtureFile -Root $root -Relative 'tools/chocolateyinstall.ps1' `
            -Pattern "(?m)(^\s*checksum64\s*=\s*')[0-9a-f]{64}(')" -Replacement "`${1}$('0' * 64)`${2}"
        $manifestPath = Join-Path $root 'artifact-manifest.json'
        Set-Content -LiteralPath $manifestPath -Value (@{
                version   = $script:Version
                msiSha256 = '0' * 64
            } | ConvertTo-Json)

        $result = Invoke-Validator -Root $root -ExtraArgs @(
            '-Version', $script:Version, '-ManifestPath', $manifestPath, '-RequireManifest')
        Assert-True ($result.ExitCode -ne 0) "expected a failure exit code, got 0. Output:`n$($result.Output)"
        Assert-True ($result.Output -like '*placeholder for an unpublished release*') `
            "expected the placeholder message. Output:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Rejects 'rejects a checksum that is not lowercase hex' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'tools/chocolateyinstall.ps1' `
        -Pattern "(?m)(^\s*checksum64\s*=\s*')[0-9a-f]{64}(')" -Replacement "`${1}NOTAHASH`${2}"
} 'not 64 lowercase hex characters'

Test-Rejects 'rejects a missing uninstall script' {
    param($root)
    Remove-Item -LiteralPath (Get-FixturePath -Root $root -Relative 'tools/chocolateyuninstall.ps1') -Force
} 'Missing tools/chocolateyuninstall.ps1'

Test-Rejects 'rejects Write-Host in an automation script' {
    param($root)
    Add-Content -LiteralPath (Get-FixturePath -Root $root -Relative 'tools/chocolateyinstall.ps1') `
        -Value "`nWrite-Host 'installed'"
} 'uses Write-Host'

Test-Rejects 'rejects an automation script without a Stop preference' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'tools/chocolateyuninstall.ps1' `
        -Pattern "(?m)^\s*\`$ErrorActionPreference\s*=\s*'Stop'\s*\r?\n" -Replacement ''
} "must start with `$ErrorActionPreference = 'Stop'"

Test-Rejects 'rejects a private Chocolatey environment variable' {
    param($root)
    Add-Content -LiteralPath (Get-FixturePath -Root $root -Relative 'tools/chocolateyinstall.ps1') `
        -Value "`n`$target = `$env:chocolateyPackageFolder"
} 'private Chocolatey environment variable'

Test-Rejects 'rejects comma-separated tags' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '<tags>[^<]+</tags>' -Replacement '<tags>exosnap,recorder</tags>'
} 'space-separated'

Test-Rejects 'rejects a chocolatey tag' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '<tags>([^<]+)</tags>' -Replacement '<tags>${1} chocolatey</tags>'
} "must not contain 'chocolatey'"

Test-Rejects 'rejects a raw GitHub icon URL' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '<iconUrl>[^<]+</iconUrl>' `
        -Replacement '<iconUrl>https://raw.githubusercontent.com/Exoridus/exosnap/main/icon.png</iconUrl>'
} 'must go through a CDN'

Test-Rejects 'rejects a plain-http metadata URL' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '<bugTrackerUrl>https://' -Replacement '<bugTrackerUrl>http://'
} 'is not an https:// URL'

Test-Rejects 'rejects a missing nuspec enhancement field' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' -Pattern '<docsUrl>[^<]+</docsUrl>' -Replacement ''
} '<docsUrl> is missing or empty'

Test-Rejects 'rejects a Markdown heading with no space after the hash' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' -Pattern '## Features' -Replacement '##Features'
} 'no space after'

Test-Rejects 'rejects a dependency without a version range' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '<dependency id="vcredist140" version="[^"]+" />' -Replacement '<dependency id="vcredist140" />'
} 'has no version range'

Test-Rejects 'rejects a non-script file in tools/' {
    param($root)
    Set-Content -LiteralPath (Get-FixturePath -Root $root -Relative 'tools/exosnap.exe') -Value 'not really a binary'
} 'is not a PowerShell script'

Test-Rejects 'rejects a stale version reference left in the description' {
    param($root)
    Edit-FixtureFile -Root $root -Relative 'exosnap.nuspec' `
        -Pattern '## Notes' -Replacement '## Notes (carried over from 0.8.1)'
} 'stale version reference'

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
