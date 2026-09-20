#Requires -Version 7.0
<#
.SYNOPSIS
    Fails if docs/superpowers/ has been recreated.

.DESCRIPTION
    docs/superpowers/ held agent-generated implementation plans and specs that
    were promoted or deleted as part of the documentation architecture cleanup
    (AGENTS.md, "Working context"). That work belongs in the untracked private
    working-context directory instead, and a durable conclusion is promoted
    into the matching docs/ category. This is a path check, not a content
    rule: any tracked file under docs/superpowers/ is itself the violation.

.PARAMETER RepoRoot
    Repository to check. Defaults to the repository this script lives in.

.EXAMPLE
    .\scripts\check-docs-superpowers-removed.ps1
#>

param(
    [string] $RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$tracked = @(& git -C $RepoRoot ls-files -- 'docs/superpowers/**')

if ($tracked.Count -gt 0) {
    Write-Host ''
    Write-Host 'docs-superpowers-removed: docs/superpowers/ must not be recreated.'
    Write-Host 'Agent-generated plans, specs and research belong in .workspace/ instead;'
    Write-Host 'a durable conclusion is promoted into the matching docs/ category.'
    foreach ($path in $tracked) { Write-Host "  $path" }
    exit 1
}

Write-Host ''
Write-Host 'docs-superpowers-removed: OK'
exit 0
