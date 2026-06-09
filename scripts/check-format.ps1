param(
    [switch]$Fix,
    [switch]$Staged,
    [switch]$VerboseOutput
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Find-Tool {
    param([string]$Name)
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
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd -and (Test-ToolCandidate $cmd.Source)) { return $cmd.Source }
    throw "$Name.exe not found on PATH, VS LLVM, or LLVM install."
}

$clangFormat = Find-Tool 'clang-format'

if ($VerboseOutput) {
    Write-Host "clang-format..."
}

if ($Staged) {
    $srcFiles = @(git -C $repoRoot diff --cached --name-only --diff-filter=ACMR -- 'libs/' 'apps/' 'tests/' |
        Where-Object { $_ -match '\.(cpp|h)$' })
}
else {
    $srcFiles = @(git -C $repoRoot ls-files -- 'libs/' 'apps/' 'tests/' |
        Where-Object { $_ -match '\.(cpp|h)$' })
}

if (-not $srcFiles) {
    $scope = if ($Staged) { "staged" } else { "tracked" }
    if ($VerboseOutput) {
        Write-Host "clang-format: SKIP (no $scope C++ source files)"
    }
    exit 0
}

Push-Location $repoRoot
try {
    if ($Fix -and $Staged) {
        $unstagedFiles = @(git -C $repoRoot diff --name-only -- 'libs/' 'apps/' 'tests/' |
            Where-Object { $_ -match '\.(cpp|h)$' })
        $unstagedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($file in $unstagedFiles) {
            [void]$unstagedSet.Add($file)
        }

        $overlap = @($srcFiles | Where-Object { $unstagedSet.Contains($_) })
        if ($overlap) {
            $fileList = ($overlap | Select-Object -First 8) -join ', '
            if ($overlap.Count -gt 8) { $fileList += ", ..." }
            throw "Cannot autoformat staged files that also have unstaged edits: $fileList. Stage the full file or run scripts/check-format.ps1 -Fix manually."
        }
    }

    if ($Fix) {
        & $clangFormat -i @srcFiles
        if ($LASTEXITCODE -ne 0) { throw "clang-format failed." }

        if ($Staged) {
            git -C $repoRoot add -- @srcFiles
            if ($LASTEXITCODE -ne 0) { throw "git add failed after clang-format." }
        }

        if ($VerboseOutput) {
            Write-Host "clang-format: OK (formatted $($srcFiles.Count) file(s))"
        }
        exit 0
    }

    & $clangFormat --dry-run --Werror @srcFiles
    if ($LASTEXITCODE -ne 0) {
        Write-Error "clang-format violations found. Fix with: clang-format -i <file>"
    }
    if ($VerboseOutput) {
        Write-Host "clang-format: OK"
    }
}
finally {
    Pop-Location
}
