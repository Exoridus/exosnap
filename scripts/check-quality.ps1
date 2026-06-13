param(
    [int]$FailureTailLines = 160,
    [switch]$VerboseOutput,
    [switch]$StaticOnly
)

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

# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------

$compDb = Join-Path $repoRoot 'build/windows-x64-debug/compile_commands.json'
if (Test-Path -Path $compDb -PathType Leaf) {
    if (-not $clangTidy) {
        throw "clang-tidy.exe not found on PATH, VS LLVM, or LLVM install."
    }

    if ($srcFiles) {
        Invoke-QuietNative -Name 'clang-tidy' -FilePath $clangTidy -Arguments (@('-p', 'build/windows-x64-debug') + $srcFiles)
    }
    else {
        Write-Host "clang-tidy: SKIP (no tracked C++ source files)"
    }
}
else {
    if ($VerboseOutput) {
        Write-Host "clang-tidy: SKIP (no compile_commands.json; run: cmake --preset windows-x64-debug)"
    }
}

# ---------------------------------------------------------------------------
# cppcheck
# ---------------------------------------------------------------------------

if ($cppcheck) {
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
        '-I', 'libs/recorder_facade/include',
        'libs',
        'app'
    )
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

$qtBin = 'C:\Qt\6.9.0\msvc2022_64\bin'
if (Test-Path $qtBin -PathType Container) {
    $env:PATH = "$qtBin;$env:PATH"
}

Invoke-QuietNative -Name 'cmake configure' -FilePath 'cmake' -Arguments @('--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'cmake build' -FilePath 'cmake' -Arguments @('--build', '--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'ctest' -FilePath 'ctest' -Arguments @('--preset', 'windows-x64-debug', '--output-on-failure')

Write-Host ""
Write-Host "Quality check passed."
