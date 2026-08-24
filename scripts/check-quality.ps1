param(
    [int]$FailureTailLines = 160,
    [switch]$VerboseOutput,
    [switch]$StaticOnly,
    # Which static pass to run. 'all' is the historical behaviour and stays the
    # default. The single-tool values exist so scripts/verify.ps1 can report
    # cppcheck and clang-tidy as separate checks -- one line per tool, so a
    # failure names the tool that failed -- without a second copy of the
    # invocation living over there. Implies -StaticOnly.
    [ValidateSet('all', 'cppcheck', 'clang-tidy')]
    [string]$Only = 'all',
    # The configured tree clang-tidy reads compile_commands.json from. Passing it
    # makes a missing database an error instead of a silent skip: a caller that
    # names a tree has already built it, and "clang-tidy passed" must not be the
    # report for a run that never started. Left empty, the first tree that has a
    # database is used and the skip is stated out loud.
    [string]$BuildDir = '',
    # Restrict clang-tidy to the C++ touched since this revision. The broad check
    # set this pass runs is advisory (advisory-checks.yml owns the full-tree form
    # in CI), and a whole-tree run measured over ten minutes here even at one job
    # per core, which is not a price a pre-commit or pre-push hook can pay. The
    # BLOCKING check set is a different script and stays whole-tree.
    [string]$Base = ''
)

if ($Only -ne 'all') { $StaticOnly = $true }

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Write-CommandFailure {
    param(
        [string]$Name,
        [string]$LogPath,
        [int]$TailLines
    )

    Write-Host "${Name}: FAILED"
    if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
        Write-Host "---- $Name output (last $TailLines lines) ----"
        Get-Content -LiteralPath $LogPath -Tail $TailLines | ForEach-Object { Write-Host $_ }
        Write-Host "Full log: $LogPath"
    }
}

function Invoke-QuietNative {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = $repoRoot
    )

    $safeName = ($Name -replace '[^A-Za-z0-9_.-]', '-').ToLowerInvariant()
    $logPath = Join-Path ([System.IO.Path]::GetTempPath()) "exosnap-$safeName-$PID.log"
    Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue

    if ($VerboseOutput) {
        Write-Host ""
        Write-Host "=== $Name ==="
        Push-Location $WorkingDirectory
        try {
            & $FilePath @Arguments
            if ($LASTEXITCODE -ne 0) {
                throw "$Name failed with exit code $LASTEXITCODE."
            }
            Write-Host "${Name}: OK"
        }
        finally {
            Pop-Location
        }
        return
    }

    # Run quietly (full output is captured to $logPath for failure reporting) but emit a
    # lightweight heartbeat so long-running steps (configure/build/test) visibly progress
    # instead of looking hung. Output stays minimal: the step name, an elapsed-seconds
    # marker roughly every 15 s, then the total duration.
    Write-Host "$Name..." -NoNewline
    $errPath = "$logPath.err"
    Remove-Item -LiteralPath $errPath -Force -ErrorAction SilentlyContinue
    $started = Get-Date

    $startArgs = @{
        FilePath               = $FilePath
        WorkingDirectory       = $WorkingDirectory
        NoNewWindow            = $true
        PassThru               = $true
        RedirectStandardOutput = $logPath
        RedirectStandardError  = $errPath
    }
    if ($Arguments -and $Arguments.Count -gt 0) {
        $startArgs.ArgumentList = $Arguments
    }

    $proc = $null
    try {
        $proc = Start-Process @startArgs
    }
    catch {
        Write-Host ""
        throw "$Name failed to start: $_"
    }

    $nextBeat = 10
    while (-not $proc.HasExited) {
        Start-Sleep -Milliseconds 1000
        $elapsed = ((Get-Date) - $started).TotalSeconds
        if ($elapsed -ge $nextBeat) {
            Write-Host (" {0}s" -f [int]$elapsed) -NoNewline
            $nextBeat += 15
        }
    }
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode

    # Fold captured stderr into the step log so failure tails include it.
    if (Test-Path -LiteralPath $errPath -PathType Leaf) {
        Get-Content -LiteralPath $errPath -ErrorAction SilentlyContinue | Add-Content -LiteralPath $logPath
        Remove-Item -LiteralPath $errPath -Force -ErrorAction SilentlyContinue
    }

    $totalSeconds = [int]((Get-Date) - $started).TotalSeconds
    if ($exitCode -ne 0) {
        Write-Host ""
        Write-CommandFailure -Name $Name -LogPath $logPath -TailLines $FailureTailLines
        throw "$Name failed with exit code $exitCode."
    }

    Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue
    Write-Host " OK (${totalSeconds}s)"
}

