#!/usr/bin/env pwsh
#Requires -Version 7.0
<#
.SYNOPSIS
    Report the difference between the rulesets this repository declares and the
    ones GitHub actually enforces.

.DESCRIPTION
    Branch and tag protection lives server-side, where it is invisible to review
    and changes without leaving a commit. `.github/rulesets/*.json` is the
    intended state; this script reads the live state and prints both plus the
    difference.

    It never writes. Applying a ruleset is a separately authorised act, and the
    README next to the payloads spells out the one command that does it.

.PARAMETER Desired
    Directory holding the intended ruleset payloads. Default: .github/rulesets.

.PARAMETER CurrentJson
    Read the live state from a file instead of the GitHub API — the array
    `gh api repos/:owner/:repo/rulesets` returns, with each entry's rules and
    bypass_actors expanded. Used by the tests, and usable offline.

.PARAMETER Quiet
    Print only the difference, not the full comparison.

.OUTPUTS
    Exit code 0 when the live state matches, 1 when it drifts, 2 when the live
    state could not be read at all.
#>
[CmdletBinding()]
param(
    [string]$Desired = '',
    [string]$CurrentJson = '',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Desired) { $Desired = Join-Path $repoRoot '.github/rulesets' }

# --- Intended state ----------------------------------------------------------
if (-not (Test-Path -LiteralPath $Desired -PathType Container)) {
    Write-Host "No ruleset directory at '$Desired'." -ForegroundColor Red
    exit 2
}

$desiredSets = @(Get-ChildItem -LiteralPath $Desired -Filter '*.json' | ForEach-Object {
    $payload = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
    [pscustomobject]@{ File = $_.Name; Payload = $payload }
})

if ($desiredSets.Count -eq 0) {
    Write-Host "No ruleset payloads under '$Desired'." -ForegroundColor Red
    exit 2
}

# --- Live state --------------------------------------------------------------
function Get-LiveRulesets {
    param([string] $FromFile)

    if ($FromFile) {
        return (Get-Content -LiteralPath $FromFile -Raw | ConvertFrom-Json)
    }

    $index = & gh api repos/:owner/:repo/rulesets 2>&1
    if ($LASTEXITCODE -ne 0) { throw "gh api repos/:owner/:repo/rulesets failed: $index" }

    # The index omits rules and bypass_actors; each ruleset has to be read.
    return @(($index | ConvertFrom-Json) | ForEach-Object {
        $detail = & gh api "repos/:owner/:repo/rulesets/$($_.id)" 2>&1
        if ($LASTEXITCODE -ne 0) { throw "gh api for ruleset $($_.id) failed: $detail" }
        $detail | ConvertFrom-Json
    })
}

try { $live = @(Get-LiveRulesets -FromFile $CurrentJson) }
catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host 'Could not read the live rulesets. This is not a pass.' -ForegroundColor Red
    exit 2
}

# --- Comparison --------------------------------------------------------------
# Matched on target plus the refs the ruleset applies to, not on the name: a
# ruleset's name is editable in the web UI (and one of these was misspelled for
# months), while what it protects is the thing that has to stay true.
function Get-MatchKey {
    param([Parameter(Mandatory)] $Ruleset)
    $includes = @()
    if ($Ruleset.conditions -and $Ruleset.conditions.ref_name) {
        $includes = @($Ruleset.conditions.ref_name.include) | Sort-Object
    }
    return "$($Ruleset.target)|$($includes -join ',')"
}

function Get-RuleParameters {
    param([Parameter(Mandatory)] $Ruleset, [Parameter(Mandatory)] [string] $Type)
    $rule = @($Ruleset.rules) | Where-Object { $_.type -eq $Type } | Select-Object -First 1
    if (-not $rule) { return $null }
    return $rule.parameters
}

function Format-Contexts {
    param($Parameters)
    if (-not $Parameters) { return '(rule absent)' }
    $contexts = @($Parameters.required_status_checks) | ForEach-Object { $_.context } | Sort-Object
    if (-not $contexts) { return '(none)' }
    return ($contexts -join ', ')
}

function Format-Bypass {
    param($Ruleset)
    $actors = @($Ruleset.bypass_actors)
    if (-not $actors) { return '(none)' }
    return (($actors | ForEach-Object { "$($_.actor_type)#$($_.actor_id):$($_.bypass_mode)" } | Sort-Object) -join ', ')
}

$differences = New-Object System.Collections.Generic.List[string]

