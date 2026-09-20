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
    to <BuildDir>/Testing/last-run.log. Only a compact summary -- pass/fail
    counts, wall-clock, and the failing binaries + their gtest cases -- goes to
    stdout.

    Each CTest entry is one test BINARY (gtest_main runs all its cases in-process
    and prints the exact failing case), so -R / -Filter matches binary names,
    e.g. "recorder_core." or "capability.".

    WHAT MAKES THE RESULT EVIDENCE. Build, suite and receipt happen under one
    host lock on the build directory (see lib/HostResourceLock.psm1), so no other
    cooperating entry point configures or rebuilds the tree while this run is
    deciding what it holds. The source identity is taken under that lock before
    the build and re-taken before the receipt is published, so a change made to
    the working tree mid-run is reported as drift rather than silently attributed
    to the result. Every run publishes a receipt, including a failing one: a
    broken run must never leave an older successful receipt standing as its
    outcome. `reusable` in that receipt is the single field a downstream consumer
    reads -- it is true only for a run whose source identity, census, phases and
    evidence all held.

    Exit codes:
      0   the selected tests passed and the run is valid
      2   the build directory does not exist
      3   the tree was not proven to match the source (see -AllowStale)
      4   the run is not a valid verification: census, phase or source-identity
          contradiction, or evidence that could not be secured. Raw build and
          ctest exit codes are preserved in the receipt either way.
      *   otherwise the build's or ctest's own exit code

.PARAMETER BuildDir
    CMake build tree to test. Default: build/windows-x64-ninja-debug -- the same
    tree verify.ps1 configures and builds, so the inner loop and the gate judge
    the same binaries. A Visual Studio tree works, but an incremental build in one
    takes minutes rather than seconds.

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

.PARAMETER NoBuild
    Skip the build and test whatever the tree currently holds. The run then has to
    infer whether those binaries match the source, which only a Ninja tree can even
    attempt and which a pending CMake regeneration defeats -- so a skipped build
    usually ends in the refusal below rather than in a result.

.PARAMETER AllowStale
    Report a result even though the build tree was not proven to match the source
    currently checked out. Off by default: an unproven tree is refused with exit 3
    rather than tested, because a pass from binaries that predate the change reads
    exactly like a pass from the change. Only reachable together with -NoBuild.

.EXAMPLE
    pwsh scripts/run-tests.ps1

.EXAMPLE
    pwsh scripts/run-tests.ps1 -Filter recorder_core.

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
    [string]$BuildDir = 'build/windows-x64-ninja-debug',
    [string]$Config = 'Debug',
    [string]$Filter = '',
    [string]$ExcludeLabel = '',
    [ValidateSet('', 'hermetic', 'cpu', 'gpu', 'desktop', 'vm', 'human')]
    [string]$Phase = '',
    [int]$Jobs = 0,
    [switch]$NoBuild,
    [switch]$AllowStale
)

$ErrorActionPreference = 'Stop'

# Resolve paths relative to the repo root (this script lives in scripts/)
# without Set-Location, so an in-session caller keeps its working directory.
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $repoRoot $BuildDir
}

Import-Module (Join-Path $PSScriptRoot 'lib/HostResourceLock.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/SourceFingerprint.psm1') -Force

# Bounded by the same budget the verify runner uses, never the full core count:
# a test run at -j<cores> beside a build in another worktree is the contention
# the host lock below exists for, and the budget keeps the one that got through
# from taking the machine anyway.
if ($Jobs -le 0) { $Jobs = Get-HostJobBudget }

$script:KnownPhases = @('hermetic', 'cpu', 'gpu', 'desktop', 'vm', 'human')

# Exit code for a run that produced no trustworthy verdict: the suite may even
# have passed, but something about the run means its result cannot be read as
# one. Distinct from a test failure on purpose -- and never the only record,
# because the receipt carries the raw build and ctest codes beside it.
$script:ExitInvalidRun = 4

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

