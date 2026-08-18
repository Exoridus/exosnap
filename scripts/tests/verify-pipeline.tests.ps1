#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the local verification orchestrator: scope, order, dependency
    truthfulness and the QML diagnostic contract.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use, so CTest
    runs all of them the same way and a contributor reads one style.

    Every check command is a test double. Nothing here configures, builds,
    compiles, or runs a real test binary -- which is the point: the property under
    test is what the orchestrator CLAIMS, and the cheapest way to prove a claim is
    wrong is to make the underlying step lie on demand.

    The assertions that matter most are the negative ones. A pipeline that reports
    PASS is only worth something if it has been shown to report FAIL when it
    should, and to refuse to report PASS for a step whose prerequisite did not run.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
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

$S = Get-VerifyStatusName

function Get-CheckStatus {
    param([hashtable] $Run, [string] $Name)
    $entry = @($Run.checks | Where-Object { $_.name -eq $Name })
    if ($entry.Count -ne 1) { throw "check '$Name' appears $($entry.Count) time(s) in the run" }
    return $entry[0]
}

function New-FakeExecutor {
    <#
    .SYNOPSIS
        An executor that passes everything except the named checks.
    .PARAMETER FailingChecks
        Check names that report FAIL.
    .PARAMETER Log
        A list the executor appends every executed check name to, so a test can
        assert what was NOT started.
    #>
    param([string[]] $FailingChecks = @(), [System.Collections.Generic.List[string]] $Log, [hashtable] $Extra = @{})
    return {
        param($check, $context)
        if ($Log) { $Log.Add($check.Name) }
        if ($FailingChecks -contains $check.Name) {
            $outcome = @{ Status = $S.Fail; Detail = 'fake failure' }
            if ($Extra.ContainsKey($check.Name)) { $outcome += $Extra[$check.Name] }
            return $outcome
        }
        return @{ Status = $S.Pass }
    }.GetNewClosure()
}

Write-Host ''
Write-Host 'Scope'

Test-Case 'a .cpp change requires a build and the whole suite' {
    $scope = Get-VerifyScope -ChangedFiles @('libs/recorder_core/src/muxer.cpp')
    Assert-True $scope.RequiresBuild 'a compiled source change must require a build'
    Assert-True $scope.RequiresTests 'a compiled source change must require tests'
    Assert-True $scope.RequiresFullTests 'a compiled source change may not narrow the suite'
    Assert-Equal '' $scope.TestFilter 'a full-suite run carries no filter'
}

Test-Case 'a C++ test under app/ runs its own tests, not only the Quick ones' {
    # The narrowing this replaces mapped every app/ source to "^quick\.". app/
    # holds dozens of gtest binaries under their own prefixes, so a change to one
    # of them ran the Quick UI suite and skipped the tests belonging to the file
    # that had changed -- a PASS that had never executed the relevant test.
    $scope = Get-VerifyScope -ChangedFiles @('app/tests/test_whats_new_payload.cpp')
    Assert-True $scope.RequiresFullTests 'an app/ source may not be narrowed to the Quick tests'
    Assert-True ($scope.TestFilter -notmatch 'quick') 'the Quick-only filter must be gone'
}

Test-Case 'a header change escalates to the full test suite' {
    $scope = Get-VerifyScope -ChangedFiles @('libs/recorder_core/include/recorder_core/session.h')
    Assert-True $scope.RequiresFullTests 'a header must escalate: its dependents are not resolved'
    Assert-Equal '' $scope.TestFilter 'a full-suite run carries no filter'
    Assert-True ($scope.EscalationReasons.Count -gt 0) 'the escalation has to be stated, not silent'
}

Test-Case 'a CMake change escalates' {
    foreach ($file in @('CMakeLists.txt', 'app/CMakeLists.txt', 'CMakePresets.json', 'cmake/tests/x.cmake')) {
        $scope = Get-VerifyScope -ChangedFiles @($file)
        Assert-True $scope.RequiresConfigure "$file must force a configure"
        Assert-True $scope.RequiresFullTests "$file must force the full suite"
        Assert-True $scope.RequiresQmlLint "$file must force qmllint"
    }
}

