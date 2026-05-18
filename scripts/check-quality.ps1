$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

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

$srcFiles = git -C $repoRoot ls-files -- 'libs/' 'apps/' 'tests/' |
    Where-Object { $_ -match '\.(cpp|h)$' }

# ---------------------------------------------------------------------------
# clang-tidy
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "=== clang-tidy ==="
$compDb = Join-Path $repoRoot 'build/windows-x64-debug/compile_commands.json'
if (Test-Path -Path $compDb -PathType Leaf) {
    if ($srcFiles) {
        Push-Location $repoRoot
        try {
            & $clangTidy -p "build/windows-x64-debug" @srcFiles
            if ($LASTEXITCODE -ne 0) { throw "clang-tidy found issues." }
            Write-Host "clang-tidy: OK"
        }
        finally {
            Pop-Location
        }
    }
}
else {
    Write-Host "clang-tidy: SKIP (no compile_commands.json — run: cmake --preset windows-x64-debug)"
}

# ---------------------------------------------------------------------------
# cppcheck
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "=== cppcheck ==="
if ($cppcheck) {
    Push-Location $repoRoot
    try {
        & $cppcheck `
            --enable=warning,performance,portability `
            --std=c++20 `
            --error-exitcode=1 `
            --inline-suppr `
            "--suppressions-list=.cppcheck-suppress" `
            --library=windows `
            -q `
            -I libs/recorder_core/include `
            -I libs/capability/include `
            -I libs/recorder_facade/include `
            libs apps
        if ($LASTEXITCODE -ne 0) { throw "cppcheck found issues." }
        Write-Host "cppcheck: OK"
    }
    finally {
        Pop-Location
    }
}
else {
    Write-Host "cppcheck: SKIP (not installed — install with: winget install Cppcheck.Cppcheck)"
}

# ---------------------------------------------------------------------------
# cmake configure + build + test
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "=== cmake configure ==="
Push-Location $repoRoot
try {
    cmake --preset windows-x64-debug
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed." }
    Write-Host "cmake configure: OK"
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "=== cmake build ==="
Push-Location $repoRoot
try {
    cmake --build --preset windows-x64-debug
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed." }
    Write-Host "cmake build: OK"
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "=== ctest ==="
Push-Location $repoRoot
try {
    ctest --preset windows-x64-debug --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "ctest failed." }
    Write-Host "ctest: OK"
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "Quality check passed."