function Get-CTestCatalog {
    <#
    .SYNOPSIS
        The tests a tree registers, with their labels -- from ctest's machine
        format, not from the listing written for a human.
    .DESCRIPTION
        `--show-only=json-v1` is a documented contract with names, labels and the
        DISABLED property. The human `-N` output has none of that structure: it
        gives a total and nothing that says WHICH test is missing a phase, so a
        count check built on it can only ever report an arithmetic difference.

        Returns ok = $false with a reason rather than an empty catalog, so a ctest
        that could not list the tree is never read as a tree with no tests.
    #>
    param(
        [Parameter(Mandatory)] [string] $BuildDir,
        [Parameter(Mandatory)] [string] $Config,
        [string[]] $SelectionArgs = @()
    )

    $raw = (& ctest --test-dir $BuildDir -C $Config --show-only=json-v1 @SelectionArgs 2>$null | Out-String)
    if ($LASTEXITCODE -ne 0) {
        return [pscustomobject]@{ ok = $false; reason = "ctest --show-only exited $LASTEXITCODE"; tests = @() }
    }

    try { $parsed = $raw | ConvertFrom-Json }
    catch { return [pscustomobject]@{ ok = $false; reason = "ctest --show-only output is not JSON: $($_.Exception.Message)"; tests = @() } }

    if (@($parsed.PSObject.Properties.Name) -notcontains 'tests') {
        return [pscustomobject]@{ ok = $false; reason = 'ctest --show-only output has no test list'; tests = @() }
    }

    $tests = foreach ($test in @($parsed.tests)) {
        $labels = @()
        $disabled = $false
        foreach ($property in @($test.properties)) {
            if ($property.name -eq 'LABELS') { $labels = @($property.value) }
            elseif ($property.name -eq 'DISABLED') { $disabled = [bool]$property.value }
        }
        [pscustomobject]@{ name = $test.name; labels = $labels; disabled = $disabled }
    }

    return [pscustomobject]@{ ok = $true; reason = $null; tests = @($tests) }
}

function Test-PhaseDeclaration {
    <#
    .SYNOPSIS
        Every registered test declares exactly one known execution phase.
    .DESCRIPTION
        Per test, not as a total. Two totals can agree while one test carries no
        phase and another carries two -- and `-Phase hermetic` would then silently
        leave the first one out, which is how a CI lane reports a green suite it
        never ran. Missing, duplicated and misspelled are diagnosed separately
        because they are three different mistakes in the CMake beside the test.
    #>
    param([Parameter(Mandatory)] [object[]] $Tests)

    $missing = [System.Collections.Generic.List[string]]::new()
    $multiple = [System.Collections.Generic.List[string]]::new()
    $unknown = [System.Collections.Generic.List[string]]::new()

    foreach ($test in $Tests) {
        $phases = @($test.labels | Where-Object { $_ -like 'phase.*' })
        if ($phases.Count -eq 0) { $missing.Add($test.name); continue }
        if ($phases.Count -gt 1) { $multiple.Add("$($test.name) [$($phases -join ', ')]") }
        foreach ($phase in $phases) {
            if ($script:KnownPhases -notcontains $phase.Substring('phase.'.Length)) {
                $unknown.Add("$($test.name) [$phase]")
            }
        }
    }

    return [pscustomobject]@{
        ok       = ($missing.Count + $multiple.Count + $unknown.Count) -eq 0
        missing  = @($missing)
        multiple = @($multiple)
        unknown  = @($unknown)
    }
}

function Write-NameList {
    param([string] $Heading, [string[]] $Names, [int] $Limit = 12)
    if ($Names.Count -eq 0) { return }
    Write-Host "      $Heading" -ForegroundColor Red
    foreach ($name in ($Names | Select-Object -First $Limit)) { Write-Host "        $name" -ForegroundColor Red }
    if ($Names.Count -gt $Limit) {
        Write-Host "        ... and $($Names.Count - $Limit) more" -ForegroundColor Red
    }
}

function Publish-Receipt {
    <#
    .SYNOPSIS
        Write the receipt for THIS run, atomically, replacing any older one.
    .DESCRIPTION
        Written through a temporary file and moved into place, so a reader never
        sees half a receipt, and published on every path that got far enough to
        have a log directory -- a failing run that left the previous successful
        receipt in place would be claiming that run's verdict as its own.

        Returns $true on success. A receipt that could not be written is reported
        by the caller; it is not allowed to pass silently, because everything
        downstream reads this file rather than this run's console.
    #>
    param([Parameter(Mandatory)] [string] $Path, [Parameter(Mandatory)] $Receipt)

    try {
        $directory = Split-Path -Parent $Path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Path $directory -Force -ErrorAction Stop | Out-Null
        }
        $temporary = "$Path.$PID.tmp"
        $Receipt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $temporary -Encoding utf8 -ErrorAction Stop
        Move-Item -LiteralPath $temporary -Destination $Path -Force -ErrorAction Stop
        return $true
    }
    catch {
        Write-Host "Could not publish the run receipt to $Path : $($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
}

