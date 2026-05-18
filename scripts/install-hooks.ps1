$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$hooksDir = Join-Path $repoRoot '.git\hooks'

if (-not (Test-Path $hooksDir)) {
    throw ".git/hooks not found — is this a git repository?"
}

# pre-commit: format check only (fast, ~1s)
$preCommitPath = Join-Path $hooksDir 'pre-commit'
$preCommitScript = @"
#!/bin/sh
pwsh -NonInteractive -File "`$(git rev-parse --show-toplevel)/scripts/check-format.ps1"
"@
[System.IO.File]::WriteAllText($preCommitPath, $preCommitScript.Replace("`r`n", "`n"))

# pre-push: full quality check (clang-tidy + cppcheck + build + test)
$prePushPath = Join-Path $hooksDir 'pre-push'
$prePushScript = @"
#!/bin/sh
pwsh -NonInteractive -File "`$(git rev-parse --show-toplevel)/scripts/check-quality.ps1"
"@
[System.IO.File]::WriteAllText($prePushPath, $prePushScript.Replace("`r`n", "`n"))

Write-Host "Git hooks installed:"
Write-Host "  pre-commit -> scripts/check-format.ps1   (clang-format)"
Write-Host "  pre-push   -> scripts/check-quality.ps1  (clang-tidy + cppcheck + build + test)"
Write-Host ""
Write-Host "Re-run this script after cloning to reinstall hooks."
