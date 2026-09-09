#Requires -Version 7.0
<#
.SYNOPSIS
    The version-drift gate: every packaging literal must name the version the source
    tree declares.

.DESCRIPTION
    `project(exosnap VERSION x.y.z)` in the root CMakeLists.txt is the only place the
    product version is declared, and roughly sixteen literals repeat it by hand --
    across the Chocolatey nuspec and install script, the three WinGet manifests and
    the directory they live in, and the Scoop manifest. A bump that edited some of
    them and not the others produced a package pointing at a release that does not
    exist, and nothing failed until a submission was rejected or a user's install
    404ed.

    One entry point, three validators, one verdict. It runs on every pull request
    rather than only in a hook, because a hook is per-machine and this is the kind of
    mistake that is made on the machine where the hook was skipped.

    Version only. Installer hashes and the Chocolatey moderation bar are checked by
    the same validators in their full form, at submission time
    (docs/release-checklist.md section 8) -- neither can be true between a bump and
    the release that produces the bytes they describe, so requiring them here would
    make the gate red for the whole of every release cycle.

.PARAMETER Version
    Target version. Defaults to the canonical CMake project version, which is the
    point of the check; pass it only to test a bump before making it.
#>
param(
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if (-not $Version) {
    $cmakeText = Get-Content -LiteralPath (Join-Path $repoRoot 'CMakeLists.txt') -Raw
    if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not parse project(exosnap VERSION x.y.z) from root CMakeLists.txt.'
    }
    $Version = $Matches[1]
}

Write-Host "Packaging version gate: every packaging literal must name $Version." -ForegroundColor Cyan

$validators = @(
    @{ Name = 'Chocolatey'; Script = 'validate-chocolatey-package.ps1'; Arguments = @('-VersionOnly') },
    @{ Name = 'WinGet'; Script = 'validate-winget-manifest.ps1'; Arguments = @() },
    @{ Name = 'Scoop'; Script = 'validate-scoop-manifest.ps1'; Arguments = @() }
)

$failed = @()
foreach ($validator in $validators) {
    Write-Host ''
    Write-Host "-- $($validator.Name)"
    $path = Join-Path $PSScriptRoot $validator.Script
    # A child process per validator, so one that throws cannot take the others with
    # it: a bump usually breaks more than one surface, and reporting only the first
    # costs a whole round trip per literal.
    & pwsh -NoProfile -NonInteractive -File $path -Version $Version @($validator.Arguments)
    if ($LASTEXITCODE -ne 0) { $failed += $validator.Name }
}

Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host ("Packaging version gate FAILED: $($failed -join ', ') do not agree with the " +
        "CMake version $Version. Bump every literal (docs/release-checklist.md section 8) or fix " +
        'the source version.') -ForegroundColor Red
    exit 1
}
Write-Host "Packaging version gate PASSED: Chocolatey, WinGet and Scoop all name $Version." -ForegroundColor Green
exit 0