function Invoke-RunTestsSeam {
    <#
    .SYNOPSIS
        A named point a test can pause at or fail at. Inert unless one of two
        variables names this point.
    .DESCRIPTION
        Two ordering guarantees of this script cannot be provoked from outside it.
        The evidence rescue has to survive a run that threw instead of returning a
        verdict, which needs the suite to have run and written its logs FIRST and
        the failure to come after. And the tree lock has to still be held while the
        receipt is written, which needs the run observed mid-publish rather than
        after it -- a second process racing the release loses that race on wake-up
        latency alone and reports a pass either way.

        EXOSNAP_RUNTESTS_FAULT=<point>           throws here.
        EXOSNAP_RUNTESTS_PAUSE=<point>=<barrier> blocks here until <barrier> exists,
                                                 after creating "<barrier>.reached"
                                                 so the waiting test never sleeps on
                                                 a guess. Capped, so a test that
                                                 died cannot hang a build.

        Unset in every normal run, including CI. The same kind of seam the updater
        exposes as EXOSNAP_UPDATER_FAULT for the prompts a test cannot answer.
    #>
    param([Parameter(Mandatory)] [string] $Point)

    $pause = $env:EXOSNAP_RUNTESTS_PAUSE
    if ($pause -and $pause.StartsWith("$Point=")) {
        $barrier = $pause.Substring($Point.Length + 1)
        Set-Content -LiteralPath "$barrier.reached" -Value $Point -ErrorAction SilentlyContinue
        $deadline = [DateTime]::UtcNow.AddSeconds(120)
        while (-not (Test-Path -LiteralPath $barrier) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 50
        }
    }

    if ($env:EXOSNAP_RUNTESTS_FAULT -eq $Point) {
        throw "injected fault '$Point' (EXOSNAP_RUNTESTS_FAULT)"
    }
}

function Save-TestEvidence {
    <#
    .SYNOPSIS
        Copy what the suite wrote into EXOSNAP_CONFIG_DIR out before the cleanup deletes it.
    .DESCRIPTION
        The QML and cursor-audit suites write their application logs there, and the
        failure they were run to diagnose is in those logs.

        Called from a finally around the suite, so a run that threw rather than
        returning a verdict is covered too -- the run whose logs are worth the most
        and the one that used to lose them. $Verdict is $null for such a run, and
        that counts as "not evaluated", never as "passed".

        Anything short of a confirmed copy pins the original in place through
        $script:KeepConfigDir, a source directory that cannot be listed included:
        an unreadable directory is not an empty one, and treating it as one is how
        the only copy gets deleted.
    #>
    param([AllowNull()] $Verdict)

    if (-not $configDir -or -not (Test-Path -LiteralPath $configDir)) { return }
    if ($null -ne $Verdict -and $Verdict -eq 0) { return }

    try {
        $leftBehind = @(Get-ChildItem -LiteralPath $configDir -Recurse -File -ErrorAction Stop)
    }
    catch {
        $script:KeepConfigDir = $true
        $script:receipt.rescued_config_dir = $configDir
        $script:receipt.rescue_status = 'failed'
        $script:receipt.rescue_detail = "the source directory could not be listed: $($_.Exception.Message)"
        Write-Host ("Could not list $configDir to secure the test logs: $($_.Exception.Message)") -ForegroundColor Red
        Write-Host "The original directory is kept at $configDir" -ForegroundColor Yellow
        Add-InvalidReason "test evidence could not be secured: $($_.Exception.Message)"
        return
    }

    if ($leftBehind.Count -eq 0) { return }

    # One directory per run. A fixed name meant a second run overwrote the
    # evidence of the first, and two runs finishing at once raced for it.
    $rescuedTo = Join-Path $logDir "last-run-config-dir/$runId"
    try {
        New-Item -ItemType Directory -Path $rescuedTo -Force -ErrorAction Stop | Out-Null
        Copy-Item -Path (Join-Path $configDir '*') -Destination $rescuedTo -Recurse -Force -ErrorAction Stop
        $copied = @(Get-ChildItem -LiteralPath $rescuedTo -Recurse -File -ErrorAction Stop)
        if ($copied.Count -lt $leftBehind.Count) {
            throw "copied $($copied.Count) of $($leftBehind.Count) files"
        }
        $script:receipt.rescued_config_dir = $rescuedTo
        $script:receipt.rescue_status = 'rescued'
        $script:receipt.rescue_detail = "$($copied.Count) file(s)"
    }
    catch {
        # The original is kept: a rescue that failed halfway must not also be the
        # reason the only copy was deleted. The cleanup reads KeepConfigDir.
        $script:KeepConfigDir = $true
        $script:receipt.rescued_config_dir = $configDir
        $script:receipt.rescue_status = 'failed'
        $script:receipt.rescue_detail = $_.Exception.Message
        Write-Host ("Could not secure the test logs from $configDir : $($_.Exception.Message)") -ForegroundColor Red
        Write-Host "The original directory is kept at $configDir" -ForegroundColor Yellow
        Add-InvalidReason "test evidence could not be secured: $($_.Exception.Message)"
    }
}