Test-Case 'a QML change maps to qmllint and the Quick tests' {
    $scope = Get-VerifyScope -ChangedFiles @('app/quick/ExoSnap/Quick/RecordPage.qml')
    Assert-True $scope.RequiresQmlLint 'QML must be linted'
    Assert-True $scope.RequiresBuild 'QML is compiled into the module, so it must be rebuilt before it is tested'
    Assert-Equal '^quick\.' $scope.TestFilter 'QML maps to the Quick tests'
}

Test-Case 'a workflow-only change compiles nothing' {
    $scope = Get-VerifyScope -ChangedFiles @('.github/workflows/ci.yml', '.github/actions/setup-qt/action.yml')
    Assert-True (-not $scope.RequiresBuild) 'a workflow edit must not trigger a product build'
    Assert-True (-not $scope.RequiresTests) 'a workflow edit must not trigger the test suite'
    Assert-True (-not $scope.RequiresConfigure) 'a workflow edit must not trigger a configure'
}

Test-Case 'a git-hook change compiles nothing' {
    # The hooks have no file extension. Before they were named here they fell to
    # the default "unrecognised, escalate everything" rule, and editing a hook
    # kicked off a full configure and build -- conservative, but wrong about a
    # shell script that is never compiled.
    $scope = Get-VerifyScope -ChangedFiles @('.githooks/pre-commit', '.githooks/pre-push')
    Assert-True (-not $scope.RequiresBuild) 'a git hook must not trigger a product build'
    Assert-True (-not $scope.RequiresConfigure) 'a git hook must not trigger a configure'
}

Test-Case 'a docs-only change compiles nothing' {
    $scope = Get-VerifyScope -ChangedFiles @('docs/product-spec.md', 'README.md', '.workspace/notes.md')
    Assert-True (-not $scope.RequiresBuild) 'documentation must not trigger a build'
}

Test-Case 'a script change runs the script tests and nothing heavier' {
    $scope = Get-VerifyScope -ChangedFiles @('scripts/verify.ps1')
    Assert-True $scope.RequiresScriptTests 'a script change must run the script tests'
    Assert-True (-not $scope.RequiresBuild) 'a script change must not trigger a product build'
}

Test-Case 'an unrecognised file type escalates rather than being ignored' {
    $scope = Get-VerifyScope -ChangedFiles @('libs/recorder_core/src/mystery.zzz')
    Assert-True $scope.RequiresFullTests 'an unknown type must widen, never narrow'
    Assert-True ($scope.EscalationReasons -join ' ') -match 'unrecognised'
}

Test-Case 'an empty change set verifies everything' {
    # A shallow clone or a missing base produces no file list. Reading that as
    # "nothing to do" is the one failure mode that makes -Fast falsely safe.
    $scope = Get-VerifyScope -ChangedFiles @()
    Assert-True $scope.RequiresBuild 'an undetermined change set must not skip the build'
    Assert-True $scope.RequiresFullTests 'an undetermined change set must run everything'
}

Test-Case 'mixed Quick and engine changes widen to the full suite' {
    $scope = Get-VerifyScope -ChangedFiles @('libs/recorder_core/src/muxer.cpp', 'app/quick/ExoSnap/Quick/x.cpp')
    Assert-True $scope.RequiresFullTests 'two disjoint areas must not be narrowed into one filter'
    Assert-Equal '' $scope.TestFilter 'a widened run carries no filter'
}

Write-Host ''
Write-Host 'Fast is not Full'

Test-Case 'Full contains every check Fast can contain' {
    $narrow = Get-VerifyScope -ChangedFiles @('libs/recorder_core/src/muxer.cpp')
    $wide = Get-VerifyScope -ChangedFiles @()
    $fastNames = @((New-VerifyPlan -Mode 'Fast' -Scope $wide).Checks.Name)
    $fullNames = @((New-VerifyPlan -Mode 'Full' -Scope $narrow).Checks.Name)
    foreach ($name in $fastNames) {
        Assert-True ($fullNames -contains $name) "-Full is missing the check '$name' that -Fast can run"
    }
    Assert-True ($fullNames -contains 'clang-tidy-blocking') '-Full must carry the blocking clang-tidy set'
    Assert-True (-not ($fastNames -contains 'clang-tidy-blocking')) '-Fast must not claim the blocking clang-tidy set'
}

