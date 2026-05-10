$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

function Find-Tool {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vsLlvmX64 = (Get-ChildItem -LiteralPath "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022" -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Llvm\\x64\\bin\\' } |
        Select-Object -First 1)
    if ($vsLlvmX64) { return $vsLlvmX64.FullName }
    $llvmProg = (Get-ChildItem -LiteralPath "${env:ProgramFiles}\LLVM" -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1)
    if ($llvmProg) { return $llvmProg.FullName }
    throw "$Name.exe not found on PATH or in VS LLVM / LLVM install."
}

$clangFormat = Find-Tool 'clang-format'
$clangTidy   = Find-Tool 'clang-tidy'

Write-Host "=== clang-format check ==="
$srcFiles = git -C $repoRoot ls-files -- 'libs/' 'apps/' 'tests/' |
    Where-Object { $_ -match '\.(cpp|h)$' }
if ($srcFiles) {
    Push-Location $repoRoot
    try {
        & $clangFormat --dry-run --Werror @srcFiles
        if ($LASTEXITCODE -ne 0) { throw "clang-format found violations." }
        Write-Host "clang-format: OK"
    }
    finally {
        Pop-Location
    }
}
else {
    Write-Host "No tracked source files to check."
}

Write-Host ""
Write-Host "=== clang-tidy check ==="
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
    Write-Host "Compilation database not found at $compDb — skipping clang-tidy."
}

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
Write-Host "All checks passed."
