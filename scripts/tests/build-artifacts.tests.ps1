#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for scripts/lib/BuildArtifacts.psm1, the build-tree artifact resolver.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case builds a throwaway repository root holding real files with set
    timestamps, so the selection runs against a real filesystem rather than a stub.
    The defect these cover is the one that shipped: a first-match resolver returning
    a week-old binary while a fresh one sat in another tree.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'lib/BuildArtifacts.psm1') -Force -DisableNameChecking

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

function New-FixtureRoot {
    $root = Join-Path ([IO.Path]::GetTempPath()) "build-artifacts/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    return $root
}

function New-FixtureArtifact {
    <#
    .SYNOPSIS
        Write a file at a tree-relative path and give it a definite build time.
    #>
    param(
        [Parameter(Mandatory)] [string] $Root,
        [Parameter(Mandatory)] [string] $RelativePath,
        [Parameter(Mandatory)] [datetime] $BuiltAt
    )

    $full = Join-Path $Root $RelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $full) -Force | Out-Null
    Set-Content -LiteralPath $full -Value 'binary' -Encoding utf8
    (Get-Item -LiteralPath $full).LastWriteTime = $BuiltAt
    return $full
}

Write-Host 'BuildArtifacts resolver'

Test-Case 'the newest build wins over the one that comes first alphabetically' {
    # The shipped defect: the release tree was listed first and answered every call,
    # so a campaign ran a week-old tool while the afternoon's build sat unused.
    $root = New-FixtureRoot
    try {
        New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-11 18:00') `
            -RelativePath 'build/windows-x64-release/tools/envctl/Release/exosnap-envctl.exe' | Out-Null
        $fresh = New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-17 17:35') `
            -RelativePath 'build/windows-x64-ninja-debug/tools/envctl/exosnap-envctl.exe'

        $resolved = Resolve-BuiltArtifact -RelativePath 'tools/envctl/exosnap-envctl.exe' -RepoRoot $root
        Assert-True ($resolved.Path -ieq $fresh) "resolved $($resolved.Path), expected $fresh"
        Assert-True ($resolved.Tree -eq 'build/windows-x64-ninja-debug') "named tree $($resolved.Tree)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the MSBuild configuration directory is searched as well as the Ninja shape' {
    $root = New-FixtureRoot
    try {
        $only = New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-17 09:00') `
            -RelativePath 'build/windows-x64-debug/tools/envctl/Debug/exosnap-envctl.exe'

        $resolved = Resolve-BuiltArtifact -RelativePath 'tools/envctl/exosnap-envctl.exe' -RepoRoot $root
        Assert-True ($null -ne $resolved) 'the MSBuild layout was not found at all'
        Assert-True ($resolved.Path -ieq $only) "resolved $($resolved.Path), expected $only"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a tree that holds no copy resolves to nothing instead of throwing' {
    $root = New-FixtureRoot
    try {
        $resolved = Resolve-BuiltArtifact -RelativePath 'tools/envctl/exosnap-envctl.exe' -RepoRoot $root
        Assert-True ($null -eq $resolved) "resolved $resolved from an empty root"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a newer build elsewhere is reported to a caller that keeps its own order' {
    $root = New-FixtureRoot
    try {
        $chosen = New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-11 18:00') `
            -RelativePath 'build/windows-x64-release/app/Release/exosnap.exe'
        New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-17 17:35') `
            -RelativePath 'build/windows-x64-ninja-debug/app/exosnap.exe' | Out-Null

        $newer = Test-NewerArtifactExists -ChosenPath $chosen -RelativePath 'app/exosnap.exe' -RepoRoot $root
        Assert-True ($null -ne $newer) 'a build six days newer was not reported'
        Assert-True ($newer.Tree -eq 'build/windows-x64-ninja-debug') "named tree $($newer.Tree)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the newest build being the chosen one is reported as nothing to say' {
    $root = New-FixtureRoot
    try {
        $chosen = New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-17 17:35') `
            -RelativePath 'build/windows-x64-ninja-release/app/exosnap.exe'
        New-FixtureArtifact -Root $root -BuiltAt ([datetime]'2026-09-11 18:00') `
            -RelativePath 'build/windows-x64-debug/app/Debug/exosnap.exe' | Out-Null

        $newer = Test-NewerArtifactExists -ChosenPath $chosen -RelativePath 'app/exosnap.exe' -RepoRoot $root
        Assert-True ($null -eq $newer) 'a build older than the chosen one was reported as newer'
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