Test-Case 'Full ignores the change set entirely' {
    $scope = Get-VerifyScope -ChangedFiles @('README.md')
    $plan = New-VerifyPlan -Mode 'Full' -Scope $scope
    foreach ($check in $plan.Checks) {
        Assert-True $check.Applicable "-Full skipped '$($check.Name)' because of the change set; it must not"
    }
}

Test-Case 'Fast labels itself Fast' {
    $plan = New-VerifyPlan -Mode 'Fast' -Scope (Get-VerifyScope -ChangedFiles @('README.md'))
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor)
    Assert-Equal 'Fast' $run.mode 'a Fast run must never report itself as anything else'
}

Test-Case 'a check is planned at most once' {
    foreach ($mode in @('Fast', 'Full')) {
        $plan = New-VerifyPlan -Mode $mode -Scope (Get-VerifyScope -ChangedFiles @())
        $names = @($plan.Checks.Name)
        Assert-Equal $names.Count (@($names | Sort-Object -Unique).Count) "$mode plans a duplicate check"
    }
}

Write-Host ''
Write-Host 'Fail fast'

Test-Case 'a diff failure stops everything after it' {
    $log = [System.Collections.Generic.List[string]]::new()
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('diff') -Log $log)

    Assert-Equal 'failed' $run.result 'the run must report failure'
    Assert-Equal $S.Fail (Get-CheckStatus $run 'diff').status 'diff must be the failure'
    foreach ($later in @('format', 'configure', 'qmllint', 'build', 'tests', 'cppcheck', 'clang-tidy')) {
        Assert-True ((Get-CheckStatus $run $later).status -ne $S.Pass) "'$later' must not claim PASS after diff failed"
        Assert-True (-not ($log -contains $later)) "'$later' must not even be started after diff failed"
    }
}

Test-Case 'a format failure stops the compile and everything past it' {
    $log = [System.Collections.Generic.List[string]]::new()
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('format') -Log $log)
    Assert-Equal 'failed' $run.result 'the run must report failure'
    Assert-True (-not ($log -contains 'build')) 'no build may start after a format failure'
    # NOT_RUN or SKIPPED_DEPENDENCY, depending on whether the check's own
    # prerequisite was reached first. Both are non-green; PASS is the only
    # answer that would be a lie.
    Assert-True ((Get-CheckStatus $run 'build').status -in @($S.NotRun, $S.SkipDependency)) `
        'the build must be reported as not run or dependency-skipped'
    Assert-True ((Get-CheckStatus $run 'tests').status -ne $S.Pass) 'no test may read as passed'
}

Test-Case 'a source-hygiene failure fails the run' {
    $plan = New-VerifyPlan -Mode 'Fast' -Scope (Get-VerifyScope -ChangedFiles @('app/quick/ExoSnap/Quick/x.cpp'))
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('source-hygiene'))
    Assert-Equal 'failed' $run.result 'provenance in a source comment is a blocking gate'
}

Test-Case 'source-hygiene runs in both modes and before the compile' {
    foreach ($mode in @('Fast', 'Full')) {
        $plan = New-VerifyPlan -Mode $mode -Scope (Get-VerifyScope -ChangedFiles @())
        $names = @($plan.Checks.Name)
        Assert-True ($names -contains 'source-hygiene') "$mode must run source-hygiene"
        Assert-True ([array]::IndexOf($names, 'source-hygiene') -lt [array]::IndexOf($names, 'build')) `
            "$mode must run the cheap text check before the compile"
    }
}

Test-Case 'a drift failure fails the run' {
    $plan = New-VerifyPlan -Mode 'Fast' -Scope (Get-VerifyScope -ChangedFiles @('.github/workflows/ci.yml'))
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('drift'))
    Assert-Equal 'failed' $run.result 'a drift violation must block'
}

Test-Case 'a cppcheck failure fails the run' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('cppcheck'))
    Assert-Equal 'failed' $run.result 'cppcheck is a blocking gate'
}

Test-Case 'a clang-tidy failure fails the run' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('clang-tidy'))
    Assert-Equal 'failed' $run.result 'clang-tidy is a blocking gate'
}

