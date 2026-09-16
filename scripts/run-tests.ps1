#!/usr/bin/env pwsh
#Requires -Version 7.0
<#
.SYNOPSIS
    The canonical way to run the ExoSnap test suite.

.DESCRIPTION
    Wraps `ctest` with the environment every ExoSnap test binary needs and a
    quiet, summarised output:

      * EXOSNAP_CONFIG_DIR  -> a fresh throwaway temp dir (never touch real
                               user config), removed again on exit.
      * QT_QPA_PLATFORM      -> offscreen (no windows pop up; headless-safe).
      * QT_PLUGIN_PATH       -> the Qt install's plugins dir.
      * PATH                 -> Qt bin prepended so Qt/FFmpeg DLLs resolve.

    The full ctest output (including every failing gtest `Suite.Case`) is written
    to <BuildDir>/Testing/last-run.log. Only a compact summary — pass/fail
    counts, wall-clock, and the failing binaries + their gtest cases — goes to
    stdout. The script's exit code is ctest's exit code.

    Each CTest entry is one test BINARY (gtest_main runs all its cases in-process
    and prints the exact failing case), so -R / -Filter matches binary names,
    e.g. "recorder_core." or "capability.".

.PARAMETER BuildDir
    CMake build tree to test. Default: build/windows-x64-debug.

.PARAMETER Config
    Multi-config configuration to run (ctest -C). Default: Debug.

.PARAMETER Filter
    Regex passed to `ctest -R` to select test binaries by name.

.PARAMETER ExcludeLabel
    Label excluded from the run. Use "live" to skip the binaries that issue real
    hardware queries (DXGI adapter enumeration, GPU capability probes) so the
    suite runs cleanly with no GPU present.

    ctest -LE takes a regular expression, not a label name, so an unanchored
    "live" also matches "live_verify" and silently drops six script suites that
    query no hardware at all. A plain word is therefore anchored to ^word$ before
    it reaches ctest; a value that already contains regex metacharacters is
    passed through unchanged for callers that mean a pattern.

.PARAMETER Jobs
    Parallel test jobs (ctest -j). Default: the processor count.

.PARAMETER Build
    Do a full build of the tree (all targets) before running tests.

.PARAMETER RequireFresh
    Fail instead of warning when the build tree was not built from the source
    currently checked out. Off by default so an inner-loop run stays usable;
    CI and the pre-push gate turn it on.

.EXAMPLE
    pwsh scripts/run-tests.ps1

.EXAMPLE
    pwsh scripts/run-tests.ps1 -Filter recorder_core. -Build

.PARAMETER Phase
    Run only the tests of one execution phase. Every registered test declares
    exactly one, and the guard below refuses a tree where one does not.

      hermetic  Nothing outside the process: no hardware, no network, no desktop.
      cpu       Real CPU work or real time. Deterministic in result, not duration.
      gpu       Needs a graphics adapter. Without one these can only skip.
      desktop   Needs an interactive desktop: a window, focus, a Qt surface.
      vm        Needs a disposable machine of its own.
      human     Needs a person to do something no API can do.

.EXAMPLE
    pwsh scripts/run-tests.ps1 -ExcludeLabel live

.EXAMPLE
    pwsh scripts/run-tests.ps1 -Phase hermetic
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build/windows-x64-debug',
    [string]$Config = 'Debug',
    [string]$Filter = '',
    [string]$ExcludeLabel = '',
    [ValidateSet('', 'hermetic', 'cpu', 'gpu', 'desktop', 'vm', 'human')]
    [string]$Phase = '',
    [int]$Jobs = 0,
    [switch]$Build,
    [switch]$RequireFresh
)

$ErrorActionPreference = 'Stop'

# Resolve paths relative to the repo root (this script lives in scripts/)
# without Set-Location, so an in-session caller keeps its working directory.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

Import-Module (Join-Path $PSScriptRoot 'lib/HostResourceLock.psm1') -Force

# Bounded by the same budget the verify runner uses, never the full core count:
# a test run at -j<cores> beside a build in another worktree is the contention
# the host lock below exists for, and the budget keeps the one that got through
# from taking the machine anyway.
if ($Jobs -le 0) { $Jobs = Get-HostJobBudget }

# --- Helpers -----------------------------------------------------------------

function ConvertTo-CTestLabelPattern {
    <#
    .SYNOPSIS
        Anchor a plain label name so ctest -LE cannot match a longer label.
    #>
    param([Parameter(Mandatory)] [string] $Label)

    # Anything that is not a bare label word is the caller's own regex.
    if ($Label -match '^[A-Za-z0-9_]+$') { return "^$Label`$" }
    return $Label
}

