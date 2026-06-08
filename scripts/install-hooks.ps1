$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')

if (-not (Test-Path (Join-Path $repoRoot '.git'))) {
    throw "Not a git repository: $repoRoot"
}

# Point git at the tracked .githooks/ directory.
# CMake configure does this automatically; run this script manually after a
# bare clone without a configure step.
git -C $repoRoot config core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) { throw "git config failed." }

Write-Host "Git hooks path set to .githooks/"
Write-Host "  pre-commit -> scripts/check-format.ps1 -Staged -Fix  (quiet staged C++ autoformat)"
Write-Host "  pre-push   -> scripts/check-quality.ps1              (quiet branch-update quality gate)"
