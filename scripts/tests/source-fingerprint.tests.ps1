#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the source identity a test result is attributed to.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Every case builds a throwaway git repository, so the real git answers the real
    question. The property under test is that two working trees which differ in
    anything the build can see produce different digests, and that a digest is
    never produced for a tree this code could not read.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptRoot 'lib/SourceFingerprint.psm1') -Force

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

function New-ThrowawayRepo {
    <#
    .SYNOPSIS
        A git repository with one commit, owned by the calling case.
    #>
    $root = Join-Path ([IO.Path]::GetTempPath()) "source-fingerprint/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    & git -C $root init --quiet 2>&1 | Out-Null
    & git -C $root config user.email 'tests@exosnap.invalid' | Out-Null
    & git -C $root config user.name 'ExoSnap tests' | Out-Null
    Set-Content -LiteralPath (Join-Path $root 'tracked.txt') -Value 'one' -Encoding utf8
    & git -C $root add -A 2>&1 | Out-Null
    & git -C $root commit -m 'initial' --quiet 2>&1 | Out-Null
    return $root
}

Write-Host 'source fingerprint'

Test-Case 'the same tree twice produces the same digest' {
    $repo = New-ThrowawayRepo
    try {
        $first = Get-SourceFingerprint -RepoRoot $repo
        $second = Get-SourceFingerprint -RepoRoot $repo
        Assert-True $first.ok "no digest: $($first.reason)"
        Assert-True ($first.fingerprint -eq $second.fingerprint) 'an unchanged tree produced two digests'
        Assert-True (-not $first.dirty) 'a clean tree reported itself dirty'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a modification to a tracked file changes the digest' {
    $repo = New-ThrowawayRepo
    try {
        $before = Get-SourceFingerprint -RepoRoot $repo
        Set-Content -LiteralPath (Join-Path $repo 'tracked.txt') -Value 'two' -Encoding utf8
        $after = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($after.fingerprint -ne $before.fingerprint) 'an edited tracked file left the digest unchanged'
        Assert-True $after.dirty 'a modified tree did not report itself dirty'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'the CONTENT of an untracked file is part of the digest' {
    # The hole this closes. A new source file is untracked until it is added, and a
    # digest over untracked NAMES reports the same value before and after every
    # edit to it -- so a test result would be attributed to source the build never
    # saw.
    $repo = New-ThrowawayRepo
    try {
        $new = Join-Path $repo 'new-source.cpp'
        Set-Content -LiteralPath $new -Value 'int main() { return 0; }' -Encoding utf8
        $before = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($before.untracked_files -eq 1) "expected 1 untracked file, got $($before.untracked_files)"

        Set-Content -LiteralPath $new -Value 'int main() { return 1; }' -Encoding utf8
        $after = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($after.fingerprint -ne $before.fingerprint) `
            'an untracked file was edited and the digest did not move'
        Assert-True $after.dirty 'a tree with an untracked file did not report itself dirty'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'an ignored file is not part of the digest' {
    # Build output lives under the working tree. Hashing it would turn the source
    # identity into a statement about build products, and every build would look
    # like a source change.
    $repo = New-ThrowawayRepo
    try {
        Set-Content -LiteralPath (Join-Path $repo '.gitignore') -Value 'build/' -Encoding utf8
        & git -C $repo add -A 2>&1 | Out-Null
        & git -C $repo commit -m 'ignore build' --quiet 2>&1 | Out-Null

        New-Item -ItemType Directory -Path (Join-Path $repo 'build') -Force | Out-Null
        $before = Get-SourceFingerprint -RepoRoot $repo
        Set-Content -LiteralPath (Join-Path $repo 'build/artifact.bin') -Value 'output' -Encoding utf8
        $after = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($after.fingerprint -eq $before.fingerprint) 'ignored build output moved the source digest'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'two untracked files cannot be swapped without moving the digest' {
    # A digest built from a set of content hashes with no names in it would be
    # identical for two files whose contents were exchanged.
    $repo = New-ThrowawayRepo
    try {
        Set-Content -LiteralPath (Join-Path $repo 'a.txt') -Value 'alpha' -Encoding utf8
        Set-Content -LiteralPath (Join-Path $repo 'b.txt') -Value 'beta' -Encoding utf8
        $before = Get-SourceFingerprint -RepoRoot $repo
        Set-Content -LiteralPath (Join-Path $repo 'a.txt') -Value 'beta' -Encoding utf8
        Set-Content -LiteralPath (Join-Path $repo 'b.txt') -Value 'alpha' -Encoding utf8
        $after = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($after.fingerprint -ne $before.fingerprint) `
            'two untracked files exchanged their contents and the digest did not move'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a directory that is not a repository yields a stated failure, not a digest' {
    # An empty digest would compare equal to the next failure, and two runs that
    # both failed to identify their source would look like two runs of the same one.
    $root = Join-Path ([IO.Path]::GetTempPath()) "source-fingerprint/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    try {
        $result = Get-SourceFingerprint -RepoRoot $root
        Assert-True (-not $result.ok) 'a directory outside any repository produced a digest'
        Assert-True ($null -eq $result.fingerprint) 'a failed identification still carried a digest'
        Assert-True ($result.reason -match 'rev-parse') "the failure did not say what failed: $($result.reason)"
    }
    finally { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'a tree with more untracked files than the cap is refused, not summarised' {
    $repo = New-ThrowawayRepo
    try {
        1..5 | ForEach-Object {
            Set-Content -LiteralPath (Join-Path $repo "extra-$_.txt") -Value "$_" -Encoding utf8
        }
        $result = Get-SourceFingerprint -RepoRoot $repo -MaxUntrackedFiles 3
        Assert-True (-not $result.ok) 'a tree above the untracked-file cap produced a digest anyway'
        Assert-True ($result.reason -match 'untracked files exceed') "unexpected reason: $($result.reason)"
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'untracked content above the byte cap is refused' {
    $repo = New-ThrowawayRepo
    try {
        Set-Content -LiteralPath (Join-Path $repo 'big.bin') -Value ('x' * 4096) -Encoding ascii
        $result = Get-SourceFingerprint -RepoRoot $repo -MaxUntrackedBytes 1024
        Assert-True (-not $result.ok) 'a tree above the untracked-byte cap produced a digest anyway'
        Assert-True ($result.reason -match 'exceed') "unexpected reason: $($result.reason)"
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'an untracked file git lists and this code cannot read is a failure' {
    # An unreadable input is an unknown input. Skipping it would produce a digest
    # that silently omits part of the source.
    $repo = New-ThrowawayRepo
    try {
        $locked = Join-Path $repo 'locked.txt'
        Set-Content -LiteralPath $locked -Value 'secret' -Encoding utf8
        $stream = [System.IO.File]::Open($locked, 'Open', 'Read', 'None')
        try {
            $result = Get-SourceFingerprint -RepoRoot $repo
            Assert-True (-not $result.ok) 'an unreadable untracked file was silently left out of the digest'
            Assert-True ($result.reason -match 'locked\.txt') "the failure did not name the file: $($result.reason)"
        }
        finally { $stream.Dispose() }
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Test-Case 'an edit that was reverted is indistinguishable, and the docs say so' {
    # Not a defect to fix here: a content digest cannot see history. It is stated
    # so that nobody reads the before/after pair as proof of an untouched tree --
    # the host tree lock is what keeps other entry points out, and this pair is
    # what ties the result to a source.
    $repo = New-ThrowawayRepo
    try {
        $before = Get-SourceFingerprint -RepoRoot $repo
        Set-Content -LiteralPath (Join-Path $repo 'tracked.txt') -Value 'two' -Encoding utf8
        Set-Content -LiteralPath (Join-Path $repo 'tracked.txt') -Value 'one' -Encoding utf8
        $after = Get-SourceFingerprint -RepoRoot $repo
        Assert-True ($after.fingerprint -eq $before.fingerprint) `
            'the premise of the documented limitation no longer holds; the documentation has to change with it'
    }
    finally { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
