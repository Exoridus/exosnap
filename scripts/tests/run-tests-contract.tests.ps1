#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the canonical test entry point, scripts/run-tests.ps1.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case points run-tests.ps1 at a throwaway build tree consisting of a
    hand-written CTestTestfile.cmake, so the real ctest answers the real
    question and nothing has to be stubbed. Two tests, two labels: "live" for
    the hardware-querying category CI excludes, and "live_verify" for a script
    suite that queries nothing and must survive that exclusion.

    The exclusion is tested from both sides, and the unanchored form is asserted
    to still be wrong. A guard whose own premise stopped being true would
    otherwise keep passing after the behaviour it protects against went away --
    or, worse, after the fix was reverted and the pattern became a plain label
    again.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$runTests = Join-Path $scriptRoot 'run-tests.ps1'

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

function New-FixtureTree {
    <#
    .SYNOPSIS
        A build tree ctest can run without anything having been compiled.
    .PARAMETER Empty
        Register no tests at all, for the zero-selection cases.
    #>
    param([switch] $Empty)

    $root = Join-Path ([IO.Path]::GetTempPath()) "run-tests-contract/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    # An absolute command: ctest resolves a test command against the environment
    # it inherits, and a bare interpreter name is not reliably on it.
    $noop = (Get-Command cmake).Source -replace '\\', '/'

    $body = if ($Empty) { '' } else { @"
add_test(fixture.hardware "$noop" "-E" "true")
set_tests_properties(fixture.hardware PROPERTIES LABELS "live")
add_test(fixture.script_suite "$noop" "-E" "true")
set_tests_properties(fixture.script_suite PROPERTIES LABELS "live_verify")
add_test(fixture.unlabelled "$noop" "-E" "true")
"@ }

    Set-Content -LiteralPath (Join-Path $root 'CTestTestfile.cmake') -Value $body -Encoding utf8
    return $root
}

function Invoke-RunTests {
    <#
    .SYNOPSIS
        Run the entry point against a fixture tree and return its receipt.
    #>
    param(
        [Parameter(Mandatory)] [string] $BuildDir,
        [string] $ExcludeLabel = '',
        [string] $Filter = '',
        [switch] $RequireFresh
    )

    $arguments = @('-NoProfile', '-NonInteractive', '-File', $runTests, '-BuildDir', $BuildDir, '-Config', 'Debug')
    if ($ExcludeLabel) { $arguments += @('-ExcludeLabel', $ExcludeLabel) }
    if ($Filter)       { $arguments += @('-Filter', $Filter) }
    if ($RequireFresh) { $arguments += '-RequireFresh' }

    $output = (& pwsh @arguments 2>&1 | Out-String)
    $exit = $LASTEXITCODE

    $receiptPath = Join-Path $BuildDir 'Testing/last-run-receipt.json'
    $receipt = $null
    if (Test-Path -LiteralPath $receiptPath) {
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    }

    return [pscustomobject]@{ ExitCode = $exit; Output = $output; Receipt = $receipt; BuildDir = $BuildDir }
}

function Get-CTestNames {
    param([Parameter(Mandatory)] [string] $BuildDir, [string[]] $ExtraArgs = @())
    $listing = (& ctest --test-dir $BuildDir -C Debug -N @ExtraArgs 2>&1 | Out-String)
    return ([regex]::Matches($listing, '(?m)^\s*Test\s+#\d+:\s+(\S+)\s*$') | ForEach-Object { $_.Groups[1].Value })
}

Write-Host 'run-tests.ps1 contract'

Test-Case 'excluding "live" keeps the live_verify suites' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -ExcludeLabel 'live'
        Assert-True ($result.ExitCode -eq 0) "expected success, got $($result.ExitCode): $($result.Output)"
        Assert-True ($null -ne $result.Receipt) 'no receipt was written'
        Assert-True ($result.Receipt.exclude_pattern -eq '^live$') `
            "label was passed to ctest as '$($result.Receipt.exclude_pattern)', not anchored"
        Assert-True ($result.Receipt.tests_selected -eq 2) `
            "expected 2 of 3 tests selected, got $($result.Receipt.tests_selected)"
        Assert-True ($result.Receipt.tests_passed -eq 2) `
            "expected 2 tests to run, got $($result.Receipt.tests_passed)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the unanchored pattern this guards against still drops live_verify' {
    # The premise. If ctest ever stopped treating -LE as a regex, the anchoring
    # above would be testing nothing and this case would be the one to notice.
    $tree = New-FixtureTree
    try {
        $kept = Get-CTestNames -BuildDir $tree -ExtraArgs @('-LE', 'live')
        Assert-True ($kept -notcontains 'fixture.script_suite') `
            'ctest -LE live no longer drops live_verify; the anchoring guard has lost its premise'
        $anchored = Get-CTestNames -BuildDir $tree -ExtraArgs @('-LE', '^live$')
        Assert-True ($anchored -contains 'fixture.script_suite') `
            'ctest -LE ^live$ dropped live_verify'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'excluding "live_verify" still excludes exactly that label' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -ExcludeLabel 'live_verify'
        Assert-True ($result.ExitCode -eq 0) "expected success, got $($result.ExitCode): $($result.Output)"
        Assert-True ($result.Receipt.tests_selected -eq 2) `
            "expected 2 of 3 tests selected, got $($result.Receipt.tests_selected)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a caller-supplied regex is passed through unchanged' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -ExcludeLabel '^(live|live_verify)$'
        Assert-True ($result.Receipt.exclude_pattern -eq '^(live|live_verify)$') `
            "pattern was rewritten to '$($result.Receipt.exclude_pattern)'"
        Assert-True ($result.Receipt.tests_selected -eq 1) `
            "expected 1 of 3 tests selected, got $($result.Receipt.tests_selected)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a selection that matches nothing fails instead of passing' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -Filter 'no.such.test'
        Assert-True ($result.ExitCode -ne 0) 'a run over zero tests reported success'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'an empty build tree fails instead of passing' {
    $tree = New-FixtureTree -Empty
    try {
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -ne 0) 'a build tree registering no tests reported success'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a tree whose freshness cannot be proven is refused under -RequireFresh' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -RequireFresh
        Assert-True ($result.ExitCode -eq 3) `
            "expected the freshness refusal (3), got $($result.ExitCode): $($result.Output)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the receipt records the source the run describes' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree
        $head = (& git -C $scriptRoot rev-parse HEAD).Trim()
        Assert-True ($result.Receipt.source_head -eq $head) `
            "receipt names $($result.Receipt.source_head), HEAD is $head"
        Assert-True ($result.Receipt.tests_registered -eq 3) `
            "census recorded $($result.Receipt.tests_registered) registered tests, expected 3"
        Assert-True (-not $result.Receipt.census_mismatch) 'census mismatch reported for a complete run'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the process environment it mutates is restored' {
    # In-process on purpose: the leak this guards against is only observable in a
    # caller that survives the script, which is how verify.ps1 and the hooks run it.
    $tree = New-FixtureTree
    $savedConfigDir = $env:EXOSNAP_CONFIG_DIR
    $savedPath = $env:PATH
    $savedPlatform = $env:QT_QPA_PLATFORM
    try {
        & $runTests -BuildDir $tree -Config Debug *> $null
        Assert-True ($env:EXOSNAP_CONFIG_DIR -eq $savedConfigDir) `
            "EXOSNAP_CONFIG_DIR was left at '$env:EXOSNAP_CONFIG_DIR'"
        Assert-True ($env:PATH -eq $savedPath) 'PATH was left with the Qt bin directory prepended'
        Assert-True ($env:QT_QPA_PLATFORM -eq $savedPlatform) `
            "QT_QPA_PLATFORM was left at '$env:QT_QPA_PLATFORM'"
    }
    finally {
        $env:EXOSNAP_CONFIG_DIR = $savedConfigDir
        $env:PATH = $savedPath
        $env:QT_QPA_PLATFORM = $savedPlatform
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'what a failing test wrote is kept, not deleted with the config dir' {
    $tree = Join-Path ([IO.Path]::GetTempPath()) "run-tests-contract/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $tree -Force | Out-Null
    try {
        # The failing test writes into EXOSNAP_CONFIG_DIR the way the QML and
        # cursor-audit suites do, then fails. Without the rescue, the only copy
        # of that log is deleted by the cleanup on the way out.
        $failing = Join-Path $tree 'writes-a-log.ps1'
        Set-Content -LiteralPath $failing -Encoding utf8 -Value @'
Set-Content -LiteralPath (Join-Path $env:EXOSNAP_CONFIG_DIR 'app.log') -Value 'diagnostic'
exit 1
'@
        $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
        $body = "add_test(fixture.writes_a_log `"$pwshPath`" `"-NoProfile`" `"-File`" `"$($failing -replace '\\', '/')`")"
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Value $body -Encoding utf8

        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -ne 0) 'the fixture was supposed to fail'

        $rescued = $result.Receipt.rescued_config_dir
        Assert-True ($rescued) 'no config-dir rescue was recorded'
        Assert-True (Test-Path -LiteralPath (Join-Path $rescued 'app.log')) `
            "the test's log is not under $rescued"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
