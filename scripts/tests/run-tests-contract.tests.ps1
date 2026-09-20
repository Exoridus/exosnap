#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the canonical test entry point, scripts/run-tests.ps1.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Most cases point run-tests.ps1 at a throwaway build tree consisting of a
    hand-written CTestTestfile.cmake, so the real ctest answers the real
    question and nothing has to be stubbed. Two tests, two labels: "live" for
    the hardware-querying category CI excludes, and "live_verify" for a script
    suite that queries nothing and must survive that exclusion.

    The exclusion is tested from both sides, and the unanchored form is asserted
    to still be wrong. A guard whose own premise stopped being true would
    otherwise keep passing after the behaviour it protects against went away --
    or, worse, after the fix was reverted and the pattern became a plain label
    again.

    The cases about the build, the freshness and the reusable verdict need a tree
    a build system actually answers for, so those use New-ConfiguredTree: a real
    CMake project with no languages enabled, which configures and builds in about
    a second and still exercises the whole path.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptRoot
$runTests = Join-Path $scriptRoot 'run-tests.ps1'
Import-Module (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -Force

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

function New-ScratchPath {
    return Join-Path ([IO.Path]::GetTempPath()) "run-tests-contract/$([guid]::NewGuid().ToString('n'))"
}

function New-FixtureTree {
    <#
    .SYNOPSIS
        A build tree ctest can run without anything having been compiled.
    .PARAMETER Empty
        Register no tests at all, for the zero-selection cases.
    #>
    param([switch] $Empty)

    $root = New-ScratchPath
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    # An absolute command: ctest resolves a test command against the environment
    # it inherits, and a bare interpreter name is not reliably on it.
    $noop = (Get-Command cmake).Source -replace '\\', '/'

    $body = if ($Empty) { '' } else { @"
add_test(fixture.hardware "$noop" "-E" "true")
set_tests_properties(fixture.hardware PROPERTIES LABELS "live;phase.gpu")
add_test(fixture.script_suite "$noop" "-E" "true")
set_tests_properties(fixture.script_suite PROPERTIES LABELS "live_verify;phase.hermetic")
add_test(fixture.unlabelled "$noop" "-E" "true")
# Unlabelled in the sense the other cases mean: no subject label. It still
# declares a phase, because every registered test does and the runner refuses a
# tree where one does not.
set_tests_properties(fixture.unlabelled PROPERTIES LABELS "phase.hermetic")
"@ }

    Set-Content -LiteralPath (Join-Path $root 'CTestTestfile.cmake') -Value $body -Encoding utf8
    return $root
}

function New-CustomFixtureTree {
    <#
    .SYNOPSIS
        A build tree with exactly the CTestTestfile.cmake body the case needs.
    #>
    param([Parameter(Mandatory)] [string] $Body)
    $root = New-ScratchPath
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $root 'CTestTestfile.cmake') -Value $Body -Encoding utf8
    return $root
}

function New-ConfiguredTree {
    <#
    .SYNOPSIS
        A real, configured CMake build tree whose build is a no-op.
    .DESCRIPTION
        project(... NONE) needs no compiler, so this costs about a second and
        still gives the cases a tree with a build system behind it -- the only
        way to exercise the build, the freshness verdict and `reusable`, which a
        hand-written CTestTestfile.cmake can never reach.
    #>
    param([string] $ExtraCMake = '')

    $root = New-ScratchPath
    $source = Join-Path $root 'src'
    $build = Join-Path $root 'build'
    New-Item -ItemType Directory -Path $source -Force | Out-Null
    $project = @"
cmake_minimum_required(VERSION 3.20)
project(run_tests_fixture NONE)
enable_testing()
add_test(NAME fixture.trivial COMMAND "`${CMAKE_COMMAND}" -E true)
set_tests_properties(fixture.trivial PROPERTIES LABELS "phase.hermetic")
$ExtraCMake
"@
    Set-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Value $project -Encoding utf8
    & cmake -S $source -B $build -G Ninja *> $null
    if ($LASTEXITCODE -ne 0) { throw 'the fixture project did not configure' }
    return [pscustomobject]@{ Root = $root; Build = $build }
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
        [string] $Phase = '',
        # A hand-written fixture tree has no build system behind it, so the default
        # build would fail before such a case reached its subject. Every case that
        # is not about the build or the freshness refusal therefore skips both.
        [switch] $Build,
        [switch] $EnforceFreshness,
        [string] $ScriptPath = $runTests
    )

    $arguments = @('-NoProfile', '-NonInteractive', '-File', $ScriptPath, '-BuildDir', $BuildDir, '-Config', 'Debug')
    if ($ExcludeLabel) { $arguments += @('-ExcludeLabel', $ExcludeLabel) }
    if ($Filter)       { $arguments += @('-Filter', $Filter) }
    if ($Phase)        { $arguments += @('-Phase', $Phase) }
    if (-not $Build) { $arguments += '-NoBuild' }
    if (-not $EnforceFreshness) { $arguments += '-AllowStale' }

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

function Start-TreeLockHolder {
    <#
    .SYNOPSIS
        A second process that holds the tree lock of one build directory.
    .DESCRIPTION
        Signals through a file that the lock is held, so a case never races the
        child's startup. This is the barrier the contention cases wait on instead
        of sleeping and hoping.
    #>
    param([Parameter(Mandatory)] [string] $Path)
    $dir = New-ScratchPath
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $held = Join-Path $dir 'held'
    $release = Join-Path $dir 'release'
    $module = (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -replace '\\', '/'

    $body = @"
Import-Module '$module' -Force
`$lock = Enter-HostLock -Kind 'tree' -Path '$($Path -replace '\\', '/')' -Holder 'foreign'
Set-Content -LiteralPath '$($held -replace '\\', '/')' -Value 'held'
while (-not (Test-Path -LiteralPath '$($release -replace '\\', '/')')) { Start-Sleep -Milliseconds 50 }
Exit-HostLock -Lock `$lock
"@
    $process = Start-Process -FilePath 'pwsh' -PassThru -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-NonInteractive', '-Command', $body)

    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not (Test-Path -LiteralPath $held) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path -LiteralPath $held)) {
        $process.Kill()
        throw 'the foreign holder never reported that it took the tree lock'
    }
    return [pscustomobject]@{ Process = $process; Release = $release; Dir = $dir }
}

