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
    $srcFiles = @(git -C $repoRoot diff --cached --name-only --diff-filter=ACMR -- 'libs/' 'app/' 'tests/' |
        Where-Object { $_ -match '\.(cpp|h)$' })
}
else {
    $srcFiles = @(git -C $repoRoot ls-files -- 'libs/' 'app/' 'tests/' |
        Where-Object { $_ -match '\.(cpp|h)$' -and (Test-Path -LiteralPath (Join-Path $repoRoot $_) -PathType Leaf) })
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
        $unstagedFiles = @(git -C $repoRoot diff --name-only -- 'libs/' 'app/' 'tests/' |
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

    # Passing every source file in one invocation eventually exceeds the command
    # line length limit ("Der Dateiname oder die Erweiterung ist zu lang") — the
    # file list grows with the repository, and a deep checkout path pushes it
    # over sooner. Chunk it so the limit is a function of the batch, not of how
    # many files the project has.
    $batchSize = 100

    function Invoke-InBatches {
        param([string[]]$Files, [string[]]$Arguments)
        for ($i = 0; $i -lt $Files.Count; $i += $batchSize) {
            $batch = $Files[$i..([Math]::Min($i + $batchSize - 1, $Files.Count - 1))]
            & $clangFormat @Arguments @batch
            if ($LASTEXITCODE -ne 0) { return $LASTEXITCODE }
        }
        return 0
    }

    if ($Fix) {
        $code = Invoke-InBatches -Files $srcFiles -Arguments @('-i')
        if ($code -ne 0) { throw "clang-format failed." }

        if ($Staged) {
            git -C $repoRoot add -- @srcFiles
            if ($LASTEXITCODE -ne 0) { throw "git add failed after clang-format." }
        }

        if ($VerboseOutput) {
            Write-Host "clang-format: OK (formatted $($srcFiles.Count) file(s))"
        }
        exit 0
    }

    $code = Invoke-InBatches -Files $srcFiles -Arguments @('--dry-run', '--Werror')
    if ($code -ne 0) {
        Write-Error "clang-format violations found. Fix with: clang-format -i <file>"
    }
    if ($VerboseOutput) {
        Write-Host "clang-format: OK"
    }
}
finally {
    Pop-Location
}