function Get-SourceFingerprint {
    <#
    .SYNOPSIS
        What the tests were built from: HEAD plus every tracked modification.
    .DESCRIPTION
        Timestamps are not usable here -- a restored or copied tree keeps the
        mtimes it was created with, so a stale tree can look newer than its
        source. The fingerprint is content-derived instead, and deliberately
        includes the working-tree diff: testing a dirty tree against a build of
        its clean HEAD is the same stale result with a cleaner alibi.
    #>
    param([Parameter(Mandatory)] [string] $RepoRoot)

    $head = (& git -C $RepoRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $head) { return $null }

    $diff = (& git -C $RepoRoot diff HEAD 2>$null | Out-String)
    $untracked = (& git -C $RepoRoot ls-files --others --exclude-standard 2>$null | Out-String)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes("$head`n$diff`n$untracked")
        $digest = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }

    return [pscustomobject]@{
        head        = $head.Trim()
        dirty       = [bool]($diff.Trim() -ne '')
        fingerprint = $digest
    }
}

# Write-Host, not Write-Error: with ErrorActionPreference=Stop a Write-Error
# throws immediately, which would skip the intended exit code (and further down
# the config-dir cleanup).
if (-not (Test-Path $BuildDir -PathType Container)) {
    Write-Host "Build dir '$BuildDir' does not exist. Configure it first (cmake --preset ...) or pass -BuildDir." -ForegroundColor Red
    exit 2
}