function Find-Tool {
    param([string]$Name, [switch]$Optional)
    function Test-AppExecutionAlias {
        param([string]$Path)
        return $Path -and $Path.StartsWith((Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'), [StringComparison]::OrdinalIgnoreCase)
    }

    function Test-ToolCandidate {
        param([string]$Path)
        if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            return $false
        }
        if (Test-AppExecutionAlias $Path) {
            return $false
        }
        try {
            & $Path --version *> $null
            return $LASTEXITCODE -eq 0
        }
        catch {
            return $false
        }
    }

    $vsLlvm = Get-ChildItem -LiteralPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" `
        -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Llvm\\x64\\bin\\' } |
        Where-Object { Test-ToolCandidate $_.FullName } |
        Select-Object -First 1
    if ($vsLlvm) { return $vsLlvm.FullName }
    $llvm = Get-ChildItem -LiteralPath "${env:ProgramFiles}\LLVM" `
        -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Where-Object { Test-ToolCandidate $_.FullName } |
        Select-Object -First 1
    if ($llvm) { return $llvm.FullName }
    $cppcheckDir = Get-ChildItem -LiteralPath "${env:ProgramFiles}\Cppcheck" `
        -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Where-Object { Test-ToolCandidate $_.FullName } |
        Select-Object -First 1
    if ($cppcheckDir) { return $cppcheckDir.FullName }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and (Test-ToolCandidate $cmd.Source)) { return $cmd.Source }
    if ($Optional) { return $null }
    throw "$Name.exe not found on PATH, VS LLVM, LLVM, or Cppcheck install."
}

$clangTidy = Find-Tool 'clang-tidy' -Optional
$cppcheck  = Find-Tool 'cppcheck' -Optional

$srcFiles = @(git -C $repoRoot ls-files -- 'libs/' 'app/' 'tests/' |
    Where-Object { $_ -match '\.(cpp|h)$' }
)
$srcScope = 'every tracked source'

if ($Base) {
    $touched = @(
        @(git -C $repoRoot diff --name-only "$Base...HEAD") +
        @(git -C $repoRoot diff --name-only) +
        @(git -C $repoRoot diff --cached --name-only)
    ) | Where-Object { $_ } | Sort-Object -Unique
    # Intersect rather than filter the diff directly: a deleted file is in the
    # diff and cannot be analysed, and a path outside the scanned roots is not
    # this pass's business.
    $srcFiles = @($srcFiles | Where-Object { $touched -contains $_ })
    $srcScope = "changed since $Base"
}

# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------

# Only the Ninja presets export a compile database; the Visual Studio generator
# does not, which is why the hardcoded path this replaces never resolved and the
# clang-tidy step reported success without running for as long as it existed.
$compDbTree = $BuildDir
if (-not $compDbTree) {
    foreach ($candidate in @('build/windows-x64-ninja-debug', 'build/windows-x64-ninja-release', 'build/windows-x64-debug')) {
        if (Test-Path -Path (Join-Path $repoRoot "$candidate/compile_commands.json") -PathType Leaf) {
            $compDbTree = $candidate
            break
        }
    }
}
$compDb = if ($compDbTree) { Join-Path $repoRoot "$compDbTree/compile_commands.json" } else { '' }

