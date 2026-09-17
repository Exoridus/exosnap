#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the hard-wrapped prose guard.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    What this guard must NOT report matters more than what it must: a formatting
    rule with false positives is switched off within a week, and it is meant to
    become blocking once the tree's backlog is cleared.
    Every exemption the script claims -- code fences, tables, headings, front
    matter, list items, a long line that simply ends a paragraph -- gets a case
    here, next to the wrapped paragraph it must still catch.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$guard = Join-Path $scriptRoot 'check-prose-lines.ps1'

$script:LeakedGitVariables = @(
    'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_OBJECT_DIRECTORY',
    'GIT_ALTERNATE_OBJECT_DIRECTORIES', 'GIT_COMMON_DIR', 'GIT_PREFIX',
    'GIT_CEILING_DIRECTORIES', 'GIT_NAMESPACE', 'GIT_QUARANTINE_PATH')

function Invoke-IsolatedGit {
    param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $GitArgs)
    $saved = @{}
    foreach ($name in $script:LeakedGitVariables) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
        if ($null -ne $saved[$name]) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    }
    try { & git @GitArgs 2>&1 | Out-Null }
    finally {
        foreach ($name in $script:LeakedGitVariables) {
            if ($null -ne $saved[$name]) { Set-Item "Env:$name" -Value $saved[$name] }
        }
    }
}

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

function Invoke-Guard {
    <#
    .SYNOPSIS
        Runs the guard over a temp repository containing one Markdown file.
    .DESCRIPTION
        -All rather than the diff scope: the diff scope is tested separately,
        and deriving it here would test git's diff output rather than the rule.
    #>
    param([string] $Content, [string[]] $ExtraArgs = @())

    $root = Join-Path ([IO.Path]::GetTempPath()) "prose-lines-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    try {
        [IO.File]::WriteAllText((Join-Path $root 'doc.md'), $Content, [Text.UTF8Encoding]::new($false))
        Invoke-IsolatedGit -C $root init --quiet --initial-branch=main
        Invoke-IsolatedGit -C $root add -A
        $arguments = @('-NoProfile', '-NonInteractive', '-File', $guard, '-RepoRoot', $root, '-All') + $ExtraArgs
        $output = & pwsh @arguments 2>&1
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($output | Out-String) }
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

$longA = 'The recording engine keeps its own clock, and the muxer reads it once'
$longB = 'per segment rather than per packet, which is what makes a split cheap.'

Write-Host ''
Write-Host 'rejected'

Test-Case 'a paragraph wrapped at a column is reported' {
    $result = Invoke-Guard -Content "# Title`n`n$longA`n$longB`n"
    Assert-True ($result.ExitCode -ne 0) "a wrapped paragraph passed:`n$($result.Output)"
    Assert-True ($result.Output -match 'doc\.md:3') "the wrong line was named:`n$($result.Output)"
}

Test-Case 'every wrapped line of a longer paragraph is reported' {
    $result = Invoke-Guard -Content "$longA`n$longB`n$longA`n"
    Assert-True ($result.ExitCode -ne 0) "a wrapped paragraph passed:`n$($result.Output)"
    Assert-True ($result.Output -match '2 finding') "expected two reports:`n$($result.Output)"
}

Write-Host ''
Write-Host 'accepted'

Test-Case 'a long line that ends its paragraph is not a wrap' {
    # The decisive half of the rule. Without it every long final sentence in the
    # tree is a violation, and the gate is worthless.
    $result = Invoke-Guard -Content "$longA $longB`n`nAnother paragraph.`n"
    Assert-True ($result.ExitCode -eq 0) "a single long line was reported:`n$($result.Output)"
}

Test-Case 'a fenced code block is exempt' {
    $result = Invoke-Guard -Content "Text.`n`n``````powershell`n$longA`n$longB`n```````n"
    Assert-True ($result.ExitCode -eq 0) "code inside a fence was reported:`n$($result.Output)"
}

Test-Case 'a table is exempt' {
    $result = Invoke-Guard -Content "| $longA |`n| --- |`n| $longB |`n"
    Assert-True ($result.ExitCode -eq 0) "a table row was reported:`n$($result.Output)"
}

Test-Case 'an indented block is exempt' {
    $result = Invoke-Guard -Content "Text.`n`n    $longA`n    $longB`n"
    Assert-True ($result.ExitCode -eq 0) "an indented block was reported:`n$($result.Output)"
}

Test-Case 'YAML front matter is exempt' {
    $result = Invoke-Guard -Content "---`ndescription: $longA`nsummary: $longB`n---`n`nText.`n"
    Assert-True ($result.ExitCode -eq 0) "front matter was reported:`n$($result.Output)"
}

Test-Case 'consecutive list items are not a wrapped paragraph' {
    $result = Invoke-Guard -Content "- $longA`n- $longB`n"
    Assert-True ($result.ExitCode -eq 0) "list items were reported:`n$($result.Output)"
}

Test-Case 'a long heading followed by prose is not a wrap' {
    $result = Invoke-Guard -Content "## $longA`n$longB`n"
    Assert-True ($result.ExitCode -eq 0) "a heading was reported:`n$($result.Output)"
}

Test-Case 'an unterminated fence does not leak into the rest of the file' {
    # Fail-open on a malformed document: reporting the remainder of a file whose
    # fence someone forgot to close would be a report about the fence.
    $result = Invoke-Guard -Content "``````text`n$longA`n$longB`n"
    Assert-True ($result.ExitCode -eq 0) "an open fence was scanned as prose:`n$($result.Output)"
}

Test-Case '-Advisory reports the same findings without failing' {
    # The gate runs it this way today. A switch that also suppressed the report
    # would make the backlog invisible instead of deferred.
    $blocking = Invoke-Guard -Content "$longA`n$longB`n"
    $advisory = Invoke-Guard -Content "$longA`n$longB`n" -ExtraArgs @('-Advisory')
    Assert-True ($blocking.ExitCode -ne 0) "the blocking form passed:`n$($blocking.Output)"
    Assert-True ($advisory.ExitCode -eq 0) "the advisory form failed:`n$($advisory.Output)"
    Assert-True ($advisory.Output -match 'ADVISORY  doc\.md:1') "the finding was suppressed:`n$($advisory.Output)"
}

Write-Host ''
Write-Host 'scope'

Test-Case 'only the lines a branch adds are in scope by default' {
    $root = Join-Path ([IO.Path]::GetTempPath()) "prose-lines-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    try {
        [IO.File]::WriteAllText((Join-Path $root 'old.md'), "$longA`n$longB`n", [Text.UTF8Encoding]::new($false))
        Invoke-IsolatedGit -C $root init --quiet --initial-branch=main
        Invoke-IsolatedGit -C $root config user.name 'Fixture'
        Invoke-IsolatedGit -C $root config user.email 'fixture@example.invalid'
        Invoke-IsolatedGit -C $root config commit.gpgsign false
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'Initial import' --quiet
        $base = (& git -C $root rev-parse HEAD).Trim()

        Invoke-IsolatedGit -C $root checkout -q -b work
        [IO.File]::WriteAllText((Join-Path $root 'new.md'), "One sentence that stands on its own line.`n",
            [Text.UTF8Encoding]::new($false))
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'docs: add a page' --quiet

        $scoped = & pwsh -NoProfile -NonInteractive -File $guard -RepoRoot $root -Base $base 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0) "the pre-existing wrap was reported in the diff scope:`n$scoped"

        $swept = & pwsh -NoProfile -NonInteractive -File $guard -RepoRoot $root -All 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -ne 0) "the sweep missed the pre-existing wrap:`n$swept"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
