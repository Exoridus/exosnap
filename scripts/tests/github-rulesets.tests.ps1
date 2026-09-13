#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the ruleset drift report.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case is a pair of fixtures -- an intended-state directory and a live
    state read from a file -- so nothing reaches GitHub and the cases that
    matter (a weaker live state than the repository declares) are reachable at
    all. Each rule is tested both ways: a matching pair has to be accepted, or a
    checker that rejects everything would look like a working guard.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$checker = Join-Path $scriptRoot 'check-github-rulesets.ps1'

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

function New-DesiredRuleset {
    <#
    .SYNOPSIS
        The intended state: one branch ruleset requiring one aggregate context.
    #>
    param([string[]] $Contexts = @('ci-required'), [bool] $Strict = $true)

    return [ordered]@{
        name       = 'Block push on main'
        target     = 'branch'
        enforcement = 'active'
        conditions = @{ ref_name = @{ include = @('~DEFAULT_BRANCH'); exclude = @() } }
        bypass_actors = @(@{ actor_id = 5; actor_type = 'RepositoryRole'; bypass_mode = 'pull_request' })
        rules = @(
            @{ type = 'deletion' },
            @{ type = 'non_fast_forward' },
            @{ type = 'pull_request'; parameters = @{ required_approving_review_count = 0 } },
            @{ type = 'required_status_checks'; parameters = @{
                strict_required_status_checks_policy = $Strict
                required_status_checks = @($Contexts | ForEach-Object { @{ context = $_ } })
            } }
        )
    }
}

function Invoke-Checker {
    <#
    .SYNOPSIS
        Run the checker over a fixture pair and return its exit code and output.
    #>
    param([Parameter(Mandatory)] $DesiredPayloads, [Parameter(Mandatory)] $LivePayloads)

    $root = Join-Path ([IO.Path]::GetTempPath()) "github-rulesets-tests/$([guid]::NewGuid().ToString('n'))"
    $desiredDir = Join-Path $root 'desired'
    New-Item -ItemType Directory -Path $desiredDir -Force | Out-Null

    $index = 0
    foreach ($payload in @($DesiredPayloads)) {
        $index++
        $payload | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $desiredDir "set$index.json") -Encoding utf8
    }

    $liveFile = Join-Path $root 'live.json'
    # A single-element array must survive ConvertTo-Json as an array.
    ConvertTo-Json -InputObject @($LivePayloads) -Depth 12 -AsArray |
        Set-Content -LiteralPath $liveFile -Encoding utf8

    $output = (& pwsh -NoProfile -NonInteractive -File $checker -Desired $desiredDir -CurrentJson $liveFile 2>&1 | Out-String)
    $exit = $LASTEXITCODE

    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
    return [pscustomobject]@{ ExitCode = $exit; Output = $output }
}

Write-Host 'check-github-rulesets.ps1'

Test-Case 'a live state matching the declared one is accepted' {
    $wanted = New-DesiredRuleset
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $wanted
    Assert-True ($result.ExitCode -eq 0) "expected 0, got $($result.ExitCode): $($result.Output)"
}

Test-Case 'a required context missing from the live state is reported' {
    $wanted = New-DesiredRuleset -Contexts @('ci-required', 'crash-capture-required')
    $live = New-DesiredRuleset -Contexts @('ci-required')
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $live
    Assert-True ($result.ExitCode -eq 1) "expected drift (1), got $($result.ExitCode)"
    Assert-True ($result.Output -match 'required status checks') 'the missing context was not named'
}

Test-Case 'a live state that dropped the pull-request rule is reported' {
    $wanted = New-DesiredRuleset
    $live = New-DesiredRuleset
    $live.rules = @($live.rules | Where-Object { $_.type -ne 'pull_request' })
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $live
    Assert-True ($result.ExitCode -eq 1) "expected drift (1), got $($result.ExitCode)"
    Assert-True ($result.Output -match 'pull request required') 'the dropped rule was not named'
}

Test-Case 'a wider bypass than declared is reported' {
    $wanted = New-DesiredRuleset
    $live = New-DesiredRuleset
    $live.bypass_actors = @(
        @{ actor_id = 2; actor_type = 'RepositoryRole'; bypass_mode = 'always' },
        @{ actor_id = 5; actor_type = 'RepositoryRole'; bypass_mode = 'always' }
    )
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $live
    Assert-True ($result.ExitCode -eq 1) "expected drift (1), got $($result.ExitCode)"
    Assert-True ($result.Output -match 'bypass actors') 'the wider bypass was not named'
}

