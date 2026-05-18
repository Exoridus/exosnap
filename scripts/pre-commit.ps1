$ErrorActionPreference = 'Stop'
$scriptsDir = $PSScriptRoot

# Convenience wrapper — runs all checks in sequence.
# For granular use: check-format.ps1 (fast) and check-quality.ps1 (full).
# To install as automatic git hooks: scripts/install-hooks.ps1

& "$scriptsDir\check-format.ps1"
& "$scriptsDir\check-quality.ps1"
