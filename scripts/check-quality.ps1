param(
    [int]$FailureTailLines = 160,
    [switch]$VerboseOutput
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

    Write-Host "$Name..."
    $exitCode = 0
    $invokeError = $null

    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments *> $logPath
        $exitCode = $LASTEXITCODE
    }
    catch {
        $exitCode = if ($LASTEXITCODE -is [int]) { $LASTEXITCODE } else { 1 }
        $invokeError = $_
    }
    finally {
        Pop-Location
    }

    if ($invokeError) {
        Write-CommandFailure -Name $Name -LogPath $logPath -TailLines $FailureTailLines
        throw $invokeError
    }

    if ($exitCode -ne 0) {
        Write-CommandFailure -Name $Name -LogPath $logPath -TailLines $FailureTailLines
        throw "$Name failed with exit code $exitCode."
    }

    Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue
    Write-Host "${Name}: OK"
}

function Find-Tool {
    param([string]$Name, [switch]$Optional)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vsLlvm = Get-ChildItem -LiteralPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" `
        -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Llvm\\x64\\bin\\' } |
        Select-Object -First 1
    if ($vsLlvm) { return $vsLlvm.FullName }
    $llvm = Get-ChildItem -LiteralPath "${env:ProgramFiles}\LLVM" `
        -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($llvm) { return $llvm.FullName }
    $cppcheckDir = Get-ChildItem -LiteralPath "${env:ProgramFiles}\Cppcheck" `
        -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($cppcheckDir) { return $cppcheckDir.FullName }
    if ($Optional) { return $null }
    throw "$Name.exe not found on PATH, VS LLVM, LLVM, or Cppcheck install."
}

$clangTidy = Find-Tool 'clang-tidy'
$cppcheck  = Find-Tool 'cppcheck' -Optional

$srcFiles = @(git -C $repoRoot ls-files -- 'libs/' 'apps/' 'tests/' |
    Where-Object { $_ -match '\.(cpp|h)$' }
)

# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------

$compDb = Join-Path $repoRoot 'build/windows-x64-debug/compile_commands.json'
if (Test-Path -Path $compDb -PathType Leaf) {
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
        'apps'
    )
}
else {
    Write-Host "cppcheck: SKIP (not installed; install with: winget install Cppcheck.Cppcheck)"
}

# ---------------------------------------------------------------------------
# cmake configure + build + test
# ---------------------------------------------------------------------------

Invoke-QuietNative -Name 'cmake configure' -FilePath 'cmake' -Arguments @('--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'cmake build' -FilePath 'cmake' -Arguments @('--build', '--preset', 'windows-x64-debug')
Invoke-QuietNative -Name 'ctest' -FilePath 'ctest' -Arguments @('--preset', 'windows-x64-debug', '--output-on-failure')

Write-Host ""
Write-Host "Quality check passed."
