#Requires -Version 7.0
<#
.SYNOPSIS
    Tests that pull request METADATA validation and heavy code validation stay
    in separate workflows and separate status contexts.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    The thing under test is a cost contract, and a cost contract has no failing
    test unless it is asserted directly. The title is the only input to a merge
    verdict that is not in the tree, so it is the only one whose correction
    arrives as a `pull_request: edited` event. While that trigger sat on ci.yml,
    every title correction re-ran the Windows build and test matrix. Nothing in
    a build result would ever show that; only these assertions do.

    Read as text rather than as YAML: PowerShell 7 ships no YAML reader, and
    adding a dependency to assert the absence of one word is a worse trade than
    a narrow regex over a file actionlint already validates the syntax of.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$workflows = Join-Path $repoRoot '.github/workflows'
$ci = Get-Content -LiteralPath (Join-Path $workflows 'ci.yml') -Raw
$prPolicy = Get-Content -LiteralPath (Join-Path $workflows 'pr-policy.yml') -Raw

$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }

function Get-PullRequestTypes {
    param([Parameter(Mandatory)] [string] $Content, [Parameter(Mandatory)] [string] $Name)
    $match = [regex]::Match($Content, '(?m)^\s*pull_request:\s*\r?\n\s*types:\s*\[(?<types>[^\]]*)\]')
    if (-not $match.Success) { throw "$Name declares no pull_request types list" }
    return @($match.Groups['types'].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

Write-Host 'Workflow split'

Test-Case 'heavy CI does not listen for a title-only edit' {
    $types = Get-PullRequestTypes -Content $ci -Name 'ci.yml'
    Assert-True ($types -notcontains 'edited') `
        "ci.yml still triggers on 'edited'; a title correction would re-run the Windows matrix (types: $($types -join ', '))"
    # The events that DO have to stay, so this cannot be satisfied by deleting
    # the trigger outright.
    foreach ($required in @('opened', 'synchronize', 'reopened')) {
        Assert-True ($types -contains $required) "ci.yml no longer triggers on '$required'"
    }
}

Test-Case 'the job the heavy legs hang off validates the title itself' {
    # The authoritative verdict is pr-policy.yml's, but there is no
    # cross-workflow `needs:`, so a title that only pr-policy.yml rejects cannot
    # stop the Windows legs in this workflow. Removing this step would give back
    # exactly the first, most expensive run that Paket P was about.
    $block = [regex]::Match($ci, '(?ms)^  guardrails:\r?\n(?<body>.*?)(?=^  [a-z][a-z0-9-]*:\r?\n)')
    Assert-True $block.Success 'ci.yml has no guardrails job'
    $body = $block.Groups['body'].Value
    Assert-True ($body -match 'check-commit-policy\.ps1[\s\S]{0,200}-Subject') `
        'guardrails does not run the commit policy check against the title, so a malformed title cannot stop the Windows legs'
    Assert-True ($body -match '-PullRequestNumber') `
        'guardrails does not pass the pull request number, so its preflight is weaker than the required check'
    Assert-True ($body -match 'github\.event\.pull_request\.title') `
        'guardrails reads no title'

    # Same script, same rule set: a preflight that drifted from the required
    # check would fail a title the required check accepts, or the reverse.
    $ciArgs = [regex]::Match($body, 'check-commit-policy\.ps1(?<args>[\s\S]{0,200}?)(?=\r?\n\r?\n|\r?\n      -)').Groups['args'].Value
    $policyArgs = [regex]::Match($prPolicy, 'check-commit-policy\.ps1(?<args>[\s\S]{0,200}?)(?=\r?\n\r?\n|\Z)').Groups['args'].Value
    $normalize = { param($text) ($text -replace '\s+', ' ').Trim() }
    Assert-True ((& $normalize $ciArgs) -eq (& $normalize $policyArgs)) `
        "the preflight and the required check invoke the policy differently:`n  ci.yml      $(& $normalize $ciArgs)`n  pr-policy   $(& $normalize $policyArgs)"
}

Test-Case 'the metadata workflow listens for the edit and checks the title' {
    $types = Get-PullRequestTypes -Content $prPolicy -Name 'pr-policy.yml'
    Assert-True ($types -contains 'edited') "pr-policy.yml does not trigger on 'edited' (types: $($types -join ', '))"
    Assert-True ($prPolicy -match 'check-commit-policy\.ps1[\s\S]{0,200}-Subject') `
        'pr-policy.yml does not run the commit policy check against the title'
    Assert-True ($prPolicy -match '-PullRequestNumber') `
        'pr-policy.yml does not pass the pull request number, so a title carrying its own number would pass'
}

Test-Case 'the metadata verdict is its own required context' {
    Assert-True ($prPolicy -match '(?m)^\s*name:\s*pr-policy-required\s*$') `
        'pr-policy.yml declares no pr-policy-required aggregate job'
    # Its declared context names, not any mention: the comment below the job is
    # allowed to say what shape it copied.
    Assert-True ($prPolicy -notmatch '(?m)^\s*name:\s*ci-required\s*$') `
        'pr-policy.yml reports into ci-required; a green metadata run would then stand in for a failed code run'
}

Test-Case 'the expensive legs wait for the cheap guardrails' {
    # Both Windows legs, by the job keys they are declared under. A malformed
    # workflow, a drift, or a hygiene violation has to fail on ubuntu in under a
    # minute rather than after a ~15 min compile.
    foreach ($job in @('build-test-debug', 'build-test-release')) {
        $block = [regex]::Match($ci, "(?ms)^  $([regex]::Escape($job)):\r?\n(?<body>.*?)(?=^  [a-z][a-z0-9-]*:\r?\n)")
        Assert-True $block.Success "ci.yml has no job '$job'"
        Assert-True ($block.Groups['body'].Value -match '(?m)^\s*needs:.*guardrails') `
            "'$job' does not declare needs on guardrails, so it starts before the cheap checks have failed"
    }
}

Write-Host ''
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