if ($Only -eq 'cppcheck') {
    if ($VerboseOutput) { Write-Host "clang-tidy: SKIP (-Only cppcheck)" }
}
elseif ($BuildDir -and -not (Test-Path -Path $compDb -PathType Leaf)) {
    throw "clang-tidy: '$BuildDir' has no compile_commands.json. Configure it with a Ninja preset before asking for this check."
}
elseif ($compDb -and (Test-Path -Path $compDb -PathType Leaf)) {
    if (-not $clangTidy) {
        throw "clang-tidy.exe not found on PATH, VS LLVM, or LLVM install."
    }

    if ($srcFiles) {
        # -clang-analyzer-*: .clang-tidy enables a few path-sensitive analyser
        # checks for the blocking gate, and the analyser turns this pass into a
        # multi-hour one. scripts/run-clang-tidy-blocking.ps1 runs those checks.
        #
        # Two reasons this is not one invocation. A single command line carrying
        # every tracked source exceeds the length limit ("Der Dateiname oder die
        # Erweiterung ist zu lang"), which check-format.ps1 already batches
        # around; and clang-tidy is single-threaded, so a serial whole-tree pass
        # over this repository runs for the better part of an hour, nearly all of
        # it re-parsing Qt headers. Batches are independent, so they run
        # concurrently the way run-clang-tidy-blocking.ps1 runs its units.
        $batchSize = 25
        $batches = [System.Collections.Generic.List[object]]::new()
        for ($i = 0; $i -lt $srcFiles.Count; $i += $batchSize) {
            $batches.Add(@($srcFiles[$i..([Math]::Min($i + $batchSize - 1, $srcFiles.Count - 1))]))
        }

        $jobs = [Math]::Max(1, [Environment]::ProcessorCount)
        Write-Host ("clang-tidy... {0} file(s) ({1}) in {2} batch(es), {3} parallel job(s)" -f `
                $srcFiles.Count, $srcScope, $batches.Count, $jobs) -NoNewline
        $started = Get-Date

        # An absolute -p and an absolute working directory: the parallel runspaces
        # do not inherit this script's location, and a relative build directory
        # resolves against whatever the caller's happened to be.
        $compDbAbsolute = (Resolve-Path -LiteralPath (Join-Path $repoRoot $compDbTree)).Path
        $failures = $batches | ForEach-Object -ThrottleLimit $jobs -Parallel {
            $arguments = @('-p', $using:compDbAbsolute, '--checks=-clang-analyzer-*') + $_
            Set-Location $using:repoRoot
            $output = & $using:clangTidy @arguments 2>&1
            if ($LASTEXITCODE -ne 0) { ($output | Out-String) }
        }

        Write-Host (" [{0}s]" -f [int]((Get-Date) - $started).TotalSeconds)
        if ($failures) {
            # Reported, never thrown. The broad set in .clang-tidy is advisory by
            # design -- advisory-checks.yml owns its CI form -- and it currently
            # reports findings across this tree. The set that blocks is the
            # curated one in scripts/run-clang-tidy-blocking.ps1, which
            # scripts/verify.ps1 runs as its own step.
            $text = ($failures -join "`n")
            $lines = @($text -split "`r?`n" | Where-Object { $_ -match ': (warning|error): ' })
            Write-Host ("clang-tidy ADVISORY: {0} finding(s), last {1} shown" -f $lines.Count, $FailureTailLines)
            Write-Host ((@($text -split "`r?`n") | Select-Object -Last $FailureTailLines) -join "`n")
        }
        else {
            Write-Host "clang-tidy: OK"
        }
    }
    else {
        Write-Host "clang-tidy: SKIP (no C++ in scope: $srcScope)"
    }
}
else {
    # Not gated on -VerboseOutput. A skipped gate that says nothing is how this
    # one went unnoticed.
    Write-Host "clang-tidy: SKIP (no compile_commands.json; run: cmake --preset windows-x64-ninja-debug)"
}

# ---------------------------------------------------------------------------
# cppcheck
# ---------------------------------------------------------------------------