# --- Test environment, restored again on every exit path ---------------------
# The suite mutates process state that outlives it when the script is dot-sourced
# or called from a long-running session: PATH gains the Qt bin directory,
# QT_QPA_PLATFORM forces offscreen, and EXOSNAP_CONFIG_DIR points at a directory
# this script deletes. A caller that kept any of those would run its next command
# against a Qt that is no longer on disk, or write config into a deleted path.
$savedEnvironment = @{}
foreach ($name in @('PATH', 'QT_QPA_PLATFORM', 'QT_PLUGIN_PATH', 'EXOSNAP_CONFIG_DIR')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

$ctestExit = 0
$configDir = $null

try {
    # --- Qt / DLL resolution -------------------------------------------------
    # Resolved from .qt-version, never spelled out here: a hard-coded path keeps
    # working after a Qt uplift, against the previous Qt.
    Import-Module (Join-Path $PSScriptRoot 'lib/QtEnvironment.psm1') -Force
    Add-QtToPath -RepoRoot $repoRoot -IncludePlugins | Out-Null
    $env:QT_QPA_PLATFORM = 'offscreen'

    # --- Isolated, throwaway config dir --------------------------------------
    $configDir = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap_runtests_" + [System.Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    $env:EXOSNAP_CONFIG_DIR = $configDir

    # --- Optional full build -------------------------------------------------
    if ($Build) {
        Write-Host "Building all targets in $BuildDir ($Config)..." -ForegroundColor Cyan
        & cmake --build $BuildDir --config $Config
        if ($LASTEXITCODE -ne 0) {
            $buildExit = $LASTEXITCODE
            Write-Host "Build failed (exit $buildExit)." -ForegroundColor Red
            exit $buildExit
        }
    }

    $logDir = Join-Path $BuildDir 'Testing'
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $logFile = Join-Path $logDir 'last-run.log'

    # --- Is this build tree actually the source in front of us? --------------
    $generator = ''
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $generatorLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
        if ($generatorLine) { $generator = $generatorLine.Matches[0].Groups[1].Value }
    }

    # Ninja is the only generator here that can answer the question cheaply and
    # honestly: it replays its own dependency graph and reports whether anything
    # is out of date. A timestamp comparison would not do -- a copied or restored
    # tree keeps the mtimes it was created with, so a stale build can look newer
    # than the source it is missing.
    $freshness = 'unknown'
    $freshnessDetail = 'generator cannot report staleness without building; pass -Build'
    if ($generator -match 'Ninja') {
        $ninja = ''
        $makeProgramLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_MAKE_PROGRAM:\w+=(.*)$' | Select-Object -First 1
        if ($makeProgramLine) { $ninja = $makeProgramLine.Matches[0].Groups[1].Value }
        if ($ninja -and (Test-Path -LiteralPath $ninja)) {
            $dryRun = (& $ninja -C $BuildDir -n 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                $freshness = 'unknown'
                $freshnessDetail = 'ninja could not evaluate the graph'
            }
            else {
                # CMake's glob verification is a _force target: it is dirty on
                # every build by construction, and the CMake re-run that depends
                # on it is listed with it, whether or not a glob actually
                # changed. Both are configuration housekeeping and say nothing
                # about the binaries. The residual limit: if a glob HAS changed,
                # the compile steps that would follow the re-run are not in this
                # listing yet, so a brand-new source file is invisible here --
                # which is why a gate builds first and only then asks.
                $housekeeping = 'Re-checking globbed directories|Re-running CMake|Entering directory|no work to do'
                $pending = @($dryRun -split "`r?`n" |
                    Where-Object { $_.Trim() -ne '' -and $_ -notmatch $housekeeping })

                if ($pending.Count -eq 0) {
                    $freshness = 'fresh'
                    $freshnessDetail = 'ninja has no build step left that would change these binaries'
                }
                else {
                    $freshness = 'stale'
                    $freshnessDetail = "ninja would run $($pending.Count) build step(s) before these binaries match the source"
                }
            }
        }
    }
    elseif ($Build) {
        $freshness = 'fresh'
        $freshnessDetail = 'built by this invocation'
    }

    if ($freshness -eq 'stale') {
        Write-Host "Build tree is STALE: $freshnessDetail" -ForegroundColor Red
        Write-Host "  $BuildDir" -ForegroundColor Red
        if ($RequireFresh) {
            Write-Host 'Refusing to report a result for binaries that are not the source in front of us. Re-run with -Build.' -ForegroundColor Red
            exit 3
        }
        Write-Host 'Continuing anyway (-RequireFresh would stop here). The result describes the OLD binaries.' -ForegroundColor Yellow
    }
    elseif ($freshness -eq 'unknown' -and $RequireFresh) {
        Write-Host "Cannot prove this build tree matches the source: $freshnessDetail" -ForegroundColor Red
        Write-Host "  $BuildDir" -ForegroundColor Red
        exit 3
    }

    # --- ctest invocation ----------------------------------------------------
    $labelPattern = ''
    if ($ExcludeLabel) { $labelPattern = ConvertTo-CTestLabelPattern -Label $ExcludeLabel }

    $ctestArgs = @(
        '--test-dir', $BuildDir,
        '-C', $Config,
        '-j', "$Jobs",
        '--output-on-failure',
        # A selection that matches nothing is a configuration mistake, never a
        # pass. Without this, a renamed binary or a filter typo reports success
        # over zero tests and every gate downstream believes it.
        '--no-tests=error'
    )
    if ($Filter)       { $ctestArgs += @('-R', $Filter) }
    if ($labelPattern) { $ctestArgs += @('-LE', $labelPattern) }
    if ($Phase)        { $ctestArgs += @('-L', "^phase\.$Phase`$") }

    # The census: how many tests this tree registers at all, independent of what
    # this run selects. A suite that quietly lost half its cases to a missing
    # tool at configure time still passes everything it kept.
    $registeredCount = 0
    $censusLine = (& ctest --test-dir $BuildDir -C $Config -N 2>&1 |
        Select-String -Pattern '^Total Tests:\s*(\d+)' | Select-Object -Last 1)
    if ($censusLine) { $registeredCount = [int]$censusLine.Matches[0].Groups[1].Value }

    # Every registered test declares exactly one execution phase. A tree where one
    # does not is a tree whose selection means nothing: `-Phase hermetic` would
    # silently leave it out, and a CI lane built on that would report a green suite
    # it never ran. Checked against the census above rather than by name, so a new
    # test registered without a phase is caught the first time the suite runs.
    $phaseTotal = 0
    foreach ($known in 'hermetic', 'cpu', 'gpu', 'desktop', 'vm', 'human') {
        $line = (& ctest --test-dir $BuildDir -C $Config -N -L "^phase\.$known`$" 2>&1 |
            Select-String -Pattern '^Total Tests:\s*(\d+)' | Select-Object -Last 1)
        if ($line) { $phaseTotal += [int]$line.Matches[0].Groups[1].Value }
    }
    if ($registeredCount -gt 0 -and $phaseTotal -ne $registeredCount) {
        Write-Host ''
        Write-Host ("FAIL  $($registeredCount - $phaseTotal) of $registeredCount registered test(s) declare no " +
            'execution phase, so a phase selection would silently leave them out.') -ForegroundColor Red
        Write-Host ('      Give each one a PHASE in exosnap_add_gtest, or an exosnap_set_test_phase(...) ' +
            'beside its add_test.') -ForegroundColor Red
        exit 1
    }

    $selectedCount = 0
    $selectArgs = @('--test-dir', $BuildDir, '-C', $Config, '-N')
    if ($Filter)       { $selectArgs += @('-R', $Filter) }
    if ($labelPattern) { $selectArgs += @('-LE', $labelPattern) }
    if ($Phase)        { $selectArgs += @('-L', "^phase\.$Phase`$") }
    $selectedLine = (& ctest @selectArgs 2>&1 |
        Select-String -Pattern '^Total Tests:\s*(\d+)' | Select-Object -Last 1)
    if ($selectedLine) { $selectedCount = [int]$selectedLine.Matches[0].Groups[1].Value }

    $argLine = ($ctestArgs -join ' ')
    Write-Host "ctest $argLine" -ForegroundColor DarkGray
    Write-Host "Build tree: $BuildDir [$generator] freshness=$freshness" -ForegroundColor DarkGray
    Write-Host "Selected $selectedCount of $registeredCount registered tests" -ForegroundColor DarkGray
    Write-Host "Full log: $logFile" -ForegroundColor DarkGray
    Write-Host ''

    # Under the host device lock for the whole run. RESOURCE_LOCK and RUN_SERIAL
    # serialise tests within THIS ctest; a second ctest in another worktree, a
    # live check or a VM campaign shares the GPU and the desktop with it and none
    # of them can see the others. One test run on the host at a time.
    $startedUtc = [DateTime]::UtcNow
    $ctestExit = Invoke-WithHostLock -Kind 'device' -Holder "run-tests $BuildDir" -Body {
        & ctest @ctestArgs 2>&1 | Tee-Object -FilePath $logFile | Out-Null
        $LASTEXITCODE
    }

    # --- Rescue what the tests wrote before the config dir goes --------------
    # The QML and cursor-audit suites write their application logs into
    # EXOSNAP_CONFIG_DIR, and the failure they were run to diagnose is in those
    # logs. Deleting the directory on the way out threw away the only copy.
    $rescuedTo = $null
    if ($ctestExit -ne 0 -and $configDir -and (Test-Path -LiteralPath $configDir)) {
        $leftBehind = Get-ChildItem -LiteralPath $configDir -Recurse -File -ErrorAction SilentlyContinue
        if ($leftBehind) {
            $rescuedTo = Join-Path $logDir 'last-run-config-dir'
            Remove-Item -LiteralPath $rescuedTo -Recurse -Force -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Path $rescuedTo -Force | Out-Null
            Copy-Item -Path (Join-Path $configDir '*') -Destination $rescuedTo -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # --- Summary -------------------------------------------------------------
    $log = Get-Content -LiteralPath $logFile

    # ctest writes two different summary lines: "N% tests passed, M tests failed
    # out of T" when something failed, and "N% tests passed out of T" when
    # nothing did. Matching only the first shape printed no summary at all for a
    # green run -- the case a reader is most likely to take on trust.
    $summaryLine = ($log | Select-String -Pattern '^\s*\d+% tests passed' | Select-Object -Last 1).Line
    $timeLine    = ($log | Select-String -Pattern 'Total Test time' | Select-Object -Last 1).Line

    $passedCount = 0
    $failedCount = 0
    if ($summaryLine -match '(\d+)%\s+tests passed,\s*(\d+)\s+tests failed out of\s*(\d+)') {
        $failedCount = [int]$Matches[2]
        $passedCount = [int]$Matches[3] - $failedCount
    }
    elseif ($summaryLine -match '(\d+)%\s+tests passed out of\s*(\d+)') {
        $failedCount = 0
        $passedCount = [int]$Matches[2]
    }

    # Selected but never accounted for: ctest dropped tests between the listing
    # and the run (a NOT_RUN entry, a missing command). Reporting only the ones
    # that did run would turn that into a pass.
    $censusMismatch = ($passedCount + $failedCount) -ne $selectedCount

    # --- Receipt -------------------------------------------------------------
    # What ran, against which source, in which tree. A test result with no
    # identity cannot be reused as evidence by anything downstream, and a green
    # summary quoted out of an old log is indistinguishable from a fresh one.
    $source = Get-SourceFingerprint -RepoRoot $repoRoot
    $receipt = [ordered]@{
        finished_utc       = [DateTime]::UtcNow.ToString('o')
        started_utc        = $startedUtc.ToString('o')
        build_dir          = $BuildDir
        generator          = $generator
        config             = $Config
        freshness          = $freshness
        freshness_detail   = $freshnessDetail
        ctest_args         = $ctestArgs
        exclude_label      = $ExcludeLabel
        phase              = $Phase
        exclude_pattern    = $labelPattern
        filter             = $Filter
        tests_registered   = $registeredCount
        tests_selected     = $selectedCount
        tests_passed       = $passedCount
        tests_failed       = $failedCount
        census_mismatch    = $censusMismatch
        exit_code          = $ctestExit
        source_head        = $(if ($source) { $source.head } else { $null })
        source_dirty       = $(if ($source) { $source.dirty } else { $null })
        source_fingerprint = $(if ($source) { $source.fingerprint } else { $null })
        log                = $logFile
        rescued_config_dir = $rescuedTo
    }
    $receiptPath = Join-Path $logDir 'last-run-receipt.json'
    $receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding utf8

    Write-Host ('-' * 60)
    if ($summaryLine) { Write-Host $summaryLine.Trim() -ForegroundColor ($ctestExit -eq 0 ? 'Green' : 'Red') }
    if ($timeLine)    { Write-Host $timeLine.Trim() }
    if ($censusMismatch) {
        Write-Host "Census mismatch: $selectedCount tests selected, $($passedCount + $failedCount) accounted for." -ForegroundColor Yellow
    }

    if ($ctestExit -ne 0) {
        # Failed binaries, as ctest lists them under "The following tests FAILED:".
        $failedBinaries = $log |
            Select-String -Pattern '^\s*\d+\s+-\s+(.+?)\s+\(.*(Failed|Timeout).*\)$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }

        if ($failedBinaries) {
            Write-Host ''
            Write-Host 'Failed test binaries:' -ForegroundColor Red
            $failedBinaries | Select-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        }

        # Exact failing gtest cases (gtest_main prints "[  FAILED  ] Suite.Case").
        $failedCases = $log |
            Select-String -Pattern '\[\s*FAILED\s*\]\s+([A-Za-z0-9_./]+\.[A-Za-z0-9_/]+)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }

        if ($failedCases) {
            Write-Host ''
            Write-Host 'Failing gtest cases:' -ForegroundColor Red
            $failedCases | Select-Object -Unique | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }

            # The assertion itself, not only the case name. On CI the log file is an
            # artifact that has to be downloaded first, so a summary that stops at
            # the name sends the reader on a detour for the one line that matters.
            # Each block is what gtest printed between "[ RUN ]" and "[ FAILED ]"
            # for that case, capped so a case that logs a lot cannot bury the rest.
            $maxBlockLines = 40
            foreach ($case in ($failedCases | Select-Object -Unique)) {
                $escaped = [regex]::Escape($case)
                $start = ($log | Select-String -Pattern "\[\s*RUN\s*\]\s+$escaped\s*$" | Select-Object -First 1)
                if (-not $start) { continue }
                $block = New-Object System.Collections.Generic.List[string]
                for ($i = $start.LineNumber; $i -lt $log.Count; $i++) {
                    $line = $log[$i]
                    if ($line -match "\[\s*FAILED\s*\]\s+$escaped") { break }
                    # ctest prefixes every line of a test's output with "<n>: "; strip
                    # it so the assertion reads the way gtest wrote it.
                    $block.Add(($line -replace '^\s*\d+:\s?', ''))
                }
                Write-Host ''
                Write-Host "--- $case" -ForegroundColor Red
                $shown = $block | Where-Object { $_.Trim() -ne '' } | Select-Object -First $maxBlockLines
                $shown | ForEach-Object { Write-Host "  $_" }
                if ($block.Count -gt $maxBlockLines) {
                    Write-Host "  ... ($($block.Count - $maxBlockLines) more lines in the log)" -ForegroundColor DarkGray
                }
            }
        }
        Write-Host ''
        Write-Host "See $logFile for full output." -ForegroundColor Yellow
        if ($rescuedTo) { Write-Host "Test application logs rescued to $rescuedTo" -ForegroundColor Yellow }
    }
    Write-Host "Receipt: $receiptPath" -ForegroundColor DarkGray
    Write-Host ('-' * 60)
}
finally {
    if ($configDir) { Remove-Item -Recurse -Force $configDir -ErrorAction SilentlyContinue }
    foreach ($name in $savedEnvironment.Keys) {
        # A variable that was not set has to be removed, not set to an empty
        # string: an empty EXOSNAP_CONFIG_DIR is a value, and the app reads it
        # as one.
        if ($null -eq $savedEnvironment[$name]) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        else {
            Set-Item "Env:$name" -Value $savedEnvironment[$name]
        }
    }
}

exit $ctestExit