function Complete-Run {
    <#
    .SYNOPSIS
        Close the receipt off and publish it. Returns the exit code to report.
    .DESCRIPTION
        Runs while the tree lock is still held, and that is the point: released
        first, a second run takes the tree and starts changing it while this one is
        still deciding what its result describes and writing the shared receipt.
        The closing source fingerprint would then be taken against a tree somebody
        else already owns.
    #>
    param([Parameter(Mandatory)] [int] $ExitCode)

    Invoke-RunTestsSeam -Point 'beforeReceipt'

    $script:receipt.finished_utc = [DateTime]::UtcNow.ToString('o')
    $script:receipt.exit_code = $ExitCode

    if ($script:receipt.source_before -and $script:receipt.source_before.ok) {
        $sourceAfter = Get-SourceFingerprint -RepoRoot $repoRoot
        $script:receipt.source_after = $sourceAfter
        if (-not $sourceAfter.ok) {
            $script:receipt.source_drift = $null
            Add-InvalidReason "source identity could not be re-taken: $($sourceAfter.reason)"
            Write-Host "Cannot confirm the source did not move during the run: $($sourceAfter.reason)" -ForegroundColor Red
        }
        else {
            $script:receipt.source_drift = ($sourceAfter.fingerprint -ne $script:receipt.source_before.fingerprint)
            if ($script:receipt.source_drift) {
                Add-InvalidReason 'the working tree changed while the run was in progress'
                Write-Host 'The source changed while this run was in progress; its result describes neither state.' -ForegroundColor Red
            }
        }
    }

    $script:receipt.invalid_reasons = @($script:receipt.invalid_reasons)
    $script:receipt.reusable = ($ExitCode -eq 0 -and $script:receipt.invalid_reasons.Count -eq 0 -and
        $script:receipt.freshness -eq 'fresh' -and $script:receipt.source_drift -eq $false -and
        -not $script:receipt.census_mismatch)

    # A drift or a failed re-take found after the suite ran still has to change the
    # verdict, or the receipt would carry a reason nobody acts on.
    if ($ExitCode -eq 0 -and $script:receipt.invalid_reasons.Count -gt 0) {
        $ExitCode = $script:ExitInvalidRun
        $script:receipt.exit_code = $ExitCode
    }

    if (Test-Path -LiteralPath $logDir -PathType Container) {
        if (-not (Publish-Receipt -Path $receiptPath -Receipt $script:receipt)) {
            # The run may have been fine; what failed is the record of it. Nothing
            # downstream may read this as a pass.
            if ($ExitCode -eq 0) { $ExitCode = $script:ExitInvalidRun }
        }
        else {
            Write-Host "Receipt: $receiptPath" -ForegroundColor DarkGray
        }
        Write-Host ('-' * 60)
    }

    return $ExitCode
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

$runId = [Guid]::NewGuid().ToString('n')
$logDir = Join-Path $BuildDir 'Testing'
$logFile = Join-Path $logDir 'last-run.log'
$receiptPath = Join-Path $logDir 'last-run-receipt.json'
$configDir = $null

# Filled in as the run learns things, published exactly once at the end. Started
# as an invalid run: every field below has to be earned before `reusable` can
# become true.
$receipt = [ordered]@{
    run_id             = $runId
    started_utc        = [DateTime]::UtcNow.ToString('o')
    finished_utc       = $null
    build_dir          = $BuildDir
    generator          = ''
    config             = $Config
    jobs               = $Jobs
    freshness          = 'unknown'
    freshness_detail   = 'the run did not get far enough to judge the tree'
    allow_stale        = [bool]$AllowStale
    no_build           = [bool]$NoBuild
    build_status       = $(if ($NoBuild) { 'skipped' } else { 'not-started' })
    build_exit_code    = $null
    ctest_args         = @()
    exclude_label      = $ExcludeLabel
    exclude_pattern    = ''
    phase              = $Phase
    filter             = $Filter
    tests_registered   = 0
    tests_disabled     = 0
    tests_selected     = 0
    tests_accounted    = 0
    tests_expected     = 0
    tests_passed       = 0
    tests_failed       = 0
    census_mismatch    = $false
    phase_violations   = $null
    ctest_exit_code    = $null
    source_before      = $null
    source_after       = $null
    source_drift       = $null
    log                = $logFile
    rescued_config_dir = $null
    rescue_status      = 'not-needed'
    rescue_detail      = $null
    invalid_reasons    = @()
    exit_code          = $script:ExitInvalidRun
    reusable           = $false
}

function Add-InvalidReason {
    param([Parameter(Mandatory)] [string] $Reason)
    $script:receipt.invalid_reasons = @($script:receipt.invalid_reasons) + $Reason
}

function Invoke-TestRun {
    <#
    .SYNOPSIS
        Build, run and judge, with the build tree locked. Returns the exit code.
    .DESCRIPTION
        One function so there is exactly one place the tree lock is released: the
        caller's finally. Nothing in here exits the process -- an early return is
        an exit code the caller reports after the receipt has been published.
    #>

    New-Item -ItemType Directory -Path $logDir -Force | Out-Null

    # A receipt from an earlier run is not this run's result. Removed before the
    # work starts so a crash between here and the publish leaves no verdict at
    # all, which is honest, rather than the previous one, which is not.
    if (Test-Path -LiteralPath $receiptPath) {
        Remove-Item -LiteralPath $receiptPath -Force -ErrorAction SilentlyContinue
    }

    # --- The source this run is about ----------------------------------------
    # Taken here, under the lock and before the build, so the build and the
    # verdict are about the same source. Re-taken before the receipt is
    # published; see Get-SourceFingerprint for what the pair does and does not
    # prove.
    $sourceBefore = Get-SourceFingerprint -RepoRoot $repoRoot
    $script:receipt.source_before = $sourceBefore
    if (-not $sourceBefore.ok) {
        Write-Host "Cannot identify the source this run is about: $($sourceBefore.reason)" -ForegroundColor Red
        Add-InvalidReason "source identity unavailable: $($sourceBefore.reason)"
        return $script:ExitInvalidRun
    }

    $generator = ''
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $generatorLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
        if ($generatorLine) { $generator = $generatorLine.Matches[0].Groups[1].Value }
    }
    $script:receipt.generator = $generator

    # --- Build, so the run is its own evidence -------------------------------
    # Default on. No dry-run inference can prove a tree current (see below), and
    # the failure it would have to catch is silent: a build that fails leaves the
    # previous binaries in place, and a suite run against those passes and reads
    # exactly like a pass for the change. An incremental no-op costs seconds on a
    # Ninja tree and replaces the inference with the build's own exit code.
    if (-not $NoBuild) {
        # A Ninja tree invokes cl.exe directly and has no Developer Prompt behind it
        # unless the caller provided one. The MSBuild generator brings its own, so it
        # is left alone. Import is a no-op when a compiler already resolves.
        if ($generator -match 'Ninja') {
            Import-Module (Join-Path $PSScriptRoot 'lib/MsvcEnvironment.psm1') -Force
            Enter-MsvcEnvironment -Quiet | Out-Null
        }
        Write-Host "Building all targets in $BuildDir ($Config)..." -ForegroundColor Cyan
        # Inside the tree lock, so the ordering is tree then build: the host build
        # lock bounds compiler processes across worktrees, the tree lock owns this
        # directory. Taken in that order by every entry point, so no two can wait
        # on each other.
        #
        # Out-Host, not the pipeline: this function's output IS its exit code, and
        # a compiler's chatter returned alongside it would make the caller compare
        # an array against zero.
        $buildExit = Invoke-WithHostLock -Kind 'build' -Holder "run-tests $BuildDir" -Body {
            & cmake --build $BuildDir --config $Config | Out-Host
            $LASTEXITCODE
        }
        $script:receipt.build_exit_code = $buildExit
        if ($buildExit -ne 0) {
            $script:receipt.build_status = 'failed'
            Write-Host "Build failed (exit $buildExit)." -ForegroundColor Red
            return $buildExit
        }
        $script:receipt.build_status = 'succeeded'
    }

    New-Item -ItemType Directory -Path $logDir -Force | Out-Null

    # --- Is this build tree actually the source in front of us? --------------
    # A build by this invocation settles the question: its exit code is the
    # evidence, and nothing below can improve on it. The inference only has to
    # carry -NoBuild runs. A timestamp comparison is not among the options -- a
    # copied or restored tree keeps the mtimes it was created with, so a stale
    # build can look newer than the source it is missing.
    $freshness = 'unknown'
    $freshnessDetail = 'this generator cannot report staleness without building; drop -NoBuild'
    if (-not $NoBuild) {
        $freshness = 'fresh'
        $freshnessDetail = 'built by this invocation'
    }
    elseif ($generator -match 'Ninja') {
        $ninja = ''
        $makeProgramLine = Select-String -LiteralPath $cachePath -Pattern '^CMAKE_MAKE_PROGRAM:\w+=(.*)$' | Select-Object -First 1
        if ($makeProgramLine) { $ninja = $makeProgramLine.Matches[0].Groups[1].Value }
        if ($ninja -and (Test-Path -LiteralPath $ninja)) {
            $dryRun = (& $ninja -C $BuildDir -n 2>&1 | Out-String)
            if ($LASTEXITCODE -ne 0) {
                $freshness = 'unknown'
                $freshnessDetail = 'ninja could not evaluate the graph'
            }
            elseif ($dryRun -match 'Re-running CMake') {
                # Everything after a pending regeneration comes out of a build.ninja
                # that the regeneration would rewrite, so ninja lists the two
                # regeneration steps and stops. It is not saying there is no compile
                # work left; it is saying it cannot know yet. CMake's glob check is a
                # _force target, dirty on every invocation by construction, so this
                # is the answer on EVERY run of a CONFIGURE_DEPENDS tree -- verified
                # after a successful build, after touching a compiled source, and
                # after running the regeneration target: byte-identical listings.
                # Reading that as "nothing to do" is how this check reported fresh
                # for binaries a failed build had left behind.
                $freshness = 'unknown'
                $freshnessDetail = 'ninja cannot see past the pending CMake regeneration'
            }
            else {
                $housekeeping = 'Re-checking globbed directories|Entering directory|no work to do'
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
    $script:receipt.freshness = $freshness
    $script:receipt.freshness_detail = $freshnessDetail

    if ($freshness -eq 'stale') {
        Write-Host "Build tree is STALE: $freshnessDetail" -ForegroundColor Red
        Write-Host "  $BuildDir" -ForegroundColor Red
        if (-not $AllowStale) {
            Write-Host 'Refusing to report a result for binaries that are not the source in front of us. Re-run without -NoBuild.' -ForegroundColor Red
            Add-InvalidReason "build tree is stale: $freshnessDetail"
            return 3
        }
        Write-Host 'Continuing anyway (-AllowStale was passed). The result describes the OLD binaries.' -ForegroundColor Yellow
    }
    elseif ($freshness -eq 'unknown') {
        Write-Host "Cannot prove this build tree matches the source: $freshnessDetail" -ForegroundColor Red
        Write-Host "  $BuildDir" -ForegroundColor Red
        if (-not $AllowStale) {
            Add-InvalidReason "build tree freshness unknown: $freshnessDetail"
            return 3
        }
        # Said out loud on the way past, not only at the refusal: an unproven tree
        # that is waved through prints a normal green summary, and the reader has
        # no other signal that it may be the OLD binaries talking.
        Write-Host 'Continuing anyway (-AllowStale was passed). The result may describe OLD binaries.' -ForegroundColor Yellow
    }

    # --- ctest invocation ----------------------------------------------------
    $labelPattern = ''
    if ($ExcludeLabel) { $labelPattern = ConvertTo-CTestLabelPattern -Label $ExcludeLabel }
    $script:receipt.exclude_pattern = $labelPattern

    $selectionArgs = @()
    if ($Filter)       { $selectionArgs += @('-R', $Filter) }
    if ($labelPattern) { $selectionArgs += @('-LE', $labelPattern) }
    if ($Phase)        { $selectionArgs += @('-L', "^phase\.$Phase`$") }

    $ctestArgs = @(
        '--test-dir', $BuildDir,
        '-C', $Config,
        '-j', "$Jobs",
        '--output-on-failure',
        # A selection that matches nothing is a configuration mistake, never a
        # pass. Without this, a renamed binary or a filter typo reports success
        # over zero tests and every gate downstream believes it.
        '--no-tests=error'
    ) + $selectionArgs
    $script:receipt.ctest_args = $ctestArgs

    # The census: what this tree registers at all, independent of what this run
    # selects. A suite that quietly lost half its cases to a missing tool at
    # configure time still passes everything it kept.
    $registered = Get-CTestCatalog -BuildDir $BuildDir -Config $Config
    if (-not $registered.ok) {
        Write-Host "Cannot read the test catalog of $BuildDir : $($registered.reason)" -ForegroundColor Red
        Add-InvalidReason "test catalog unavailable: $($registered.reason)"
        return $script:ExitInvalidRun
    }
    $script:receipt.tests_registered = $registered.tests.Count
    $script:receipt.tests_disabled = @($registered.tests | Where-Object { $_.disabled }).Count

    if ($registered.tests.Count -eq 0) {
        Write-Host "The build tree registers no tests at all: $BuildDir" -ForegroundColor Red
        Add-InvalidReason 'the build tree registers no tests'
        return $script:ExitInvalidRun
    }

    # Every registered test declares exactly one execution phase. A tree where one
    # does not is a tree whose selection means nothing: `-Phase hermetic` would
    # silently leave it out, and a CI lane built on that would report a green suite
    # it never ran. Checked per test rather than by totals, so one test with no
    # phase and another with two cannot cancel out.
    $phases = Test-PhaseDeclaration -Tests $registered.tests
    $script:receipt.phase_violations = [ordered]@{
        missing  = $phases.missing
        multiple = $phases.multiple
        unknown  = $phases.unknown
    }
    if (-not $phases.ok) {
        Write-Host ''
        Write-Host 'FAIL  the execution phases this tree declares do not let a phase selection mean anything:' -ForegroundColor Red
        Write-NameList -Heading "no phase ($($phases.missing.Count)):" -Names $phases.missing
        Write-NameList -Heading "more than one phase ($($phases.multiple.Count)):" -Names $phases.multiple
        Write-NameList -Heading "phase outside the vocabulary ($($phases.unknown.Count)):" -Names $phases.unknown
        Write-Host ('      Give each one exactly one PHASE in exosnap_add_gtest, or an ' +
            'exosnap_set_test_phase(...) beside its add_test.') -ForegroundColor Red
        Add-InvalidReason ("phase declarations invalid: $($phases.missing.Count) missing, " +
            "$($phases.multiple.Count) duplicated, $($phases.unknown.Count) unknown")
        return $script:ExitInvalidRun
    }

    $selected = Get-CTestCatalog -BuildDir $BuildDir -Config $Config -SelectionArgs $selectionArgs
    if (-not $selected.ok) {
        Write-Host "Cannot read the selected tests of $BuildDir : $($selected.reason)" -ForegroundColor Red
        Add-InvalidReason "test selection unavailable: $($selected.reason)"
        return $script:ExitInvalidRun
    }
    $script:receipt.tests_selected = $selected.tests.Count
    # ctest does not count a disabled test in the summary it prints, so the number
    # that has to be accounted for is the selection minus them. Kept as its own
    # field: a test that became disabled is a change in what the suite covers, and
    # a reader should not have to derive that from two other numbers.
    $expected = @($selected.tests | Where-Object { -not $_.disabled }).Count
    $script:receipt.tests_expected = $expected

    if ($expected -eq 0) {
        Write-Host "The selection matches no test that would run (of $($selected.tests.Count) selected)." -ForegroundColor Red
        Add-InvalidReason 'the selection matches no test that would run'
        return $script:ExitInvalidRun
    }

    $argLine = ($ctestArgs -join ' ')
    Write-Host "ctest $argLine" -ForegroundColor DarkGray
    Write-Host "Build tree: $BuildDir [$generator] freshness=$freshness" -ForegroundColor DarkGray
    Write-Host "Selected $($selected.tests.Count) of $($registered.tests.Count) registered tests" -ForegroundColor DarkGray
    Write-Host "Full log: $logFile" -ForegroundColor DarkGray
    Write-Host ''

    # Under the host device lock for the whole run. RESOURCE_LOCK and RUN_SERIAL
    # serialise tests within THIS ctest; a second ctest in another worktree, a
    # live check or a VM campaign shares the GPU and the desktop with it and none
    # of them can see the others. One test run on the host at a time. Taken last
    # in the tree-build-device order, so it can never be the lock someone holds
    # while waiting for this tree.
    #
    # The rescue below runs in a finally, not after the call: recording the output
    # can fail with the suite already run and its logs already written, and that
    # threw straight past a rescue placed on the next line. $ctestExit stays $null
    # on that path, which Save-TestEvidence reads as "no verdict", not as a pass.
    $ctestExit = $null
    try {
        $ctestExit = Invoke-WithHostLock -Kind 'device' -Holder "run-tests $BuildDir" -Body {
            & ctest @ctestArgs 2>&1 | Tee-Object -FilePath $logFile | Out-Null
            $code = $LASTEXITCODE
            Invoke-RunTestsSeam -Point 'afterSuiteBeforeVerdict'
            $code
        }
        $script:receipt.ctest_exit_code = $ctestExit
    }
    finally {
        Save-TestEvidence -Verdict $ctestExit
    }

    # --- Summary -------------------------------------------------------------
    $log = @(Get-Content -LiteralPath $logFile -ErrorAction SilentlyContinue)

    # ctest writes two different summary lines: "N% tests passed, M tests failed
    # out of T" when something failed, and "N% tests passed out of T" when
    # nothing did. Matching only the first shape printed no summary at all for a
    # green run -- the case a reader is most likely to take on trust.
    $summaryLine = ($log | Select-String -Pattern '^\s*\d+% tests passed' | Select-Object -Last 1).Line
    $timeLine    = ($log | Select-String -Pattern 'Total Test time' | Select-Object -Last 1).Line

    $passedCount = 0
    $failedCount = 0
    $summaryParsed = $false
    if ($summaryLine -match '(\d+)%\s+tests passed,\s*(\d+)\s+tests failed out of\s*(\d+)') {
        $failedCount = [int]$Matches[2]
        $passedCount = [int]$Matches[3] - $failedCount
        $summaryParsed = $true
    }
    elseif ($summaryLine -match '(\d+)%\s+tests passed out of\s*(\d+)') {
        $failedCount = 0
        $passedCount = [int]$Matches[2]
        $summaryParsed = $true
    }
    $script:receipt.tests_passed = $passedCount
    $script:receipt.tests_failed = $failedCount
    $script:receipt.tests_accounted = $passedCount + $failedCount

    # Selected but never accounted for: ctest dropped tests between the listing
    # and the run (a NOT_RUN entry, a missing command). Reporting only the ones
    # that did run would turn that into a pass, so the contradiction is the
    # verdict -- fail closed, whatever the suite itself reported.
    $censusMismatch = (-not $summaryParsed) -or (($passedCount + $failedCount) -ne $expected)
    $script:receipt.census_mismatch = $censusMismatch

    Write-Host ('-' * 60)
    if ($summaryLine) { Write-Host $summaryLine.Trim() -ForegroundColor ($ctestExit -eq 0 ? 'Green' : 'Red') }
    if ($timeLine)    { Write-Host $timeLine.Trim() }

    if ($ctestExit -ne 0) {
        # Failed binaries, as ctest lists them under "The following tests FAILED:".
        #
        # The line does not end at the status: ctest appends the test's LABELS
        # after it. A pattern anchored at the closing parenthesis therefore named
        # only tests that carry no label, and every test in this tree declares a
        # phase -- so the summary named no failure at all, and a reader had to
        # open the log, or on CI download an artifact, for the one word the
        # summary exists to print.
        $failedBinaries = $log |
            Select-String -Pattern ('^\s*\d+\s+-\s+(.+?)\s+\((Failed|Timeout|Not Run|Exception[^)]*' +
                '|Subprocess aborted|Child aborted|SEGFAULT|Illegal|Numerical|Other)\)') |
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
        if ($script:receipt.rescue_status -eq 'rescued') {
            Write-Host "Test application logs rescued to $($script:receipt.rescued_config_dir)" -ForegroundColor Yellow
        }
    }

    if ($censusMismatch) {
        Write-Host ''
        if ($summaryParsed) {
            Write-Host ("Census mismatch: $expected test(s) had to be accounted for, " +
                "$($passedCount + $failedCount) were.") -ForegroundColor Red
            Add-InvalidReason "census mismatch: expected $expected accounted tests, got $($passedCount + $failedCount)"
        }
        else {
            Write-Host 'ctest printed no summary this run could read; nothing was accounted for.' -ForegroundColor Red
            Add-InvalidReason 'ctest printed no parseable summary'
        }
        Write-Host 'The suite result cannot be read as a verdict.' -ForegroundColor Red
    }

    # The raw ctest code wins when tests actually failed -- it is the more specific
    # answer and every caller already knows it. The wrapper code only covers a run
    # that produced no usable verdict at all.
    if ($ctestExit -ne 0) { return $ctestExit }
    if (@($script:receipt.invalid_reasons).Count -gt 0) { return $script:ExitInvalidRun }
    return 0
}

$script:KeepConfigDir = $false
$exitCode = $script:ExitInvalidRun
$treeLock = $null

try {
    # --- Qt / DLL resolution -------------------------------------------------
    # Resolved from .qt-version, never spelled out here: a hard-coded path keeps
    # working after a Qt uplift, against the previous Qt.
    Import-Module (Join-Path $PSScriptRoot 'lib/QtEnvironment.psm1') -Force
    Add-QtToPath -RepoRoot $repoRoot -IncludePlugins | Out-Null
    $env:QT_QPA_PLATFORM = 'offscreen'

    # --- Isolated, throwaway config dir --------------------------------------
    $configDir = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap_runtests_" + $runId)
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    $env:EXOSNAP_CONFIG_DIR = $configDir

    # Everything that reads or writes this build tree happens inside this hold:
    # the build, the freshness judgement, the census, the suite, the closing
    # source fingerprint and the receipt. A second cooperating entry point --
    # another run-tests, or verify.ps1's configure, qmllint or build step --
    # waits, so no result can describe binaries a concurrent build replaced
    # underneath it.
    #
    # Entered by hand rather than through Invoke-WithHostLock because the hold
    # has to outlast the body: that helper releases the lock as its own body
    # returns, which left the closing fingerprint and the publish outside it.
    $treeLock = Enter-HostLock -Kind 'tree' -Path $BuildDir -Holder "run-tests $PID"
    if ($treeLock.Waited) {
        Write-Host ("  waited {0:0}s for the tree lock on $BuildDir held by another run" -f `
                $treeLock.WaitedFor.TotalSeconds) -ForegroundColor DarkYellow
    }
    try {
        $exitCode = Invoke-TestRun
    }
    catch {
        Write-Host "The test run did not complete: $($_.Exception.Message)" -ForegroundColor Red
        Add-InvalidReason "run aborted: $($_.Exception.Message)"
        $exitCode = $script:ExitInvalidRun
    }
    finally {
        # --- Publish the receipt, still holding the tree -------------------------
        # On every path that got the lock, success or not. Downstream reads this
        # file, so a run that ended badly has to say so here rather than leave an
        # older, better-looking receipt in place.
        $exitCode = Complete-Run -ExitCode $exitCode
    }
}
catch {
    # Only a failure before or around the hold itself reaches here -- in practice a
    # tree lock another run did not release within the deadline. No receipt is
    # written on this path: the file describes the tree, the tree belongs to the
    # other run, and publishing would replace ITS verdict with this run's inability
    # to start.
    Write-Host "The test run did not start: $($_.Exception.Message)" -ForegroundColor Red
    if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
        Write-Host ("No receipt was written: $receiptPath belongs to the run that holds this tree.") -ForegroundColor Yellow
    }
    $exitCode = $script:ExitInvalidRun
}
finally {
    if ($treeLock) { Exit-HostLock -Lock $treeLock }

    if ($configDir -and -not $script:KeepConfigDir) {
        try { Remove-Item -Recurse -Force $configDir -ErrorAction Stop }
        catch {
            # Not fatal, but not silent either: a config dir that survived is a
            # directory the next reader will find and wonder about.
            Write-Host "Could not remove the throwaway config dir $configDir : $($_.Exception.Message)" -ForegroundColor Yellow
        }
    }

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

exit $exitCode
