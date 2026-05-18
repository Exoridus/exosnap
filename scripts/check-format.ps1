$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

function Find-Tool {
    param([string]$Name)
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
    throw "$Name.exe not found on PATH, VS LLVM, or LLVM install."
}

$clangFormat = Find-Tool 'clang-format'

Write-Host "=== clang-format ==="
$srcFiles = git -C $repoRoot ls-files -- 'libs/' 'apps/' 'tests/' |
    Where-Object { $_ -match '\.(cpp|h)$' }

if (-not $srcFiles) {
    Write-Host "No tracked source files found."
    exit 0
}

Push-Location $repoRoot
try {
    & $clangFormat --dry-run --Werror @srcFiles
    if ($LASTEXITCODE -ne 0) {
        Write-Error "clang-format violations found. Fix with: clang-format -i <file>"
    }
    Write-Host "clang-format: OK"
}
finally {
    Pop-Location
}