Test-Case 'a script-test failure fails the run' {
    $plan = New-VerifyPlan -Mode 'Fast' -Scope (Get-VerifyScope -ChangedFiles @('scripts/verify.ps1'))
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('script-tests'))
    Assert-Equal 'failed' $run.result 'the script tests are a blocking gate'
}

Write-Host ''
Write-Host 'Build and test are one evidence chain'

Test-Case 'a failed build never leaves a passing test result' {
    $log = [System.Collections.Generic.List[string]]::new()
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('build') -Log $log)

    Assert-Equal $S.Fail (Get-CheckStatus $run 'build').status 'the build must be FAIL'
    Assert-Equal $S.SkipDependency (Get-CheckStatus $run 'tests').status `
        'tests must be SKIPPED_DEPENDENCY behind a failed build, never PASS and never a bare NOT_RUN'
    Assert-True (-not ($log -contains 'tests')) 'no test may be executed against a build that failed'
    Assert-Equal 'failed' $run.result 'the run must be failed'
}

Test-Case 'a failed configure blocks qmllint, build and tests alike' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('configure'))
    Assert-Equal $S.SkipDependency (Get-CheckStatus $run 'qmllint').status 'qmllint needs a configured tree'
    Assert-Equal $S.SkipDependency (Get-CheckStatus $run 'build').status 'the build needs a configured tree'
    Assert-True ((Get-CheckStatus $run 'tests').status -ne $S.Pass) 'tests must not pass behind a failed configure'
}

Test-Case 'clang-tidy depends on the build too' {
    # clang-tidy reads compile_commands.json. Running it against a database that
    # describes source the build rejected is the same stale-evidence mistake.
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('build'))
    Assert-Equal $S.SkipDependency (Get-CheckStatus $run 'clang-tidy').status 'clang-tidy must not run behind a failed build'
    Assert-Equal $S.SkipDependency (Get-CheckStatus $run 'clang-tidy-blocking').status 'the blocking set must not run behind a failed build'
}

Test-Case 'no plan ever lets tests run without a build in scope' {
    foreach ($files in @(
            @('libs/recorder_core/src/muxer.cpp'),
            @('app/quick/ExoSnap/Quick/RecordPage.qml'),
            @('libs/recorder_core/include/recorder_core/session.h'),
            @('CMakeLists.txt'),
            @('scripts/verify.ps1'),
            @('.github/workflows/ci.yml'),
            @())) {
        $scope = Get-VerifyScope -ChangedFiles $files
        $plan = New-VerifyPlan -Mode 'Fast' -Scope $scope
        $tests = @($plan.Checks | Where-Object { $_.Name -eq 'tests' })[0]
        $build = @($plan.Checks | Where-Object { $_.Name -eq 'build' })[0]
        if ($tests.Applicable) {
            Assert-True $build.Applicable "tests are planned but the build is not, for [$($files -join ', ')]"
        }
        Assert-True ($tests.DependsOn -contains 'build') 'tests must always declare the build as a prerequisite'
    }
}

Test-Case 'a skipped-because-out-of-scope prerequisite does not block' {
    # SKIP is "not applicable here", not "went wrong". A workflow-only change
    # skips the build, and nothing downstream should be reported as damaged.
    $scope = Get-VerifyScope -ChangedFiles @('.github/workflows/ci.yml')
    $plan = New-VerifyPlan -Mode 'Fast' -Scope $scope
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor)
    Assert-Equal $S.Skip (Get-CheckStatus $run 'build').status 'the build is out of scope here'
    Assert-Equal $S.Skip (Get-CheckStatus $run 'tests').status 'the tests are out of scope here'
    Assert-Equal 'passed' $run.result 'an out-of-scope skip must not fail the run'
}

Write-Host ''
Write-Host 'QML diagnostics'

Test-Case 'a failing QML test gets a text report, not just an exit code' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $provider = { param($check, $outcome) return @('C:/logs/record_controls_qml_tests.txt') }
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('tests')) -DiagnosticProvider $provider
    $tests = Get-CheckStatus $run 'tests'
    Assert-Equal $S.Fail $tests.status 'the tests must be FAIL'
    Assert-True ($tests.diagnostics.Count -gt 0) 'a QML test failure must carry diagnostic evidence'
}

Test-Case 'a QML failure with no diagnosis says so out loud' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $provider = { param($check, $outcome) return @() }
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('tests')) -DiagnosticProvider $provider
    $tests = Get-CheckStatus $run 'tests'
    Assert-True ($tests.detail -match 'exit code is not a test report') `
        'a failure with no diagnosis must be reported as such, not left looking complete'
}

