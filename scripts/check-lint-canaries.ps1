#Requires -Version 7.0
<#
.SYNOPSIS
    Proves every blocking clang-tidy check still fires, against a file written to
    be rejected by it.

.DESCRIPTION
    The rule for adding a blocking check is that a full pass reports zero findings
    in repository-owned files. That rule is only half a rule: a check that does not
    run also reports zero findings, and the two are indistinguishable from the
    report.

    They are not hypothetical. On this tree, `bugprone-unchecked-optional-access`
    reported nothing at all in the whole-tree advisory pass, while a single-file
    run with the same check configuration and the same compile database reported
    four findings in a file the pass had covered and reported other checks from.
    A promotion decision resting on that pass would have been resting on silence.

    So each blocking check owns a canary under
    scripts/tests/fixtures/lint-canaries: a few lines written to violate exactly
    that check and nothing else. A check that does not reject its own canary is
    not working here, however clean the tree looks, and this fails.

    This is the positive half of the contract. The zero-findings pass says the
    tree is clean; this says the instrument is not.

.PARAMETER ClangTidy
    Explicit path to clang-tidy.exe. Autodetected from PATH when omitted.

.PARAMETER RepoRoot
    Repository to read the canaries from. Defaults to the repository this script
    lives in.

.PARAMETER Only
    Check only the named checks.

.EXAMPLE
    .\scripts\check-lint-canaries.ps1
#>

param(
    [string] $ClangTidy,
    [string] $RepoRoot,
    [string[]] $Only = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$canaryDirectory = Join-Path $RepoRoot 'scripts/tests/fixtures/lint-canaries'

# The check each canary belongs to, and the file that has to be rejected by it.
# A canary whose file is missing is a failure, not a skip: the pairing is the
# whole point, and a missing half of it is exactly the silence this guards.
$script:Canaries = [ordered]@{
    'bugprone-use-after-move'             = 'bugprone-use-after-move.cpp'
    'bugprone-dangling-handle'            = 'bugprone-dangling-handle.cpp'
    'readability-misleading-indentation'  = 'readability-misleading-indentation.cpp'
    'clang-analyzer-core.CallAndMessage'  = 'clang-analyzer-core.CallAndMessage.cpp'
    'clang-analyzer-core.uninitialized.*' = 'clang-analyzer-core.uninitialized.cpp'
    'clang-analyzer-cplusplus.NewDelete*' = 'clang-analyzer-cplusplus.NewDelete.cpp'
}

if (-not $ClangTidy) {
    $found = Get-Command 'clang-tidy' -ErrorAction SilentlyContinue
    if ($found) { $ClangTidy = $found.Source }
}
if (-not $ClangTidy -or -not (Test-Path -LiteralPath $ClangTidy -PathType Leaf)) {
    # Exit 3, the same code check-quality.ps1 uses: a missing tool is not a pass
    # and is not a failure of the code either.
    Write-Host 'check-lint-canaries: clang-tidy was not found.'
    exit 3
}

$failures = [System.Collections.Generic.List[string]]::new()
$checked = 0

foreach ($check in $script:Canaries.Keys) {
    if ($Only.Count -gt 0 -and $Only -notcontains $check) { continue }

    $path = Join-Path $canaryDirectory $script:Canaries[$check]
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        [void]$failures.Add("$check : its canary $($script:Canaries[$check]) is missing")
        continue
    }

    $checked++
    # --checks="-*,<check>" rather than the repository configuration: this asks
    # whether THIS check fires, and a canary that tripped some other check would
    # answer a different question. -- separates the compile arguments, which the
    # canaries need because they are not in any compile database.
    $arguments = @(
        "--checks=-*,$check",
        '--quiet',
        $path,
        '--',
        '-std=c++20',
        '-x', 'c++'
    )

    $output = & $ClangTidy @arguments 2>&1 | Out-String
    if ($output -notmatch [regex]::Escape($check.TrimEnd('*'))) {
        [void]$failures.Add(
            "$check : did not fire on its own canary. The check is not working here, " +
            'so a zero-findings pass proves nothing about it.')
        continue
    }

    Write-Host "  fires  $check"
}

Write-Host ''
Write-Host "lint canaries: $checked check(s) exercised"

if ($failures.Count -gt 0) {
    Write-Host ''
    foreach ($failure in $failures) { Write-Host "FAIL  $failure" }
    Write-Host ''
    Write-Host 'docs/dev/static-analysis.md explains why a silent check and a clean tree look the same.'
    exit 1
}

exit 0