function Compare-Field {
    param(
        [Parameter(Mandatory)] [string] $Scope,
        [Parameter(Mandatory)] [string] $Field,
        $Current,
        $Wanted
    )
    $currentText = if ($null -eq $Current) { '(absent)' } else { "$Current" }
    $wantedText  = if ($null -eq $Wanted)  { '(absent)' } else { "$Wanted" }
    if (-not $Quiet) {
        $marker = if ($currentText -eq $wantedText) { ' ' } else { '!' }
        Write-Host ("  {0} {1,-38} current: {2}" -f $marker, $Field, $currentText)
        if ($currentText -ne $wantedText) {
            Write-Host ("    {0,-38} desired: {1}" -f '', $wantedText) -ForegroundColor Yellow
        }
    }
    if ($currentText -ne $wantedText) {
        $differences.Add("$Scope :: $Field : current '$currentText', desired '$wantedText'")
    }
}

foreach ($entry in $desiredSets) {
    $wanted = $entry.Payload
    $key = Get-MatchKey -Ruleset $wanted
    $current = @($live) | Where-Object { (Get-MatchKey -Ruleset $_) -eq $key } | Select-Object -First 1

    Write-Host ''
    Write-Host "$($entry.File)  [$key]" -ForegroundColor Cyan

    if (-not $current) {
        Write-Host '  no live ruleset protects these refs' -ForegroundColor Yellow
        $differences.Add("$($entry.File) :: the repository has no ruleset for $key")
        continue
    }

    Compare-Field -Scope $entry.File -Field 'enforcement' -Current $current.enforcement -Wanted $wanted.enforcement
    Compare-Field -Scope $entry.File -Field 'rule types' `
        -Current ((@($current.rules) | ForEach-Object { $_.type } | Sort-Object) -join ', ') `
        -Wanted  ((@($wanted.rules)  | ForEach-Object { $_.type } | Sort-Object) -join ', ')
    Compare-Field -Scope $entry.File -Field 'bypass actors' `
        -Current (Format-Bypass -Ruleset $current) -Wanted (Format-Bypass -Ruleset $wanted)

    $currentChecks = Get-RuleParameters -Ruleset $current -Type 'required_status_checks'
    $wantedChecks  = Get-RuleParameters -Ruleset $wanted  -Type 'required_status_checks'
    if ($currentChecks -or $wantedChecks) {
        Compare-Field -Scope $entry.File -Field 'required status checks' `
            -Current (Format-Contexts -Parameters $currentChecks) `
            -Wanted  (Format-Contexts -Parameters $wantedChecks)
        Compare-Field -Scope $entry.File -Field 'strict (base must be current)' `
            -Current $(if ($currentChecks) { [bool]$currentChecks.strict_required_status_checks_policy } else { $null }) `
            -Wanted  $(if ($wantedChecks)  { [bool]$wantedChecks.strict_required_status_checks_policy }  else { $null })
    }

    $currentPr = Get-RuleParameters -Ruleset $current -Type 'pull_request'
    $wantedPr  = Get-RuleParameters -Ruleset $wanted  -Type 'pull_request'
    if ($currentPr -or $wantedPr) {
        Compare-Field -Scope $entry.File -Field 'pull request required' `
            -Current $(if ($currentPr) { 'yes' } else { 'no' }) `
            -Wanted  $(if ($wantedPr)  { 'yes' } else { 'no' })
        if ($currentPr -and $wantedPr) {
            Compare-Field -Scope $entry.File -Field 'required approving reviews' `
                -Current $currentPr.required_approving_review_count `
                -Wanted  $wantedPr.required_approving_review_count
        }
    }
}

# A live ruleset the repository does not declare is reported too: protection
# nobody wrote down is protection nobody is maintaining.
foreach ($set in $live) {
    $key = Get-MatchKey -Ruleset $set
    $declared = @($desiredSets) | Where-Object { (Get-MatchKey -Ruleset $_.Payload) -eq $key }
    if (-not $declared) {
        Write-Host ''
        Write-Host "undeclared live ruleset '$($set.name)'  [$key]" -ForegroundColor Yellow
        $differences.Add("live :: ruleset '$($set.name)' for $key is not declared under .github/rulesets")
    }
}

Write-Host ''
Write-Host ('-' * 60)
if ($differences.Count -eq 0) {
    Write-Host 'Rulesets match what the repository declares.' -ForegroundColor Green
    exit 0
}

Write-Host "$($differences.Count) difference(s):" -ForegroundColor Yellow
$differences | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
Write-Host ''
Write-Host 'Applying a ruleset is a separately authorised act; see .github/rulesets/README.md.' -ForegroundColor DarkGray
exit 1
