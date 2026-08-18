#Requires -Version 7.0
<#
.SYNOPSIS
    Ordering, scoping and truth-keeping for scripts/verify.ps1.

.DESCRIPTION
    This module is the part of local verification that can be tested without a
    compiler: which checks run, in what order, what each one depends on, and what
    a run is allowed to CLAIM afterwards. The commands themselves are injected, so
    the whole contract is falsifiable against fakes.

    The rule the module exists to enforce:

        compiled source changed
              -> the required build succeeds
              -> and only then may tests that consume that build run

    A test result is evidence about the source only if the binaries it ran against
    were built from that source in the same invocation. Three times during the Qt
    6.11 uplift a separately-invoked build and test disagreed about which binaries
    were current -- once reporting green for code that had not compiled, twice
    reporting red for code that had. So a check whose dependency did not pass is
    never PASS here; it is SKIPPED_DEPENDENCY, which is not a green.

.NOTES
    Nothing in this file starts a process. Invoke-VerifyPlan calls whatever
    scriptblock it is handed, and scripts/verify.ps1 is what hands it the real one.
#>

Set-StrictMode -Version Latest

# Statuses. Only PASS is a green; everything else must keep the run from claiming success.
$script:StatusPass           = 'PASS'
$script:StatusFail           = 'FAIL'
$script:StatusSkip           = 'SKIP'                 # out of scope for this run
$script:StatusSkipDependency = 'SKIPPED_DEPENDENCY'   # a prerequisite did not pass
$script:StatusNotRun         = 'NOT_RUN'              # an earlier failure stopped the run

function Get-VerifyStatusName {
    <#
    .SYNOPSIS
        The canonical status strings, so callers and tests never spell them by hand.
    #>
    [OutputType([hashtable])]
    param()
    return [ordered]@{
        Pass           = $script:StatusPass
        Fail           = $script:StatusFail
        Skip           = $script:StatusSkip
        SkipDependency = $script:StatusSkipDependency
        NotRun         = $script:StatusNotRun
    }
}

