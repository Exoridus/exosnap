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

# Per-clone local config, so it cannot be committed — set it alongside the hooks
# path instead. "only" makes a pull that would need a merge fail loudly rather
# than quietly recording one, which is how a stale local main goes unnoticed.
git -C $repoRoot config pull.ff only
if ($LASTEXITCODE -ne 0) { throw "git config failed." }

Write-Host "Git hooks path set to .githooks/"
Write-Host "  pre-commit -> branch guard + scripts/verify.ps1 -Fast -Staged  (scoped to the staged change)"
Write-Host "  pre-push   -> scripts/verify.ps1 -Full                         (every local blocking gate)"
Write-Host "pull.ff set to 'only' (a pull needing a merge fails instead of merging)"
