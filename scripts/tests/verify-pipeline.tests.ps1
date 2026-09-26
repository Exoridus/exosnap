#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the command-line batching and tool cache helpers that
    scripts/check-quality.ps1 imports.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use, so CTest
    runs all of them the same way and a contributor reads one style. The
    orchestration contracts (scope, order, dependency truthfulness, QML
    diagnostics) are Rust tests in tools/exo-dev.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot
Import-Module (Join-Path $scriptRoot 'lib/VerifyPipeline.psm1') -Force -DisableNameChecking

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
function Assert-Equal {
    param($Expected, $Actual, [string] $Message)
    if ("$Expected" -ne "$Actual") { throw "$Message (expected '$Expected', got '$Actual')" }
}

Write-Host ''
Write-Host 'Command lines that fit, caches that are derived'

Test-Case 'no batch can exceed the Windows command-line limit' {
    # 900 paths on one command line is roughly 36 KB against a 32767-character
    # ceiling: the process never starts, and a step that tolerates failure then
    # reports a clean pass over an analysis that did not happen.
    $files = 1..900 | ForEach-Object { "libs/engine/src/some_reasonably_long_translation_unit_name_$_.cpp" }
    $fixed = @('-p', 'C:/Users/someone/Development/exosnap/build/windows-x64-ninja-debug', '--checks=-clang-analyzer-*')
    $batches = Split-VerifyCommandLineBatch -Item $files -FixedArgument $fixed
    Assert-True ($batches.Count -gt 1) '900 files must not end up on one command line'
    foreach ($batch in $batches) {
        $length = (@($fixed) + @($batch) | ForEach-Object { $_.Length + 3 } | Measure-Object -Sum).Sum
        Assert-True ($length -lt 32767) "a batch of $($batch.Count) file(s) is $length characters, past the limit"
    }
    $total = @($batches | ForEach-Object { $_ }).Count
    Assert-Equal $files.Count $total 'batching must not drop a file'
}

Test-Case 'a single argument longer than the budget still yields one batch' {
    $batches = Split-VerifyCommandLineBatch -Item @('x' * 200) -CommandLineBudget 50
    Assert-Equal 1 $batches.Count 'an over-budget item is still analysed, never silently dropped'
}

Test-Case 'an empty list produces no invocation at all' {
    Assert-Equal 0 (Split-VerifyCommandLineBatch -Item @()).Count 'nothing to analyse means no command line'
}

Test-Case 'the tool cache is derived, and lives outside the repository and the build trees' {
    $directory = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @('14.44', 'x64')
    $normalised = $directory.Replace('\', '/')
    Assert-True ($normalised -notlike "$($repoRoot.Replace('\', '/'))*") 'the cache must not live inside the repository'
    Assert-True ($normalised -notmatch '(^|/)build(/|$)') 'the cache must not live inside a build tree'
    Assert-True ($normalised -like '*/clang-tidy/*') 'the cache must be namespaced by tool'
}

Test-Case 'a different toolchain gets a different cache directory' {
    $root = Join-Path ([IO.Path]::GetTempPath()) 'verify-tests-cache'
    $one = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @('14.44') -Root $root
    $same = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @('14.44') -Root $root
    $other = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @('14.45') -Root $root
    Assert-Equal $one $same 'the same toolchain must address the same cache'
    Assert-True ($one -ne $other) 'a compiler change must not be served results from the previous one'
}

Test-Case 'an empty fingerprint is recorded as unqualified rather than shared' {
    $root = Join-Path ([IO.Path]::GetTempPath()) 'verify-tests-cache'
    $blank = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @() -Root $root
    $named = Get-VerifyToolCacheDirectory -Tool 'clang-tidy' -Fingerprint @('14.44') -Root $root
    Assert-True ($blank -ne $named) 'an unknown toolchain must not read a known one'
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
