#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the guard that proves each blocking clang-tidy check still fires.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    This guard exists because a silent check and a clean tree produce the same
    report, so the thing that matters most about it is that it FAILS when a check
    stops firing. Each case below removes one half of a check-canary pair and
    requires the guard to say so.

    The canaries are also read here as what they are -- files written to be
    rejected -- which is why scripts/tests/ is excluded from the source-hygiene
    and drift scanners: a directory that contains every shape a rule rejects
    reports its own evidence as a violation.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$guard = Join-Path $scriptRoot 'check-lint-canaries.ps1'
$canaryDirectory = Join-Path $PSScriptRoot 'fixtures/lint-canaries'

$script:Passed = 0
$script:Failed = 0
$script:Skipped = 0

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

function Invoke-Guard {
    param([string] $Root, [string[]] $ExtraArgs = @())
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $guard) + $ExtraArgs
    if ($Root) { $arguments += @('-RepoRoot', $Root) }
    $output = & pwsh @arguments 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($output | Out-String) }
}

function New-CanaryCopy {
    <#
    .SYNOPSIS
        A repository-shaped temp tree holding copies of the real canaries.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) "lint-canaries-tests/$([guid]::NewGuid().ToString('n'))"
    $target = Join-Path $root 'scripts/tests/fixtures/lint-canaries'
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -Path (Join-Path $canaryDirectory '*.cpp') -Destination $target -Force
    return $root
}

$clangTidy = Get-Command 'clang-tidy' -ErrorAction SilentlyContinue
if (-not $clangTidy) {
    # Reported, not silently passed: a suite that skips itself when its tool is
    # absent is the same silence this guard is about.
    Write-Host ''
    Write-Host 'clang-tidy is not installed; every case here needs it.' -ForegroundColor Yellow
    Write-Host '0/0 passed (1 suite skipped)'
    exit 0
}

Write-Host ''
Write-Host 'The guard accepts a working toolchain'

Test-Case 'every blocking check fires on its own canary' {
    $result = Invoke-Guard -Root $null
    Assert-True ($result.ExitCode -eq 0) "the real canaries were rejected:`n$($result.Output)"
    Assert-True ($result.Output -match 'bugprone-use-after-move') "a check went unexercised:`n$($result.Output)"
    Assert-True ($result.Output -match '6 check\(s\) exercised') "the count was wrong:`n$($result.Output)"
}

Write-Host ''
Write-Host 'The guard rejects a broken one'

Test-Case 'a canary that lost its violation is reported' {
    # The case the guard exists for, in the only direction it can be simulated
    # from outside: the check is fine, the evidence that it works is gone. A
    # check that silently stopped running produces the same output.
    $root = New-CanaryCopy
    try {
        $path = Join-Path $root 'scripts/tests/fixtures/lint-canaries/bugprone-use-after-move.cpp'
        Set-Content -LiteralPath $path -Encoding utf8 -Value @'
#include <string>
#include <utility>

namespace exosnap::lint_canary {

std::string::size_type NoLongerViolates() {
    std::string moved_from = "canary";
    const std::string moved_to = std::move(moved_from);
    return moved_to.size();
}

} // namespace exosnap::lint_canary
'@
        $result = Invoke-Guard -Root $root
        Assert-True ($result.ExitCode -ne 0) "a defanged canary passed:`n$($result.Output)"
        Assert-True ($result.Output -match 'did not fire on its own canary') `
            "the reason was not stated:`n$($result.Output)"
        Assert-True ($result.Output -match 'proves nothing about it') `
            "the consequence was not stated:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a missing canary is a failure, not a skip' {
    $root = New-CanaryCopy
    try {
        Remove-Item -LiteralPath (Join-Path $root 'scripts/tests/fixtures/lint-canaries/bugprone-dangling-handle.cpp') -Force
        $result = Invoke-Guard -Root $root
        Assert-True ($result.ExitCode -ne 0) "a missing canary was skipped:`n$($result.Output)"
        Assert-True ($result.Output -match 'is missing') "the reason was not stated:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a named check can be exercised on its own' {
    $result = Invoke-Guard -Root $null -ExtraArgs @('-Only', 'bugprone-dangling-handle')
    Assert-True ($result.ExitCode -eq 0) "the filtered run failed:`n$($result.Output)"
    Assert-True ($result.Output -match '1 check\(s\) exercised') "the filter did not narrow:`n$($result.Output)"
}

Test-Case 'a missing clang-tidy is reported as a missing tool, not a pass' {
    # Exit 3 is the repository's "the tool was not there" code. Reporting 0 would
    # make an unconfigured machine look like a verified one.
    $result = Invoke-Guard -Root $null -ExtraArgs @('-ClangTidy', (Join-Path $canaryDirectory 'not-a-tool.exe'))
    Assert-True ($result.ExitCode -eq 3) "expected exit 3, got $($result.ExitCode):`n$($result.Output)"
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
