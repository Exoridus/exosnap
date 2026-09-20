#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the guard that keeps docs/superpowers/ from being recreated.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case builds a throwaway git repository in the temp directory and
    points check-docs-superpowers-removed.ps1 at it. Nothing touches this
    repository.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$guard = Join-Path $scriptRoot 'check-docs-superpowers-removed.ps1'

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

function New-FixtureRepo {
    param([hashtable] $Files = @{})

    $root = Join-Path ([IO.Path]::GetTempPath()) "docs-superpowers-removed-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    $base = @{ 'README.md' = "# fixture`n" }
    foreach ($key in $Files.Keys) { $base[$key] = $Files[$key] }

    foreach ($relative in $base.Keys) {
        $path = Join-Path $root $relative
        $directory = Split-Path -Parent $path
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
        Set-Content -LiteralPath $path -Value $base[$relative] -Encoding utf8 -NoNewline
    }

    & git -C $root init --quiet
    & git -C $root add -A
    return $root
}

function Invoke-Guard {
    param([string] $Root)
    $output = & pwsh -NoProfile -NonInteractive -File $guard -RepoRoot $Root 2>&1 | Out-String
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Test-Fixture {
    param([hashtable] $Files = @{}, [scriptblock] $Assert)
    $root = New-FixtureRepo -Files $Files
    try { & $Assert (Invoke-Guard -Root $root) }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host 'docs-superpowers-removed'

Test-Case 'a repository with no docs/superpowers/ passes' {
    Test-Fixture -Assert {
        param($result)
        Assert-True ($result.ExitCode -eq 0) "a clean fixture must pass, but the guard said:`n$($result.Output)"
    }
}

Test-Case 'a tracked file under docs/superpowers/ is rejected' {
    Test-Fixture -Files @{
        'docs/superpowers/plans/2026-01-01-example.md' = "# example plan`n"
    } -Assert {
        param($result)
        Assert-True ($result.ExitCode -ne 0) 'a recreated docs/superpowers/ file must be rejected'
        Assert-True ($result.Output -match 'docs/superpowers/plans/2026-01-01-example\.md') `
            "the offending path must be named:`n$($result.Output)"
    }
}

Test-Case 'an untracked file under docs/superpowers/ does not fail the guard' {
    # git ls-files only reports what would actually be committed; an untracked
    # scratch file is not yet the violation this guard exists to catch.
    $root = New-FixtureRepo
    try {
        New-Item -ItemType Directory -Path (Join-Path $root 'docs/superpowers') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $root 'docs/superpowers/scratch.md') -Value '# scratch' -Encoding utf8
        $result = Invoke-Guard -Root $root
        Assert-True ($result.ExitCode -eq 0) "an untracked file must not fail the guard:`n$($result.Output)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the real repository passes' {
    $result = Invoke-Guard -Root (Resolve-Path (Join-Path $scriptRoot '..')).Path
    Assert-True ($result.ExitCode -eq 0) "this repository must satisfy its own guard:`n$($result.Output)"
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