function Clear-InheritedGitEnvironment {
    <#
    .SYNOPSIS
        Removes the git environment a hook leaks into everything it starts.
    .DESCRIPTION
        A git hook runs its command with GIT_DIR, GIT_INDEX_FILE, GIT_WORK_TREE and
        friends pointing at the repository being committed, and every process it
        starts inherits them. GIT_DIR beats discovery, so `git -C <somewhere else>`
        then still operates on THAT repository.

        This is not theoretical. A script test that builds a throwaway fixture
        repository in the temp directory ran `git init` and `git add -A` against it;
        under a pre-commit hook both landed on the real repository instead, which
        replaced its index with the fixture's and set core.bare = true on the shared
        config -- breaking `git status` in every worktree of the repository,
        including one another session was working in.

        Nothing downstream of verify.ps1 needs the hook's git environment: every git
        call here names its repository with -C.
    #>
    param()
    foreach ($name in @(
            'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY',
            'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_COMMON_DIR', 'GIT_PREFIX',
            'GIT_CEILING_DIRECTORIES', 'GIT_NAMESPACE', 'GIT_QUARANTINE_PATH')) {
        if (Test-Path "Env:$name") { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    }
}

function Get-VerifyChangedFile {
    <#
    .SYNOPSIS
        The files this invocation is responsible for.
    .DESCRIPTION
        Staged mode is the pre-commit question ("what am I about to record?").
        Otherwise the question is the push one ("what does this branch add on top
        of its base?"), plus anything still uncommitted -- a working-tree edit that
        is not committed yet still decides what has to be rebuilt before its tests
        mean anything.
    #>
    [OutputType([string[]])]
    param(
        [Parameter(Mandatory)] [string] $RepoRoot,
        [string] $Base,
        [switch] $Staged,
        [scriptblock] $GitRunner
    )

    $git = if ($GitRunner) { $GitRunner } else {
        { param($GitArgs) & git -C $RepoRoot @GitArgs 2>$null }
    }

    $files = [System.Collections.Generic.List[string]]::new()
    $add = {
        param($lines)
        foreach ($line in @($lines)) {
            if ($line -and $line.Trim()) { $files.Add($line.Trim()) }
        }
    }

    if ($Staged) {
        & $add (& $git @('diff', '--cached', '--name-only', '--diff-filter=ACMR'))
    }
    else {
        if ($Base) {
            & $add (& $git @('diff', '--name-only', '--diff-filter=ACMR', "$Base...HEAD"))
        }
        & $add (& $git @('diff', '--name-only', '--diff-filter=ACMR'))
        & $add (& $git @('diff', '--cached', '--name-only', '--diff-filter=ACMR'))
    }

    return @($files | Sort-Object -Unique)
}

function Get-VerifyScope {
    <#
    .SYNOPSIS
        Turns a changed-file list into what has to be verified.
    .DESCRIPTION
        Deliberately coarse and deliberately one-directional: every rule here may
        only ever widen the scope. Where a dependency cannot be resolved honestly
        -- a header's dependents, an unrecognised file type -- the answer is "build
        and test everything", not "probably nothing". -Fast is allowed to check
        more than it strictly had to. It is never allowed to be falsely safe.
    #>
    [OutputType([hashtable])]
    param(
        [string[]] $ChangedFiles = @()
    )

    $scope = [ordered]@{
        ChangedFiles           = @($ChangedFiles)
        Categories             = @()
        RequiresConfigure      = $false
        RequiresBuild          = $false
        RequiresQmlLint        = $false
        RequiresTests          = $false
        RequiresFullTests      = $false
        TestFilter             = ''
        RequiresScriptTests    = $false
        RequiresStaticAnalysis = $false
        EscalationReasons      = @()
    }

    if (-not $ChangedFiles -or $ChangedFiles.Count -eq 0) {
        # No detectable change set is not the same as "nothing changed" -- a shallow
        # clone, a detached HEAD or a missing base all land here. Assume the worst.
        $scope.Categories             = @('unknown')
        $scope.RequiresConfigure      = $true
        $scope.RequiresBuild          = $true
        $scope.RequiresQmlLint        = $true
        $scope.RequiresTests          = $true
        $scope.RequiresFullTests      = $true
        $scope.RequiresScriptTests    = $true
        $scope.RequiresStaticAnalysis = $true
        $scope.EscalationReasons      = @('no change set could be determined; verifying everything')
        return $scope
    }

    $categories  = [System.Collections.Generic.HashSet[string]]::new()
    $testFilters = [System.Collections.Generic.HashSet[string]]::new()

    # Escalation reasons are aggregated, not listed per file. A branch-wide change
    # set has hundreds of headers in it, and 200 identical sentences bury the one
    # line that is actually informative.
    $reasonCounts = [ordered]@{}
    $addReason = {
        param([string] $Kind, [string] $Example)
        if (-not $reasonCounts.Contains($Kind)) {
            $reasonCounts[$Kind] = [ordered]@{ Count = 0; Example = $Example }
        }
        $reasonCounts[$Kind].Count++
    }

    foreach ($file in $ChangedFiles) {
        $path = $file -replace '\\', '/'

        switch -Regex ($path) {
            '(?i)(^|/)CMakeLists\.txt$|(?i)^CMakePresets\.json$|(?i)\.cmake$|(?i)^cmake/|(?i)\.in$' {
                [void]$categories.Add('cmake')
                $scope.RequiresConfigure      = $true
                $scope.RequiresBuild          = $true
                $scope.RequiresQmlLint        = $true
                $scope.RequiresTests          = $true
                $scope.RequiresFullTests      = $true
                $scope.RequiresStaticAnalysis = $true
                & $addReason 'build infrastructure changed; configure, build and the full test suite are in scope' $path
                continue
            }
            '(?i)\.(h|hpp|hxx|inl)$' {
                [void]$categories.Add('header')
                $scope.RequiresConfigure      = $true
                $scope.RequiresBuild          = $true
                $scope.RequiresTests          = $true
                $scope.RequiresFullTests      = $true
                $scope.RequiresStaticAnalysis = $true
                & $addReason 'a header changed; its dependents are not resolved here, so the whole build and test suite is in scope' $path
                continue
            }
            '(?i)\.(cpp|cc|cxx)$' {
                [void]$categories.Add('cpp')
                $scope.RequiresConfigure      = $true
                $scope.RequiresBuild          = $true
                $scope.RequiresTests          = $true
                $scope.RequiresStaticAnalysis = $true
                # No narrowing by source root. It was tried as app/ -> "^quick\." and
                # libs/ -> "^(?!quick\.)", and the first half is false safety: app/
                # holds dozens of gtest binaries registered under their own prefixes
                # (whats_new_payload., record.error.detail., ...), so a change to one
                # of them ran the Quick UI tests and skipped its own. The suite costs
                # well under a minute; the narrowing saved a fraction of that and
                # could report PASS without having run the tests for the file that
                # changed.
                $scope.RequiresFullTests = $true
                continue
            }
            '(?i)\.(qml|mjs)$' {
                [void]$categories.Add('qml')
                $scope.RequiresConfigure = $true
                $scope.RequiresQmlLint   = $true
                # QML is compiled into the module (qmlcachegen) and staged as a
                # resource, so a QML edit that is not rebuilt is not under test.
                $scope.RequiresBuild     = $true
                $scope.RequiresTests     = $true
                [void]$testFilters.Add('^quick\.')
                continue
            }
            '(?i)^scripts/' {
                [void]$categories.Add('scripts')
                $scope.RequiresScriptTests = $true
                continue
            }
            '(?i)^\.github/|(?i)^\.githooks/|(?i)^\.gitattributes$|(?i)^\.gitignore$|(?i)^\.qt-version$' {
                [void]$categories.Add('workflow')
                continue
            }
            '(?i)^docs/|(?i)^\.workspace/|(?i)^(README|AGENTS|CLAUDE|PRIVACY|CHANGELOG|LICENSE)' {
                [void]$categories.Add('docs')
                continue
            }
            '(?i)\.(md|txt|png|svg|ico|json|toml|ya?ml|rc)$' {
                [void]$categories.Add('data')
                continue
            }
            default {
                [void]$categories.Add('other')
                $scope.RequiresConfigure      = $true
                $scope.RequiresBuild          = $true
                $scope.RequiresTests          = $true
                $scope.RequiresFullTests      = $true
                $scope.RequiresStaticAnalysis = $true
                & $addReason 'unrecognised file type; escalating rather than guessing' $path
            }
        }
    }

    if ($scope.RequiresFullTests) {
        $scope.TestFilter = ''
    }
    elseif ($testFilters.Count -eq 1) {
        $scope.TestFilter = @($testFilters)[0]
    }
    elseif ($testFilters.Count -gt 1) {
        # Two disjoint narrow filters are not worth composing into a regex that has
        # to stay correct; run everything instead.
        $scope.RequiresFullTests = $true
        $scope.TestFilter = ''
        & $addReason 'changes span more than one test area; running the full suite' ''
    }

    $rendered = [System.Collections.Generic.List[string]]::new()
    foreach ($kind in $reasonCounts.Keys) {
        $entry = $reasonCounts[$kind]
        $suffix = if ($entry.Example) { " (x$($entry.Count), e.g. $($entry.Example))" } else { '' }
        $rendered.Add("$kind$suffix")
    }

    $scope.Categories        = @($categories | Sort-Object)
    $scope.EscalationReasons = @($rendered)
    return $scope
}

function New-VerifyCheck {
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Kind,
        [string[]] $DependsOn = @(),
        [switch] $Applicable,
        [string] $SkipReason = '',
        [hashtable] $Evidence = @{},
        [string] $Diagnostics = ''
    )
    return [ordered]@{
        Name        = $Name
        Kind        = $Kind
        DependsOn   = @($DependsOn)
        Applicable  = [bool]$Applicable
        SkipReason  = $SkipReason
        Evidence    = $Evidence
        Diagnostics = $Diagnostics
    }
}

function New-VerifyPlan {
    <#
    .SYNOPSIS
        The ordered check list for a mode, cheapest first.
    .DESCRIPTION
        Order is by cost, so a whitespace mistake never waits behind a compile.

        One deliberate deviation from "lint before build": `configure` precedes
        `qmllint`, because qmllint here is the CMake target `all_qmllint`, not a
        bare qmllint invocation. A hand-rolled qmllint call on this repository
        reports resolution failures the target does not -- it is missing the
        generated import paths -- so the target is the only form whose result means
        anything, and the target needs a configured tree.

        -Full is a superset of -Fast by construction: same builder, scope forced to
        "everything". Nothing may be in -Fast that -Full omits.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [ValidateSet('Fast', 'Full')] [string] $Mode,
        [Parameter(Mandatory)] [hashtable] $Scope,
        [string] $BuildDir = 'build/windows-x64-ninja-debug',
        [string] $Preset = 'windows-x64-ninja-debug',
        [string] $Config = 'Debug'
    )

    $full = ($Mode -eq 'Full')

    $wantConfigure = $full -or $Scope.RequiresConfigure
    $wantQmlLint   = $full -or $Scope.RequiresQmlLint
    $wantBuild     = $full -or $Scope.RequiresBuild
    $wantTests     = $full -or $Scope.RequiresTests
    $wantStatic    = $full -or $Scope.RequiresStaticAnalysis
    $wantScripts   = $full -or $Scope.RequiresScriptTests
    $testFilter    = if ($full) { '' } else { $Scope.TestFilter }

    $checks = [System.Collections.Generic.List[hashtable]]::new()

    $checks.Add((New-VerifyCheck -Name 'sanity' -Kind 'sanity' -Applicable))
    $checks.Add((New-VerifyCheck -Name 'diff' -Kind 'diff' -DependsOn @('sanity') -Applicable))
    $checks.Add((New-VerifyCheck -Name 'drift' -Kind 'drift' -DependsOn @('sanity') -Applicable))
    $checks.Add((New-VerifyCheck -Name 'source-hygiene' -Kind 'source-hygiene' -DependsOn @('sanity') -Applicable))
    $checks.Add((New-VerifyCheck -Name 'format' -Kind 'format' -DependsOn @('sanity') -Applicable))

    $checks.Add((New-VerifyCheck -Name 'script-tests' -Kind 'script-tests' -DependsOn @('sanity') `
                -Applicable:$wantScripts -SkipReason 'no script under scripts/ changed'))

    $checks.Add((New-VerifyCheck -Name 'configure' -Kind 'configure' -DependsOn @('sanity') `
                -Applicable:$wantConfigure -SkipReason 'nothing that reaches the build system changed' `
                -Evidence @{ preset = $Preset; buildDir = $BuildDir }))

    $checks.Add((New-VerifyCheck -Name 'qmllint' -Kind 'qmllint' -DependsOn @('configure') `
                -Applicable:$wantQmlLint -SkipReason 'no QML changed' `
                -Evidence @{ target = 'all_qmllint'; buildDir = $BuildDir }))

    $checks.Add((New-VerifyCheck -Name 'build' -Kind 'build' -DependsOn @('configure') `
                -Applicable:$wantBuild -SkipReason 'no compiled source changed' `
                -Evidence @{ preset = $Preset; buildDir = $BuildDir; config = $Config }))

    # The whole point of the module: tests depend on the build, so a build that did
    # not pass can never leave a green test result behind it.
    $checks.Add((New-VerifyCheck -Name 'tests' -Kind 'tests' -DependsOn @('build') `
                -Applicable:$wantTests -SkipReason 'no compiled source or QML changed' -Diagnostics 'qml' `
                -Evidence @{ buildDir = $BuildDir; config = $Config; filter = $testFilter }))

    $checks.Add((New-VerifyCheck -Name 'cppcheck' -Kind 'cppcheck' -DependsOn @('sanity') `
                -Applicable:$wantStatic -SkipReason 'no C++ changed'))

    # clang-tidy reads compile_commands.json, which the configure produces and the
    # build keeps in step with the source it describes. This is the change-scoped
    # form of the blocking check set; -Full adds the whole-tree form below. The
    # overlap is deliberate: -Full is defined as a superset of -Fast, and the
    # curated set is cheap enough that dropping a step to save the overlap would
    # cost more in explanation than it saves in time.
    $checks.Add((New-VerifyCheck -Name 'clang-tidy' -Kind 'clang-tidy' -DependsOn @('build') `
                -Applicable:$wantStatic -SkipReason 'no C++ changed' `
                -Evidence @{ buildDir = $BuildDir }))

    if ($full) {
        $checks.Add((New-VerifyCheck -Name 'clang-tidy-blocking' -Kind 'clang-tidy-blocking' -DependsOn @('build') `
                    -Applicable -Evidence @{ buildDir = $BuildDir }))
    }

    return [ordered]@{
        Mode     = $Mode
        BuildDir = $BuildDir
        Preset   = $Preset
        Config   = $Config
        Scope    = $Scope
        Checks   = @($checks)
    }
}

function Invoke-VerifyPlan {
    <#
    .SYNOPSIS
        Runs a plan against an injected executor and reports what actually happened.
    .PARAMETER Executor
        Called as & $Executor $check $context. Must return a hashtable with at
        least Status (PASS/FAIL) and may add Detail, Evidence, FailedTests.
    .PARAMETER DiagnosticProvider
        Called as & $DiagnosticProvider $check $outcome when a check that declares a
        Diagnostics kind FAILS. Returns evidence paths. A QML test failure that
        produced no readable diagnosis is itself worth saying out loud -- an exit
        code is not a test report.
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [hashtable] $Plan,
        [Parameter(Mandatory)] [scriptblock] $Executor,
        [scriptblock] $DiagnosticProvider,
        [hashtable] $Context = @{}
    )

    $results  = [System.Collections.Generic.List[hashtable]]::new()
    $status   = @{}
    $failed   = $false
    $failedBy = $null

    foreach ($check in $Plan.Checks) {
        $entry = [ordered]@{
            name        = $check.Name
            status      = $null
            detail      = ''
            dependsOn   = @($check.DependsOn)
            evidence    = $check.Evidence
            diagnostics = @()
        }

        $blocking = @($check.DependsOn | Where-Object {
                $status.ContainsKey($_) -and $status[$_] -notin @($script:StatusPass, $script:StatusSkip)
            })

        if (-not $check.Applicable) {
            $entry.status = $script:StatusSkip
            $entry.detail = $check.SkipReason
        }
        elseif ($blocking.Count -gt 0) {
            $entry.status = $script:StatusSkipDependency
            $entry.detail = "depends on $($blocking -join ', '), which did not pass"
        }
        elseif ($failed) {
            $entry.status = $script:StatusNotRun
            $entry.detail = "stopped after '$failedBy' failed"
        }
        else {
            $outcome = & $Executor $check $Context
            if (-not $outcome -or -not $outcome.ContainsKey('Status')) {
                throw "The executor returned no Status for check '$($check.Name)'."
            }
            $entry.status = $outcome.Status
            if ($outcome.ContainsKey('Detail')) { $entry.detail = [string]$outcome.Detail }
            if ($outcome.ContainsKey('Evidence') -and $outcome.Evidence) {
                foreach ($key in $outcome.Evidence.Keys) { $entry.evidence[$key] = $outcome.Evidence[$key] }
            }

            if ($entry.status -eq $script:StatusFail) {
                if ($check.Diagnostics -and $DiagnosticProvider) {
                    $entry.diagnostics = @(& $DiagnosticProvider $check $outcome)
                }
                if ($check.Diagnostics -and $entry.diagnostics.Count -eq 0) {
                    $entry.detail = ("$($entry.detail) (no $($check.Diagnostics) diagnostics were produced; " +
                        'an exit code is not a test report)').Trim()
                }
                $failed = $true
                $failedBy = $check.Name
            }
        }

        $status[$check.Name] = $entry.status
        $results.Add($entry)
    }

    return [ordered]@{
        mode   = $Plan.Mode
        result = $(if ($failed) { 'failed' } else { 'passed' })
        checks = @($results)
    }
}

function Get-FailedCTestName {
    <#
    .SYNOPSIS
        The ctest names that failed, from a run-tests.ps1 or a raw ctest log.
    .DESCRIPTION
        run-tests.ps1 writes a compact summary and keeps the full ctest output in
        a separate file it names on a "Full log:" line. Only the full output
        carries the "The following tests FAILED:" block, so a parser pointed at
        the summary finds no names and the caller silently loses every diagnosis
        that depends on knowing which test failed. The pointer is followed when it
        is there; a raw ctest log parses directly.
    #>
    [OutputType([string[]])]
    param([string] $LogPath)

    if (-not $LogPath -or -not (Test-Path -LiteralPath $LogPath -PathType Leaf)) { return , @() }

    $lines = @(Get-Content -LiteralPath $LogPath)
    foreach ($line in $lines) {
        if ($line -match '^\s*Full log:\s*(.+?)\s*$') {
            $full = $Matches[1]
            if ((Test-Path -LiteralPath $full -PathType Leaf) -and $full -ne $LogPath) {
                $lines += @(Get-Content -LiteralPath $full)
            }
            break
        }
    }

    # ctest's failure summary lines look like:  "  12 - quick.qml.record_controls (Failed)"
    # Comma operator: a single name would otherwise be unrolled to a bare string,
    # and the caller's .Count fails under Set-StrictMode.
    return , @($lines | ForEach-Object {
            if ($_ -match '^\s*\d+\s+-\s+(\S+)\s+\((Failed|Timeout|Subprocess aborted)') { $Matches[1] }
        } | Sort-Object -Unique)
}

function Get-CTestCommandPath {
    <#
    .SYNOPSIS
        Test name to the executable CTest runs for it, from a configured tree.
    .DESCRIPTION
        Empty when the tree is not configured or CTest is unavailable: the caller
        falls back to a composed path, and the worst case stays "no diagnosis",
        never "ran the wrong binary".
    #>
    [OutputType([hashtable])]
    param(
        [Parameter(Mandatory)] [string] $BuildDir,
        [string] $Config = 'Debug'
    )

    $paths = @{}
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CTestTestfile.cmake') -PathType Leaf)) {
        return $paths
    }

    try {
        $raw = & ctest --test-dir $BuildDir -C $Config --show-only=json-v1 2>$null | Out-String
        if (-not $raw.Trim()) { return $paths }
        foreach ($test in (ConvertFrom-Json $raw).tests) {
            if ($test.command -and $test.command.Count -gt 0) { $paths[$test.name] = $test.command[0] }
        }
    }
    catch { return @{} }
    return $paths
}

function Resolve-QmlDiagnosticCommand {
    <#
    .SYNOPSIS
        How to make a failing QML test say what actually failed.
    .DESCRIPTION
        A QuickTest binary that fails can return nothing but a non-zero exit code,
        which says a test failed without saying which one or why. `-o <file>,txt`
        makes it write the per-function report the diagnosis actually needs. Real
        debugging on this repository needed exactly that, so the pipeline asks for
        it automatically instead of leaving it to be rediscovered.

        Only real QuickTest runners take -o. The `exosnap --*-test` entry points
        registered alongside them are ordinary executables and are left alone.

        The binary is located by asking CTest where it registered the test, not by
        composing a path. Where the runners land differs by generator -- the Visual
        Studio generator writes a per-configuration subdirectory, Ninja does not --
        and a composed path that misses only makes the diagnosis quietly absent,
        which is the one outcome this function exists to prevent.
    #>
    [OutputType([object[]])]
    param(
        [Parameter(Mandatory)] [string] $BuildDir,
        [Parameter(Mandatory)] [string] $LogDirectory,
        [string[]] $FailedTestNames = @(),
        [string] $Config = 'Debug',
        [hashtable] $TestExecutables = @{}
    )

    if ($TestExecutables.Count -eq 0) {
        $TestExecutables = @{
            'quick.qml.record_controls' = 'record_controls_qml_tests'
            'quick.qml.edit_timeline'   = 'edit_timeline_qml_tests'
        }
    }

    $registered = Get-CTestCommandPath -BuildDir $BuildDir -Config $Config

    $commands = [System.Collections.Generic.List[object]]::new()
    foreach ($name in @($FailedTestNames | Sort-Object -Unique)) {
        if (-not $TestExecutables.ContainsKey($name)) { continue }
        $exe = $TestExecutables[$name]
        $output = Join-Path $LogDirectory "$exe.txt"
        $filePath = if ($registered.ContainsKey($name)) {
            $registered[$name]
        }
        else {
            Join-Path (Join-Path $BuildDir $Config) "$exe.exe"
        }
        # A [pscustomobject], not a hashtable: @(...) around a single dictionary
        # enumerates its ENTRIES, so a one-command result would arrive at the
        # caller as five DictionaryEntry objects instead of one command.
        $commands.Add([pscustomobject]@{
                TestName   = $name
                Executable = $exe
                FilePath   = $filePath
                Arguments  = @('-o', "$output,txt")
                OutputPath = $output
            })
    }
    # Comma operator: without it an empty result returns nothing rather than an
    # empty array, and the caller's .Count fails under Set-StrictMode.
    return , @($commands)
}

function New-VerifySummary {
    <#
    .SYNOPSIS
        The console lines, one per check, status column padded so they line up.
    #>
    [OutputType([string[]])]
    param([Parameter(Mandatory)] [hashtable] $Run)

    $width = 0
    foreach ($check in $Run.checks) {
        if ($check.status.Length -gt $width) { $width = $check.status.Length }
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($check in $Run.checks) {
        $line = '{0}  {1}' -f $check.status.PadRight($width), $check.name
        if ($check.detail) { $line = "$line  -- $($check.detail)" }
        $lines.Add($line)
    }
    return @($lines)
}

function Save-VerifyResult {
    <#
    .SYNOPSIS
        Writes the small machine-readable summary.
    .DESCRIPTION
        Deliberately small. Its job is not to be an artifact database; it is to
        record which HEAD the run is about, whether the tree was clean, and which
        build a test result is standing on -- enough that a later reader cannot
        mistake an old green for a statement about new source.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [hashtable] $Run,
        [Parameter(Mandatory)] [string] $Path,
        [string] $Head = '',
        [string] $Base = '',
        [bool] $Dirty = $false,
        [hashtable] $Scope = @{}
    )

    # Assigned in statement form, not as `$x = if (...) { @() }`: an if-expression
    # whose branch yields an empty array yields nothing at all, and under
    # Set-StrictMode the .Count below then fails on $null.
    $categories = @()
    $changed    = @()
    $reasons    = @()
    if ($Scope.Contains('Categories')) { $categories = @($Scope.Categories) }
    if ($Scope.Contains('ChangedFiles')) { $changed = @($Scope.ChangedFiles) }
    if ($Scope.Contains('EscalationReasons')) { $reasons = @($Scope.EscalationReasons) }

    $document = [ordered]@{
        mode   = $Run.mode
        head   = $Head
        base   = $Base
        dirty  = $Dirty
        result = $Run.result
        scope  = [ordered]@{
            categories        = $categories
            changedFileCount  = $changed.Count
            escalationReasons = $reasons
        }
        checks = @($Run.checks)
    }

    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    Set-Content -LiteralPath $Path -Value ($document | ConvertTo-Json -Depth 8) -Encoding utf8
    return $Path
}

Export-ModuleMember -Function @(
    'Get-VerifyStatusName',
    'Clear-InheritedGitEnvironment',
    'Get-VerifyChangedFile',
    'Get-VerifyScope',
    'New-VerifyCheck',
    'New-VerifyPlan',
    'Invoke-VerifyPlan',
    'Resolve-QmlDiagnosticCommand',
    'Get-CTestCommandPath',
    'Get-FailedCTestName',
    'New-VerifySummary',
    'Save-VerifyResult'
)