function Stop-TreeLockHolder {
    param([Parameter(Mandatory)] $Holder)
    Set-Content -LiteralPath $Holder.Release -Value 'go'
    if (-not $Holder.Process.WaitForExit(15000)) { $Holder.Process.Kill() }
    Remove-Item -LiteralPath $Holder.Dir -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'run-tests.ps1 contract'

# --- Selection and labels ----------------------------------------------------

Test-Case 'a tree with a test that declares no phase is refused, and the test is named' {
    # The guard the phase selection rests on. Without it `-Phase hermetic` would
    # quietly leave an undeclared test out and a CI lane built on that would report
    # a green suite it never ran. The name matters as much as the refusal: a count
    # tells nobody which CMake line to fix.
    $tree = New-FixtureTree
    try {
        $file = Join-Path $tree 'CTestTestfile.cmake'
        $kept = Get-Content -LiteralPath $file |
            Where-Object { $_ -notmatch 'set_tests_properties\(fixture\.unlabelled' }
        Set-Content -LiteralPath $file -Value ($kept -join "`n") -Encoding utf8

        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -ne 0) 'a tree with an undeclared test was accepted'
        Assert-True ($result.Output -match 'no phase') "the refusal did not say what was missing: $($result.Output)"
        Assert-True ($result.Output -match 'fixture\.unlabelled') 'the refusal did not name the test'
        Assert-True ($result.Receipt.phase_violations.missing -contains 'fixture.unlabelled') `
            'the receipt did not record which test was missing a phase'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a test with two phases is refused although the totals add up' {
    # The case a total cannot see. One test with no phase and one with two: the
    # sum of phase labels equals the number of registered tests, so a check built
    # on totals reports a healthy tree -- while `-Phase hermetic` silently leaves
    # the first test out.
    $noop = (Get-Command cmake).Source -replace '\\', '/'
    $tree = New-CustomFixtureTree -Body @"
add_test(fixture.no_phase "$noop" "-E" "true")
add_test(fixture.two_phases "$noop" "-E" "true")
set_tests_properties(fixture.two_phases PROPERTIES LABELS "phase.hermetic;phase.gpu")
add_test(fixture.one_phase "$noop" "-E" "true")
set_tests_properties(fixture.one_phase PROPERTIES LABELS "phase.hermetic")
"@
    try {
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 4) `
            "expected the invalid-run code 4, got $($result.ExitCode): $($result.Output)"
        Assert-True ($result.Receipt.phase_violations.missing -contains 'fixture.no_phase') `
            'the test with no phase was not reported'
        Assert-True (@($result.Receipt.phase_violations.multiple) -match 'fixture\.two_phases') `
            'the test with two phases was not reported'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a phase label outside the vocabulary is refused' {
    # A misspelled phase is not a phase. Left alone it would register as declared
    # and be selected by nothing.
    $noop = (Get-Command cmake).Source -replace '\\', '/'
    $tree = New-CustomFixtureTree -Body @"
add_test(fixture.typo "$noop" "-E" "true")
set_tests_properties(fixture.typo PROPERTIES LABELS "phase.hermitic")
"@
    try {
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 4) "expected 4, got $($result.ExitCode): $($result.Output)"
        Assert-True (@($result.Receipt.phase_violations.unknown) -match 'fixture\.typo') `
            'the unknown phase label was not reported'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a phase selects exactly the tests that declare it' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -Phase 'gpu'
        Assert-True ($result.ExitCode -eq 0) "expected success, got $($result.ExitCode): $($result.Output)"
        Assert-True ($result.Receipt.phase -eq 'gpu') `
            "the receipt records phase '$($result.Receipt.phase)'"
        Assert-True ($result.Receipt.tests_selected -eq 1) `
            "expected 1 of 3 tests selected, got $($result.Receipt.tests_selected)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a phase name the vocabulary does not know is refused before anything runs' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -Phase 'gpu-ish'
        Assert-True ($result.ExitCode -ne 0) 'an unknown phase name was accepted'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

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

# --- Census ------------------------------------------------------------------

Test-Case 'a disabled test is accounted for as disabled, not as a missing result' {
    # ctest leaves a disabled test out of the summary it prints. Comparing that
    # summary against the whole selection therefore reported a contradiction for a
    # healthy tree -- and a check that cries wolf on a healthy tree is one nobody
    # can afford to make fatal.
    $noop = (Get-Command cmake).Source -replace '\\', '/'
    $tree = New-CustomFixtureTree -Body @"
add_test(fixture.runs "$noop" "-E" "true")
set_tests_properties(fixture.runs PROPERTIES LABELS "phase.hermetic")
add_test(fixture.disabled "$noop" "-E" "true")
set_tests_properties(fixture.disabled PROPERTIES LABELS "phase.hermetic" DISABLED TRUE)
"@
    try {
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 0) "expected success, got $($result.ExitCode): $($result.Output)"
        Assert-True (-not $result.Receipt.census_mismatch) `
            'a disabled test was reported as a census contradiction'
        Assert-True ($result.Receipt.tests_selected -eq 2) "selected $($result.Receipt.tests_selected), expected 2"
        Assert-True ($result.Receipt.tests_disabled -eq 1) "disabled $($result.Receipt.tests_disabled), expected 1"
        Assert-True ($result.Receipt.tests_expected -eq 1) "expected-to-run $($result.Receipt.tests_expected), expected 1"
        Assert-True ($result.Receipt.tests_accounted -eq 1) "accounted $($result.Receipt.tests_accounted), expected 1"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a run whose output could not be recorded is not reported as a pass' {
    # The other half of the census predicate: no summary to read means nothing was
    # accounted for, whatever the suite itself did. The log path is occupied by a
    # directory here, which is the cheapest way to make the recording fail.
    $tree = New-FixtureTree
    try {
        New-Item -ItemType Directory -Path (Join-Path $tree 'Testing/last-run.log') -Force | Out-Null
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 4) `
            "expected the invalid-run code 4, got $($result.ExitCode): $($result.Output)"
        Assert-True ($null -ne $result.Receipt) 'a run that failed to record itself wrote no receipt'
        Assert-True (-not $result.Receipt.reusable) 'a run with no readable output was marked reusable'
        Assert-True (@($result.Receipt.invalid_reasons).Count -gt 0) 'the receipt gave no reason'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

# --- Build, freshness and the reusable verdict -------------------------------

Test-Case 'a failed build stops before the suite runs and says so in the receipt' {
    # The failure the default build exists to prevent: a build that fails leaves the
    # previous binaries in place, and a suite run against them passes. A hand-written
    # fixture tree has no build system, so cmake --build fails here the way a broken
    # compile does.
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -Build
        Assert-True ($result.ExitCode -ne 0) 'a failed build reported success'
        Assert-True ($null -ne $result.Receipt) 'a failed build left no receipt of its own'
        Assert-True ($result.Receipt.build_status -eq 'failed') `
            "the receipt records build_status '$($result.Receipt.build_status)'"
        Assert-True (-not $result.Receipt.reusable) 'a failed build produced a reusable receipt'
        Assert-True ($null -eq $result.Receipt.ctest_exit_code) 'the suite ran anyway after a failed build'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a failing run replaces an older successful receipt' {
    # Otherwise the newest file in the tree is a green verdict from a run that is
    # not the one that just happened, and every consumer downstream reads it as
    # this run's answer.
    $fixture = New-ConfiguredTree
    try {
        $good = Invoke-RunTests -BuildDir $fixture.Build -Build -EnforceFreshness
        Assert-True ($good.ExitCode -eq 0) "the baseline run failed: $($good.Output)"
        Assert-True ($good.Receipt.reusable) 'a clean, freshly built, undrifted run was not marked reusable'
        $goodRunId = $good.Receipt.run_id

        $bad = Invoke-RunTests -BuildDir $fixture.Build -Filter 'no.such.test' -Build -EnforceFreshness
        Assert-True ($bad.ExitCode -ne 0) 'the second run was supposed to fail'
        Assert-True ($bad.Receipt.run_id -ne $goodRunId) `
            'the failing run left the previous receipt in place as its own result'
        Assert-True (-not $bad.Receipt.reusable) 'the failing run published a reusable receipt'
    }
    finally { Remove-Item -LiteralPath $fixture.Root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a tree whose freshness cannot be proven is refused under -NoBuild' {
    # The refusal is the default because the failure it prevents is silent: a build
    # that failed leaves the previous binaries in place, and a suite run against
    # them passes and reads exactly like a pass for the change.
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree -EnforceFreshness
        Assert-True ($result.ExitCode -eq 3) `
            "expected the freshness refusal (3), got $($result.ExitCode): $($result.Output)"
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case '-AllowStale reports a result and says which binaries it describes' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 0) `
            "-AllowStale did not run the suite: $($result.Output)"
        Assert-True ($result.Output -match 'OLD binaries') `
            "-AllowStale ran without saying the result may describe old binaries: $($result.Output)"
        Assert-True (-not $result.Receipt.reusable) `
            'a result that may describe old binaries was published as reusable'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

# --- Source identity ---------------------------------------------------------

Test-Case 'the receipt records the source identity at both ends of the run' {
    $tree = New-FixtureTree
    try {
        $result = Invoke-RunTests -BuildDir $tree
        $head = (& git -C $scriptRoot rev-parse HEAD).Trim()
        Assert-True ($result.Receipt.source_before.head -eq $head) `
            "receipt names $($result.Receipt.source_before.head), HEAD is $head"
        Assert-True ($result.Receipt.source_before.ok -and $result.Receipt.source_after.ok) `
            'one end of the run could not identify its source'
        Assert-True ($result.Receipt.source_before.fingerprint -eq $result.Receipt.source_after.fingerprint) `
            'the source moved during a run that touched nothing'
        Assert-True ($result.Receipt.source_drift -eq $false) 'drift was reported for an unchanged tree'
        Assert-True ($result.Receipt.tests_registered -eq 3) `
            "census recorded $($result.Receipt.tests_registered) registered tests, expected 3"
        Assert-True (-not $result.Receipt.census_mismatch) 'census mismatch reported for a complete run'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a working tree that changes during the run invalidates the result' {
    # Against a COPY of scripts/ in a repository of its own, never this one: a case
    # that dirtied the real working tree would make every other run of the suite
    # report drift, which is precisely the behaviour under test.
    $clone = New-ScratchPath
    New-Item -ItemType Directory -Path $clone -Force | Out-Null
    $fixture = $null
    try {
        Copy-Item -Path (Join-Path $repoRoot 'scripts') -Destination $clone -Recurse -Force
        Copy-Item -Path (Join-Path $repoRoot '.qt-version') -Destination $clone -Force
        & git -C $clone init --quiet 2>&1 | Out-Null
        & git -C $clone config user.email 'tests@exosnap.invalid' | Out-Null
        & git -C $clone config user.name 'ExoSnap tests' | Out-Null
        & git -C $clone add -A 2>&1 | Out-Null
        & git -C $clone commit -m 'clone' --quiet 2>&1 | Out-Null

        # The test itself is what moves the source, midway through the run.
        $mutator = Join-Path $clone 'mutate.ps1'
        Set-Content -LiteralPath $mutator -Encoding utf8 -Value @"
Set-Content -LiteralPath '$(($clone -replace '\\', '/'))/appeared-during-the-run.txt' -Value 'new source'
exit 0
"@
        $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
        $fixture = New-CustomFixtureTree -Body @"
add_test(fixture.mutates_the_tree "$pwshPath" "-NoProfile" "-File" "$($mutator -replace '\\', '/')")
set_tests_properties(fixture.mutates_the_tree PROPERTIES LABELS "phase.hermetic")
"@
        $result = Invoke-RunTests -BuildDir $fixture -ScriptPath (Join-Path $clone 'scripts/run-tests.ps1')
        Assert-True ($result.Receipt.source_drift -eq $true) `
            "the run did not notice the source moving: drift=$($result.Receipt.source_drift)"
        Assert-True ($result.ExitCode -eq 4) `
            "a drifted run was not reported as invalid (exit $($result.ExitCode))"
        Assert-True (-not $result.Receipt.reusable) 'a drifted run was published as reusable'
        Assert-True (@($result.Receipt.invalid_reasons) -match 'changed while') 'the receipt gave no drift reason'
    }
    finally {
        if ($fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $clone -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- The build tree is one run's at a time -----------------------------------

Test-Case 'a second run does not touch a build tree another run is holding' {
    # The A1 invariant. Without it a second entry point rebuilds the tree while
    # this one is deciding what it holds, and the result describes binaries nobody
    # can name afterwards.
    $tree = New-FixtureTree
    $holder = Start-TreeLockHolder -Path $tree
    $savedTimeout = $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS
    try {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = '2'
        $result = Invoke-RunTests -BuildDir $tree -Build
        Assert-True ($result.ExitCode -ne 0) 'a run took a build tree another holder owns'
        Assert-True ($result.Output -match 'held by another run') `
            "the refusal did not name the contention: $($result.Output)"
        Assert-True ($result.Output -notmatch 'Building all targets') `
            'the run started building a tree it had not acquired'
    }
    finally {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = $savedTimeout
        Stop-TreeLockHolder -Holder $holder
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'an unrelated build tree is not blocked by a run on this one' {
    # The other half: the lock is bound to one directory, so two worktrees are not
    # serialised for the duration of each other's test runs.
    $held = New-FixtureTree
    $free = New-FixtureTree
    $holder = Start-TreeLockHolder -Path $held
    $savedTimeout = $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS
    try {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = '5'
        $result = Invoke-RunTests -BuildDir $free
        Assert-True ($result.ExitCode -eq 0) `
            "an independent build tree waited on an unrelated one: $($result.Output)"
    }
    finally {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = $savedTimeout
        Stop-TreeLockHolder -Holder $holder
        Remove-Item -LiteralPath $held -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $free -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'the tree lock is held while the suite runs and released again afterwards' {
    # Observed from outside through a barrier the test itself writes, so nothing
    # here depends on a sleep being long enough.
    $signalDir = New-ScratchPath
    New-Item -ItemType Directory -Path $signalDir -Force | Out-Null
    $running = Join-Path $signalDir 'running'
    $finish = Join-Path $signalDir 'finish'

    $waiter = Join-Path $signalDir 'wait.ps1'
    Set-Content -LiteralPath $waiter -Encoding utf8 -Value @"
Set-Content -LiteralPath '$($running -replace '\\', '/')' -Value 'running'
`$deadline = [DateTime]::UtcNow.AddSeconds(30)
while (-not (Test-Path -LiteralPath '$($finish -replace '\\', '/')') -and [DateTime]::UtcNow -lt `$deadline) {
    Start-Sleep -Milliseconds 50
}
exit 0
"@
    $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
    $tree = New-CustomFixtureTree -Body @"
add_test(fixture.waits "$pwshPath" "-NoProfile" "-File" "$($waiter -replace '\\', '/')")
set_tests_properties(fixture.waits PROPERTIES LABELS "phase.hermetic")
"@
    try {
        $process = Start-Process -FilePath 'pwsh' -PassThru -WindowStyle Hidden -ArgumentList @(
            '-NoProfile', '-NonInteractive', '-File', $runTests,
            '-BuildDir', $tree, '-Config', 'Debug', '-NoBuild', '-AllowStale')

        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not (Test-Path -LiteralPath $running) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 50
        }
        Assert-True (Test-Path -LiteralPath $running) 'the fixture test never started'
        Assert-True (Test-HostLockHeld -Kind 'tree' -Path $tree) `
            'the suite is running and the build tree is not locked'

        Set-Content -LiteralPath $finish -Value 'go'
        Assert-True ($process.WaitForExit(60000)) 'the run did not finish'
        Assert-True (-not (Test-HostLockHeld -Kind 'tree' -Path $tree)) `
            'the tree lock outlived the run that took it'
    }
    finally {
        Set-Content -LiteralPath $finish -Value 'go' -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $signalDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'the tree lock is still held while the receipt is being written' {
    # The hold has to outlast the verdict, not just the suite. Released earlier, a
    # second run takes the tree and starts changing it while this one is still
    # taking its closing fingerprint and writing the shared receipt -- and the
    # receipt then describes a tree somebody else already owns.
    #
    # Observed by pausing the run at that exact point, not by racing it: a second
    # process waiting on the mutex loses to wake-up latency and reports a pass
    # whichever order the code actually uses.
    $signalDir = New-ScratchPath
    New-Item -ItemType Directory -Path $signalDir -Force | Out-Null
    $barrier = Join-Path $signalDir 'go'
    $reached = "$barrier.reached"

    $tree = New-FixtureTree
    $receiptPath = Join-Path $tree 'Testing/last-run-receipt.json'
    $savedPause = $env:EXOSNAP_RUNTESTS_PAUSE
    $process = $null
    try {
        $env:EXOSNAP_RUNTESTS_PAUSE = "beforeReceipt=$barrier"
        $process = Start-Process -FilePath 'pwsh' -PassThru -WindowStyle Hidden -ArgumentList @(
            '-NoProfile', '-NonInteractive', '-File', $runTests,
            '-BuildDir', $tree, '-Config', 'Debug', '-NoBuild', '-AllowStale')

        $deadline = [DateTime]::UtcNow.AddSeconds(90)
        while (-not (Test-Path -LiteralPath $reached) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 50
        }
        Assert-True (Test-Path -LiteralPath $reached) 'the run never reached the receipt step'
        Assert-True (-not (Test-Path -LiteralPath $receiptPath)) `
            'the pause point is after the publish, so this case proves nothing'
        Assert-True (Test-HostLockHeld -Kind 'tree' -Path $tree) `
            'the receipt is being written and the build tree is already free for another run'

        Set-Content -LiteralPath $barrier -Value 'go'
        Assert-True ($process.WaitForExit(90000)) 'the run did not finish'
        Assert-True (Test-Path -LiteralPath $receiptPath) 'the run published no receipt'
        Assert-True (-not (Test-HostLockHeld -Kind 'tree' -Path $tree)) `
            'the tree lock outlived the run that took it'
    }
    finally {
        Set-Content -LiteralPath $barrier -Value 'go' -ErrorAction SilentlyContinue
        if ($null -eq $savedPause) { Remove-Item Env:EXOSNAP_RUNTESTS_PAUSE -ErrorAction SilentlyContinue }
        else { $env:EXOSNAP_RUNTESTS_PAUSE = $savedPause }
        if ($process -and -not $process.HasExited) { $process.Kill() }
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $signalDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'a run that never got the tree lock leaves the holder"s receipt alone' {
    # The receipt describes the tree, and the tree belongs to whoever holds it. A
    # run that timed out waiting has no verdict about those binaries at all, so
    # overwriting the file would replace someone else's result with this run's
    # inability to start.
    $tree = New-FixtureTree
    $receiptPath = Join-Path $tree 'Testing/last-run-receipt.json'
    New-Item -ItemType Directory -Path (Join-Path $tree 'Testing') -Force | Out-Null
    $sentinel = '{"exit_code":0,"reusable":true,"owner":"the run that holds the tree"}'
    Set-Content -LiteralPath $receiptPath -Value $sentinel -Encoding utf8 -NoNewline

    $holder = Start-TreeLockHolder -Path $tree
    $savedTimeout = $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS
    try {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = '2'
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -ne 0) 'a run took a build tree another holder owns'
        Assert-True ((Get-Content -LiteralPath $receiptPath -Raw) -eq $sentinel) `
            "a run that never acquired the tree lock rewrote the receipt: $(Get-Content -LiteralPath $receiptPath -Raw)"
    }
    finally {
        $env:EXOSNAP_HOST_LOCK_TIMEOUT_SECONDS = $savedTimeout
        Stop-TreeLockHolder -Holder $holder
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'a run that aborts after the suite still secures what the tests wrote' {
    # The gap the rescue used to have: it sat on the line after the suite call, so
    # a failure while recording or wrapping that call -- with the tests already run
    # and their logs already written -- threw straight past it, and the cleanup
    # then deleted the only copy.
    #
    # The fixture test PASSES here on purpose. The rescue must therefore be driven
    # by the missing verdict alone, not by a failing one, which is the distinction
    # between the two paths.
    $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
    $tree = New-ScratchPath
    New-Item -ItemType Directory -Path $tree -Force | Out-Null
    $savedFault = $env:EXOSNAP_RUNTESTS_FAULT
    try {
        $writer = Join-Path $tree 'writes-a-log.ps1'
        Set-Content -LiteralPath $writer -Encoding utf8 -Value @'
Set-Content -LiteralPath (Join-Path $env:EXOSNAP_CONFIG_DIR 'app.log') -Value 'diagnostic'
exit 0
'@
        $body = @(
            "add_test(fixture.writes_a_log `"$pwshPath`" `"-NoProfile`" `"-File`" `"$($writer -replace '\\', '/')`")"
            'set_tests_properties(fixture.writes_a_log PROPERTIES LABELS "phase.hermetic")'
        ) -join "`n"
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Value $body -Encoding utf8

        $env:EXOSNAP_RUNTESTS_FAULT = 'afterSuiteBeforeVerdict'
        $result = Invoke-RunTests -BuildDir $tree

        Assert-True ($result.ExitCode -eq 4) `
            "expected the invalid-run code 4, got $($result.ExitCode): $($result.Output)"
        Assert-True ($null -ne $result.Receipt) 'the aborted run wrote no receipt'
        Assert-True ($result.Receipt.rescue_status -eq 'rescued') `
            "an aborted run reported rescue status '$($result.Receipt.rescue_status)'"
        Assert-True (Test-Path -LiteralPath (Join-Path $result.Receipt.rescued_config_dir 'app.log')) `
            "the log the suite wrote is not under $($result.Receipt.rescued_config_dir)"
        Assert-True (@($result.Receipt.invalid_reasons) -match 'aborted') `
            'an aborted run did not say so in the receipt'
    }
    finally {
        if ($null -eq $savedFault) { Remove-Item Env:EXOSNAP_RUNTESTS_FAULT -ErrorAction SilentlyContinue }
        else { $env:EXOSNAP_RUNTESTS_FAULT = $savedFault }
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'a delegated child runs under the parent hold instead of waiting on itself' {
    # A mutex is recursive for the owning thread and for nothing else. Without the
    # inheritance mark, a test that starts a process needing this same build tree
    # would wait out the deadline on a lock its own parent holds.
    $module = (Join-Path $scriptRoot 'lib/HostResourceLock.psm1') -replace '\\', '/'
    $signalDir = New-ScratchPath
    New-Item -ItemType Directory -Path $signalDir -Force | Out-Null
    $child = Join-Path $signalDir 'child.ps1'
    $tree = $null
    try {
        $tree = New-ScratchPath
        New-Item -ItemType Directory -Path $tree -Force | Out-Null
        Set-Content -LiteralPath $child -Encoding utf8 -Value @"
Import-Module '$module' -Force
`$lock = Enter-HostLock -Kind 'tree' -Path '$($tree -replace '\\', '/')' -Timeout ([TimeSpan]::FromSeconds(3))
if (-not `$lock.Inherited) { exit 4 }
exit 0
"@
        $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Encoding utf8 -Value @"
add_test(fixture.delegates "$pwshPath" "-NoProfile" "-File" "$($child -replace '\\', '/')")
set_tests_properties(fixture.delegates PROPERTIES LABELS "phase.hermetic")
"@
        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -eq 0) `
            "the delegated child did not run under the parent hold: $($result.Output)"
    }
    finally {
        if ($tree) { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $signalDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'the tree lock is released after a build failure and after a test failure' {
    # A lock a failed run kept would stop the next attempt at the same tree, which
    # is the failure mode that makes people take locks out again.
    $tree = New-FixtureTree
    try {
        Invoke-RunTests -BuildDir $tree -Build | Out-Null
        Assert-True (-not (Test-HostLockHeld -Kind 'tree' -Path $tree)) 'a failed build kept the tree lock'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }

    $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
    $failing = New-CustomFixtureTree -Body @"
add_test(fixture.fails "$pwshPath" "-NoProfile" "-Command" "exit 1")
set_tests_properties(fixture.fails PROPERTIES LABELS "phase.hermetic")
"@
    try {
        $result = Invoke-RunTests -BuildDir $failing
        Assert-True ($result.ExitCode -ne 0) 'the fixture was supposed to fail'
        Assert-True (-not (Test-HostLockHeld -Kind 'tree' -Path $failing)) 'a failed suite kept the tree lock'
    }
    finally { Remove-Item -LiteralPath $failing -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'every build-tree command in verify.ps1 goes through the tree lock' {
    # The invariant spans entry points, so testing run-tests against run-tests
    # proves only half of it: verify.ps1 builds the same directory with the host
    # `build` lock, which is a different lock and excludes nothing a test run
    # holds. Read out of the source, because driving verify.ps1's build step needs
    # the real preset and the real compiler.
    $verify = Join-Path $scriptRoot 'verify.ps1'
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($verify, [ref]$null, [ref]$errors)
    Assert-True (-not $errors) 'verify.ps1 does not parse'

    $cmakeCalls = @($ast.FindAll({
        param($node)
        if ($node -isnot [System.Management.Automation.Language.CommandAst]) { return $false }
        if ($node.GetCommandName() -ne 'Invoke-Step') { return $false }
        $text = $node.Extent.Text
        return ($text -match "-FilePath\s+'cmake'") -and ($text -match '--build|--preset')
    }, $true))
    Assert-True ($cmakeCalls.Count -ge 3) `
        "expected verify.ps1 to drive cmake for configure, qmllint and build; found $($cmakeCalls.Count)"

    foreach ($call in $cmakeCalls) {
        $node = $call.Parent
        $wrapped = $false
        while ($node) {
            if ($node -is [System.Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -eq 'Invoke-BuildTreeStep') { $wrapped = $true; break }
            $node = $node.Parent
        }
        Assert-True $wrapped ("a cmake invocation in verify.ps1 writes the build tree outside the tree lock: " +
            $call.Extent.Text)
    }

    $helper = ($ast.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq 'Invoke-BuildTreeStep'
    }, $true) | Select-Object -First 1)
    Assert-True ($null -ne $helper) 'verify.ps1 has no Invoke-BuildTreeStep'
    Assert-True ($helper.Extent.Text -match "-Kind\s+'tree'") `
        'Invoke-BuildTreeStep does not take the tree lock'
}

# --- Evidence ----------------------------------------------------------------

Test-Case 'what a failing test wrote is kept under a path of its own per run' {
    $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
    $tree = New-ScratchPath
    New-Item -ItemType Directory -Path $tree -Force | Out-Null
    try {
        # The failing test writes into EXOSNAP_CONFIG_DIR the way the QML and
        # cursor-audit suites do, then fails. Without the rescue, the only copy
        # of that log is deleted by the cleanup on the way out.
        $failing = Join-Path $tree 'writes-a-log.ps1'
        Set-Content -LiteralPath $failing -Encoding utf8 -Value @'
Set-Content -LiteralPath (Join-Path $env:EXOSNAP_CONFIG_DIR 'app.log') -Value "diagnostic $([guid]::NewGuid())"
exit 1
'@
        # Every registered test declares a phase; the runner refuses a tree where
        # one does not, and this fixture is a tree.
        $body = @(
            "add_test(fixture.writes_a_log `"$pwshPath`" `"-NoProfile`" `"-File`" `"$($failing -replace '\\', '/')`")"
            'set_tests_properties(fixture.writes_a_log PROPERTIES LABELS "phase.hermetic")'
        ) -join "`n"
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Value $body -Encoding utf8

        $first = Invoke-RunTests -BuildDir $tree
        Assert-True ($first.ExitCode -ne 0) 'the fixture was supposed to fail'
        $firstRescue = $first.Receipt.rescued_config_dir
        Assert-True ($first.Receipt.rescue_status -eq 'rescued') `
            "rescue status was '$($first.Receipt.rescue_status)'"
        Assert-True (Test-Path -LiteralPath (Join-Path $firstRescue 'app.log')) `
            "the test's log is not under $firstRescue"
        $firstContent = Get-Content -LiteralPath (Join-Path $firstRescue 'app.log') -Raw

        # A second run must not overwrite the first run's evidence, which is what a
        # single fixed directory did.
        $second = Invoke-RunTests -BuildDir $tree
        $secondRescue = $second.Receipt.rescued_config_dir
        Assert-True ($secondRescue -ne $firstRescue) 'two runs rescued their evidence to the same directory'
        Assert-True (Test-Path -LiteralPath (Join-Path $firstRescue 'app.log')) `
            'the second run destroyed the first run"s evidence'
        Assert-True ((Get-Content -LiteralPath (Join-Path $firstRescue 'app.log') -Raw) -eq $firstContent) `
            'the first run"s evidence was overwritten in place'
    }
    finally { Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a rescue that cannot complete keeps the original and reports the failure' {
    # The worst outcome is a run that reports `rescued` and deleted the only copy.
    # The destination is occupied by a file here, so creating the run directory
    # under it fails the way a full or locked disk would.
    $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
    $tree = New-ScratchPath
    New-Item -ItemType Directory -Path $tree -Force | Out-Null
    $keptConfigDir = $null
    try {
        $failing = Join-Path $tree 'writes-a-log.ps1'
        Set-Content -LiteralPath $failing -Encoding utf8 -Value @'
Set-Content -LiteralPath (Join-Path $env:EXOSNAP_CONFIG_DIR 'app.log') -Value 'diagnostic'
exit 1
'@
        $body = @(
            "add_test(fixture.writes_a_log `"$pwshPath`" `"-NoProfile`" `"-File`" `"$($failing -replace '\\', '/')`")"
            'set_tests_properties(fixture.writes_a_log PROPERTIES LABELS "phase.hermetic")'
        ) -join "`n"
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Value $body -Encoding utf8

        New-Item -ItemType Directory -Path (Join-Path $tree 'Testing') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $tree 'Testing/last-run-config-dir') -Value 'not a directory' -Encoding utf8

        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.Receipt.rescue_status -eq 'failed') `
            "a rescue that could not run reported '$($result.Receipt.rescue_status)'"
        $keptConfigDir = $result.Receipt.rescued_config_dir
        Assert-True (Test-Path -LiteralPath (Join-Path $keptConfigDir 'app.log')) `
            "the original evidence was deleted after a failed rescue: $keptConfigDir"
        Assert-True ($result.Output -match 'original directory is kept') `
            'the run did not say where the evidence still is'
        Assert-True (@($result.Receipt.invalid_reasons) -match 'evidence') `
            'evidence that could not be secured did not make the run invalid'
    }
    finally {
        if ($keptConfigDir) { Remove-Item -LiteralPath $keptConfigDir -Recurse -Force -ErrorAction SilentlyContinue }
        Remove-Item -LiteralPath $tree -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Test-Case 'a failing test is named in the summary even though it carries labels' {
    $tree = New-ScratchPath
    New-Item -ItemType Directory -Path $tree -Force | Out-Null
    try {
        # ctest writes its failure list as "<n> - <name> (Failed)" and appends the
        # test's LABELS after the status. Every test in this repository declares a
        # phase, so a summary that expects the line to end at the status names
        # nothing -- which is what sent a reader to a CI artifact for a single name.
        $pwshPath = (Get-Process -Id $PID).Path -replace '\\', '/'
        $body = @(
            "add_test(fixture.labelled_failure `"$pwshPath`" `"-NoProfile`" `"-Command`" `"exit 1`")"
            'set_tests_properties(fixture.labelled_failure PROPERTIES LABELS "phase.hermetic;quick")'
        ) -join "`n"
        Set-Content -LiteralPath (Join-Path $tree 'CTestTestfile.cmake') -Value $body -Encoding utf8

        $result = Invoke-RunTests -BuildDir $tree
        Assert-True ($result.ExitCode -ne 0) 'the fixture was supposed to fail'
        Assert-True ($result.Output -match 'Failed test binaries') 'the summary has to say that something failed'
        Assert-True ($result.Output -match 'fixture\.labelled_failure') 'and which test it was'
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

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
