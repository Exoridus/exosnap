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
    Assert-True ($body -match 'cargo exo-dev' -and $body -match '--profile ci-guardrails') `
        'guardrails does not run the ci-guardrails profile, which carries the title preflight'
    Assert-True ($body -match '--pr-title') `
        'guardrails does not pass the title, so a malformed title cannot stop the Windows legs'
    Assert-True ($body -match '--pr-number') `
        'guardrails does not pass the pull request number, so its preflight is weaker than the required check'
    Assert-True ($body -match 'github\.event\.pull_request\.title') `
        'guardrails reads no title'
    # Same rule set on both sides: both profiles plan the one commit-policy step,
    # whose arguments exo-dev builds in one place (tools/exo-dev/src/profile.rs
    # asserts the two profiles share it).
}

Test-Case 'the metadata workflow listens for the edit and checks the title' {
    $types = Get-PullRequestTypes -Content $prPolicy -Name 'pr-policy.yml'
    Assert-True ($types -contains 'edited') "pr-policy.yml does not trigger on 'edited' (types: $($types -join ', '))"
    Assert-True ($prPolicy -match 'cargo exo-dev' -and $prPolicy -match '--profile pr-policy' -and $prPolicy -match '--pr-title') `
        'pr-policy.yml does not run the commit policy check against the title'
    Assert-True ($prPolicy -match '--pr-number') `
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
Write-Host 'Pull request helpers'

Test-Case 'every process wrapper accepts an empty argument' {
    # Found the first time merge-pr.ps1 was used for real: `gh pr merge --body ""`
    # is how the merge body is cleared, and a [Parameter(Mandatory)] [string[]]
    # rejects a LIST containing an empty element before the tool is ever reached.
    # The failure is a parameter binding error at the call site, so nothing short
    # of invoking the wrapper with such a list catches it.
    foreach ($name in @('open-pr.ps1', 'merge-pr.ps1')) {
        $path = Join-Path (Split-Path -Parent $PSScriptRoot) $name
        Assert-True (Test-Path -LiteralPath $path) "$name is missing"

        $ast = [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$null)
        $wrappers = @($ast.FindAll({
                    param($node)
                    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                    $node.Name -match '^Invoke-(Git|Gh)$'
                }, $true))
        Assert-True ($wrappers.Count -gt 0) "$name declares no Invoke-Git/Invoke-Gh wrapper"

        foreach ($wrapper in $wrappers) {
            # Re-created from the AST rather than dot-sourced: these scripts run
            # their work at load, so importing one would open or merge something.
            $probe = [scriptblock]::Create("function $($wrapper.Name) $($wrapper.Body.Extent.Text)`n" +
                "$($wrapper.Name) -Arguments @('--version', '')")
            try { & $probe | Out-Null }
            catch [System.Management.Automation.ParameterBindingException] {
                throw "$name/$($wrapper.Name) rejects an empty argument: $($_.Exception.Message)"
            }
            catch {
                # Anything else means binding succeeded and the wrapper went on to
                # run the real tool, which is not what is under test here.
            }
        }
    }
}

Write-Host ''
Write-Host "$script:Passed passed, $script:Failed failed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