Test-Case 'the QuickTest re-run asks for a text report' {
    $commands = Resolve-QmlDiagnosticCommand -BuildDir 'build/windows-x64-debug' -LogDirectory 'C:/logs' `
        -FailedTestNames @('quick.qml.record_controls')
    Assert-Equal 1 $commands.Count 'the known QuickTest runner must be re-run'
    Assert-Equal 'record_controls_qml_tests' $commands[0].Executable 'the ctest name must map to its binary'
    Assert-True (($commands[0].Arguments -join ' ') -match '^-o .*,txt$') `
        'the re-run must request a txt report; without it a QuickTest failure is only an exit code'
}

Test-Case 'the failing test names are read through the summary log pointer' {
    # run-tests.ps1 prints a compact summary and keeps the ctest output that
    # names the failures in a separate file. Parsing only the summary found
    # nothing, so a failing QML test produced no diagnosis at all while the
    # pipeline still reported the failure -- honest, and useless.
    $dir = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $full = Join-Path $dir 'last-run.log'
    $summary = Join-Path $dir 'tests.log'
    Set-Content -LiteralPath $full -Encoding utf8 -Value @(
        'The following tests FAILED:',
        "`t219 - quick.qml.record_controls (Failed)                quick")
    Set-Content -LiteralPath $summary -Encoding utf8 -Value @(
        'ctest --test-dir x -C Debug',
        "Full log: $full",
        '88% tests passed, 1 tests failed out of 8')

    $names = Get-FailedCTestName -LogPath $summary
    Assert-Equal 1 $names.Count 'the failure named in the full log must be found through the summary'
    Assert-Equal 'quick.qml.record_controls' $names[0] 'the ctest name must survive the tab and trailing label'
}

Test-Case 'a raw ctest log still parses without a pointer' {
    $path = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n')).log"
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Encoding utf8 -Value @(
        'The following tests FAILED:',
        '  12 - engine.muxer (Timeout)')
    Assert-Equal 'engine.muxer' (Get-FailedCTestName -LogPath $path)[0] 'a direct ctest log must still parse'
}