if ($Only -eq 'clang-tidy') {
    if ($VerboseOutput) { Write-Host "cppcheck: SKIP (-Only clang-tidy)" }
}
elseif ($cppcheck) {
    Invoke-QuietNative -Name 'cppcheck' -FilePath $cppcheck -Arguments @(
        '--enable=warning,performance,portability',
        '--std=c++20',
        '--error-exitcode=1',
        '--inline-suppr',
        '--suppressions-list=.cppcheck-suppress',
        '--library=windows',
        '--library=qt',
        '-q',
        '-I', 'libs/recorder_core/include',
        '-I', 'libs/capability/include',
        # Vendored third-party sources are not ours to analyze (matches the /W0
        # treatment in libs/update/CMakeLists.txt). Monocypher's header uses a
        # C++ `namespace` behind a macro guard that cppcheck mis-parses as C.
        '-i', 'libs/update/third_party',
        'libs',
        'app'
    )

    # -----------------------------------------------------------------------
    # Advisory: cppcheck --enable=unusedFunction (whole-program, non-blocking)
    #
    # unusedFunction requires a whole-program pass and is NOT combined with the
    # per-check run above. It surfaces functions never called from anywhere in
    # the analyzed translation units. High false-positive risk:
    #   - Qt slots invoked via QMetaObject::invokeMethod / connect() string form
    #   - Public API functions only called from tests
    #   - Callbacks registered via Windows APIs
    # Review candidates manually before removing. --error-exitcode is NOT set so
    # this pass never fails the script.
    # -----------------------------------------------------------------------
    Write-Host ""
    Write-Host "cppcheck unusedFunction (ADVISORY — non-blocking)..." -NoNewline
    $unusedArgs = @(
        '--enable=unusedFunction',
        '--std=c++20',
        '--inline-suppr',
        '--suppressions-list=.cppcheck-suppress',
        '--library=windows',
        '--library=qt',
        '-q',
        '-I', 'libs/recorder_core/include',
        '-I', 'libs/capability/include',
        '-i', 'libs/update/third_party',
        'libs',
        'app'
    )
    $unusedLogPath = Join-Path ([System.IO.Path]::GetTempPath()) "exosnap-cppcheck-unused-$PID.log"
    $unusedErrPath = "$unusedLogPath.err"
    try {
        $unusedProc = Start-Process -FilePath $cppcheck -ArgumentList $unusedArgs -WorkingDirectory $repoRoot `
            -NoNewWindow -PassThru -RedirectStandardOutput $unusedLogPath -RedirectStandardError $unusedErrPath
        $unusedProc.WaitForExit()

        # cppcheck emits findings to stderr; collect and count them.
        $unusedFindings = @()
        if (Test-Path -LiteralPath $unusedErrPath -PathType Leaf) {
            $unusedFindings = @(Get-Content -LiteralPath $unusedErrPath -ErrorAction SilentlyContinue |
                Where-Object { $_ -match '\(unusedFunction\)' })
        }
        $unusedCount = $unusedFindings.Count
        Write-Host " $unusedCount candidate(s)"
        if ($unusedCount -gt 0) {
            Write-Host "  [ADVISORY] Review before removing — Qt slots/callbacks are expected false-positives."
            # Cap console output; full details available by re-running with --enable=unusedFunction manually.
            $unusedFindings | Select-Object -First 20 | ForEach-Object { Write-Host "    $_" }
            if ($unusedCount -gt 20) { Write-Host "    ... ($($unusedCount - 20) more; run cppcheck --enable=unusedFunction manually for the full list)" }
        }
    }
    catch {
        Write-Host ""
        Write-Host "  cppcheck unusedFunction: advisory pass could not run — $_"
    }
    finally {
        Remove-Item -LiteralPath $unusedLogPath, $unusedErrPath -Force -ErrorAction SilentlyContinue
    }
}
else {
    Write-Host "cppcheck: SKIP (not installed; install with: winget install Cppcheck.Cppcheck)"
}

if ($StaticOnly) {
    Write-Host ""
    Write-Host "Static quality check passed."
    exit 0
}

# ---------------------------------------------------------------------------
# cmake configure + build + test
#
# Qt must be on PATH so that gtest_discover_tests POST_BUILD discovery can
# launch test executables that link Qt (0xc0000135 otherwise in worktrees).
# ---------------------------------------------------------------------------

# Resolved from .qt-version (see scripts/lib/QtEnvironment.psm1), so a Qt uplift
# does not leave this script pointing at the previous install.
Import-Module (Join-Path $PSScriptRoot 'lib/QtEnvironment.psm1') -Force
Add-QtToPath -RepoRoot $repoRoot | Out-Null

Invoke-QuietNative -Name 'cmake configure' -FilePath 'cmake' -Arguments @('--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'cmake build' -FilePath 'cmake' -Arguments @('--build', '--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'ctest' -FilePath 'ctest' -Arguments @('--preset', 'windows-x64-debug', '--output-on-failure')

Write-Host ""
Write-Host "Quality check passed."