Test-Case 'a non-strict live policy is reported' {
    $wanted = New-DesiredRuleset -Strict $true
    $live = New-DesiredRuleset -Strict $false
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $live
    Assert-True ($result.ExitCode -eq 1) "expected drift (1), got $($result.ExitCode)"
    Assert-True ($result.Output -match 'strict') 'the base-currency policy was not named'
}

Test-Case 'refs nobody protects are reported' {
    $wanted = New-DesiredRuleset
    $live = New-DesiredRuleset
    $live.conditions = @{ ref_name = @{ include = @('refs/heads/something-else'); exclude = @() } }
    $result = Invoke-Checker -DesiredPayloads $wanted -LivePayloads $live
    Assert-True ($result.ExitCode -eq 1) "expected drift (1), got $($result.ExitCode)"
    Assert-True ($result.Output -match 'no live ruleset protects these refs') 'the unprotected refs were not named'
    Assert-True ($result.Output -match 'not declared') 'the undeclared live ruleset was not named'
}

Test-Case 'a live state that cannot be read is not a pass' {
    $desiredDir = Join-Path ([IO.Path]::GetTempPath()) "github-rulesets-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $desiredDir -Force | Out-Null
    try {
        New-DesiredRuleset | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $desiredDir 'set1.json') -Encoding utf8
        $missing = Join-Path $desiredDir 'no-such-live.json'
        & pwsh -NoProfile -NonInteractive -File $checker -Desired $desiredDir -CurrentJson $missing *> $null
        Assert-True ($LASTEXITCODE -eq 2) "expected the unreadable-state code (2), got $LASTEXITCODE"
    }
    finally { Remove-Item -LiteralPath $desiredDir -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the payloads this repository ships are well-formed and complete' {
    # The files are applied to GitHub verbatim, so a typo here is only visible
    # at apply time -- after the authorisation to apply has been given.
    $shipped = Get-ChildItem -LiteralPath (Join-Path $scriptRoot '../.github/rulesets') -Filter '*.json'
    Assert-True ($shipped.Count -ge 2) "expected at least two ruleset payloads, found $($shipped.Count)"
    foreach ($file in $shipped) {
        $payload = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        Assert-True ($payload.name) "$($file.Name): no name"
        Assert-True ($payload.target -in @('branch', 'tag')) "$($file.Name): target '$($payload.target)'"
        Assert-True ($payload.enforcement -eq 'active') "$($file.Name): enforcement '$($payload.enforcement)'"
        Assert-True (@($payload.conditions.ref_name.include).Count -ge 1) "$($file.Name): no refs"
        Assert-True (@($payload.rules).Count -ge 1) "$($file.Name): no rules"
        foreach ($actor in @($payload.bypass_actors)) {
            Assert-True ($actor.bypass_mode -in @('always', 'pull_request')) `
                "$($file.Name): bypass_mode '$($actor.bypass_mode)'"
        }
    }
}

Test-Case 'the required contexts name jobs that exist and always report' {
    # A required context that no job produces is a rule nothing can ever
    # satisfy; one produced by a job with an event filter is a rule a skip can
    # satisfy. Both are checked against the workflows themselves.
    $mainRuleset = Get-Content -LiteralPath (Join-Path $scriptRoot '../.github/rulesets/main-branch.json') -Raw | ConvertFrom-Json
    $checks = @($mainRuleset.rules | Where-Object { $_.type -eq 'required_status_checks' } |
        Select-Object -First 1).parameters.required_status_checks | ForEach-Object { $_.context }

    Assert-True (@($checks).Count -ge 1) 'no required contexts declared'

    $workflowText = (Get-ChildItem -LiteralPath (Join-Path $scriptRoot '../.github/workflows') -Filter '*.yml' |
        ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"

    foreach ($context in $checks) {
        Assert-True ($workflowText -match "(?m)^\s+name:\s+$([regex]::Escape($context))\s*$") `
            "required context '$context' is not produced by any job in .github/workflows"
        # The aggregate jobs carry `if: always()`; that is what makes them
        # unable to skip themselves out of the required set.
        $jobBlock = [regex]::Match($workflowText, "(?ms)^\s+name:\s+$([regex]::Escape($context))\s*$.{0,600}")
        Assert-True ($jobBlock.Value -match 'if:\s+always\(\)') `
            "required context '$context' is produced by a job that can skip itself"
    }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
