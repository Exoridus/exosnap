#Requires -Version 7.0
<#
.SYNOPSIS
    Guards the completeness of app/cli/CommandLineFlags.cpp.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    exosnap.exe rejects long options it does not know, which is only safe while
    the registry actually lists every option the five argv parsers understand. A
    missing entry does not fail quietly -- it turns a working harness or release
    invocation into an exit 2 -- so this scans the parser sources for long-option
    string literals and asserts each one is registered.

    The scan is deliberately source-based rather than a hand-maintained list:
    a list would have exactly the drift problem it is meant to prevent.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$registry = Join-Path $repoRoot 'app/cli/CommandLineFlags.cpp'

# The sources that read argv for exosnap.exe. The updater is a separate binary
# with its own options and is deliberately not scanned.
$parserSources = @(
    'app/auto_record/AutoRecordOptions.cpp'
    'app/quick/ExoSnap/Quick/main.cpp'
    'app/quick/ExoSnap/Quick/QuickAutoEditHarness.cpp'
    'app/services/ElevatedRelaunch.h'
    'app/services/UpdateFeedOverride.h'
    'app/services/VerifyReinstallMode.h'
)

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

function Get-RegisteredFlag {
    $text = Get-Content -LiteralPath $registry -Raw
    return [regex]::Matches($text, 'KnownFlag\{"(--[a-z0-9-]+)"') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
}

function Get-SourceFlag {
    param([string] $Relative)
    $path = Join-Path $repoRoot $Relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "parser source '$Relative' does not exist; update the list in this test"
    }
    $text = Get-Content -LiteralPath $path -Raw
    return [regex]::Matches($text, '"(--[a-z0-9-]+)"') |
        ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
}

Write-Host ''
Write-Host 'CLI flag registry'

Test-Case 'the registry file exists and is not empty' {
    Assert-True (Test-Path -LiteralPath $registry -PathType Leaf) "missing $registry"
    Assert-True ((Get-RegisteredFlag).Count -gt 50) 'the registry parsed to fewer than 50 flags'
}

Test-Case 'every long option in a parser source is registered' {
    $registered = Get-RegisteredFlag
    $missing = @()
    foreach ($relative in $parserSources) {
        foreach ($flag in (Get-SourceFlag $relative)) {
            if ($registered -notcontains $flag) { $missing += "$flag ($relative)" }
        }
    }
    $missing = @($missing)
    Assert-True ($missing.Count -eq 0) `
    ("unregistered long option(s): " + ($missing -join ', ') +
        " -- add them to app/cli/CommandLineFlags.cpp or exosnap.exe will refuse them")
}

Test-Case 'the registry lists no flag twice' {
    $text = Get-Content -LiteralPath $registry -Raw
    $all = [regex]::Matches($text, 'KnownFlag\{"(--[a-z0-9-]+)"') | ForEach-Object { $_.Groups[1].Value }
    $duplicates = @($all | Group-Object | Where-Object Count -gt 1 | ForEach-Object { $_.Name })
    Assert-True ($duplicates.Count -eq 0) ("duplicate registry entries: " + ($duplicates -join ', '))
}

# The guard has to be able to fail, or it proves nothing. Feed it a synthetic
# source carrying an option the registry does not have.
Test-Case 'an unregistered option is actually detected' {
    $temp = Join-Path ([IO.Path]::GetTempPath()) "cli-flags-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try {
        $fixture = Join-Path $temp 'fake_parser.cpp'
        Set-Content -LiteralPath $fixture -Value 'if (arg == QStringLiteral("--definitely-not-registered")) {}'
        $registered = Get-RegisteredFlag
        $found = [regex]::Matches((Get-Content -LiteralPath $fixture -Raw), '"(--[a-z0-9-]+)"') |
            ForEach-Object { $_.Groups[1].Value }
        Assert-True ($found -contains '--definitely-not-registered') 'the scanner did not see the fixture flag'
        Assert-True ($registered -notcontains '--definitely-not-registered') `
            'the fixture flag is somehow registered'
    }
    finally { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