Test-Case 'an unconfigured build directory yields no registered commands' {
    # The fallback path matters more than the lookup: a tree that was never
    # configured must not make the resolver throw on the way to reporting a
    # failure.
    $paths = Get-CTestCommandPath -BuildDir (Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n'))")
    Assert-Equal 0 $paths.Count 'an unconfigured directory registers nothing'
}

Test-Case 'the re-run targets an executable, however the generator laid it out' {
    $commands = Resolve-QmlDiagnosticCommand -BuildDir 'build/windows-x64-ninja-debug' -LogDirectory 'C:/logs' `
        -FailedTestNames @('quick.qml.record_controls')
    Assert-Equal 1 $commands.Count 'the known QuickTest runner must be re-run'
    Assert-True ($commands[0].FilePath -match '(?i)record_controls_qml_tests\.exe$') `
        "the command must point at the runner binary, not at a directory (got '$($commands[0].FilePath)')"
}

Test-Case 'a non-QuickTest failure is not re-run with QuickTest flags' {
    # exosnap --smoke-test is registered as a ctest test but is not a QuickTest
    # binary, and does not understand -o.
    $commands = Resolve-QmlDiagnosticCommand -BuildDir 'build/x' -LogDirectory 'C:/logs' `
        -FailedTestNames @('quick.qml.about_smoke', 'engine.muxer')
    Assert-Equal 0 $commands.Count 'only real QuickTest runners may be re-run with -o'
}

Write-Host ''
Write-Host 'Summary output'

Test-Case 'the console summary names every check and its status' {
    $plan = New-VerifyPlan -Mode 'Full' -Scope (Get-VerifyScope -ChangedFiles @())
    $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('format'))
    $lines = New-VerifySummary -Run $run
    Assert-Equal $plan.Checks.Count $lines.Count 'one line per check'
    Assert-True (($lines -join "`n") -match 'FAIL\s+format') 'the failing check must be named'
    Assert-True (-not (($lines -join "`n") -match 'PASS\s+build')) 'nothing downstream may read as passed'
}

Test-Case 'the JSON summary records the run truthfully' {
    $path = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n'))/latest.json"
    try {
        $scope = Get-VerifyScope -ChangedFiles @('libs/recorder_core/src/muxer.cpp')
        $plan = New-VerifyPlan -Mode 'Fast' -Scope $scope
        $run = Invoke-VerifyPlan -Plan $plan -Executor (New-FakeExecutor -FailingChecks @('build'))
        Save-VerifyResult -Run $run -Path $path -Head 'abc1234' -Base 'def5678' -Dirty $true -Scope $scope | Out-Null

        $document = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        Assert-Equal 'Fast' $document.mode 'the mode has to be recorded'
        Assert-Equal 'abc1234' $document.head 'the HEAD the run is about has to be recorded'
        Assert-Equal 'def5678' $document.base 'the base has to be recorded'
        Assert-Equal 'failed' $document.result 'the result has to be the truth'
        Assert-True $document.dirty 'an uncommitted tree has to be visible in the evidence'

        $tests = @($document.checks | Where-Object { $_.name -eq 'tests' })[0]
        Assert-Equal 'SKIPPED_DEPENDENCY' $tests.status 'the JSON must not show a green test behind a failed build'
        Assert-True ($tests.dependsOn -contains 'build') 'the JSON has to say what the test result stood on'

        $build = @($document.checks | Where-Object { $_.name -eq 'build' })[0]
        Assert-Equal 'build/windows-x64-ninja-debug' $build.evidence.buildDir 'the build target has to be identifiable'
    }
    finally {
        Remove-Item -LiteralPath (Split-Path -Parent $path) -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ''
Write-Host 'End to end, without a compiler'

Test-Case 'verify.ps1 -Fast exits 0 when every simulated step passes' {
    $result = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n')).json"
    try {
        & pwsh -NoProfile -NonInteractive -File (Join-Path $scriptRoot 'verify.ps1') `
            -Fast -DryRun -ResultPath $result *> $null
        Assert-Equal 0 $LASTEXITCODE 'an all-green run must exit 0'
        $document = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        Assert-Equal 'passed' $document.result 'the summary must agree with the exit code'
    }
    finally { Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue }
}

Test-Case 'verify.ps1 exits non-zero on a simulated build failure and skips the tests' {
    $result = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n')).json"
    try {
        & pwsh -NoProfile -NonInteractive -File (Join-Path $scriptRoot 'verify.ps1') `
            -Full -DryRun -SimulateFail 'build' -ResultPath $result *> $null
        Assert-True ($LASTEXITCODE -ne 0) 'a failed build must exit non-zero'
        $document = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        Assert-Equal 'failed' $document.result 'the summary must say failed'
        $tests = @($document.checks | Where-Object { $_.name -eq 'tests' })[0]
        Assert-Equal 'SKIPPED_DEPENDENCY' $tests.status 'the end-to-end run must not green a test behind a failed build'
    }
    finally { Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue }
}

Test-Case 'verify.ps1 attaches QML evidence when the simulated test step fails' {
    $result = Join-Path ([IO.Path]::GetTempPath()) "verify-tests/$([guid]::NewGuid().ToString('n')).json"
    try {
        & pwsh -NoProfile -NonInteractive -File (Join-Path $scriptRoot 'verify.ps1') `
            -Full -DryRun -SimulateFail 'tests' -ResultPath $result *> $null
        Assert-True ($LASTEXITCODE -ne 0) 'a failed test step must exit non-zero'
        $document = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        $tests = @($document.checks | Where-Object { $_.name -eq 'tests' })[0]
        Assert-True ($tests.diagnostics.Count -gt 0) 'the failing test step must carry diagnostic evidence'
    }
    finally { Remove-Item -LiteralPath $result -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
