#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the changelog assembler and the release-notes renderer.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    Both read the same commit subject grammar that `exo-dev check
    commit-policy` enforces (`tools/exo-dev/src/commit_policy.rs`), so a
    subject that check would reject is never something the assembler or the
    renderer has to guess at.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$changelogScript = Join-Path $scriptRoot 'new-changelog.ps1'
$notesScript = Join-Path $scriptRoot 'render-release-notes.ps1'
$policyModule = Join-Path $scriptRoot 'lib/CommitPolicy.psm1'

# See check-drift.tests.ps1: under a git hook these are inherited from the
# repository being committed, GIT_DIR beats -C, and a fixture's `git init` lands
# on the real repository.
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

function Set-FixtureFile {
    param([string] $Root, [string] $Relative, [string] $Content)
    $path = Join-Path $Root $Relative
    $directory = Split-Path -Parent $path
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    Set-Content -LiteralPath $path -Value $Content -Encoding utf8
}

function New-FixtureRepo {
    <#
    .SYNOPSIS
        A repository with pre-policy history, the policy commit, and a main to
        branch from.
    .DESCRIPTION
        The policy epoch is the commit that ADDED scripts/lib/CommitPolicy.psm1,
        so every fixture has to contain that file for the check to have any
        scope at all -- which is itself the behaviour under test in the
        "no policy commit" case, where it is deliberately absent.
    .PARAMETER WithPolicy
        Commit the policy module. Off produces a repository the check must
        report as having nothing in scope.
    .PARAMETER PrePolicySubjects
        Subjects committed BEFORE the policy commit. These are grandfathered.
    #>
    param([switch] $WithPolicy, [string[]] $PrePolicySubjects = @())

    $root = Join-Path ([IO.Path]::GetTempPath()) "commit-policy-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $root -Force | Out-Null

    Invoke-IsolatedGit -C $root init --quiet --initial-branch=main
    Invoke-IsolatedGit -C $root config user.name 'Fixture'
    Invoke-IsolatedGit -C $root config user.email 'fixture@example.invalid'
    Invoke-IsolatedGit -C $root config commit.gpgsign false

    Set-FixtureFile -Root $root -Relative 'README.md' -Content 'Fixture.'
    Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content @'
# Changelog

## [Unreleased]
'@
    Invoke-IsolatedGit -C $root add -A
    Invoke-IsolatedGit -C $root commit -m 'Initial import' --quiet

    foreach ($subject in $PrePolicySubjects) {
        Set-FixtureFile -Root $root -Relative "before-$([guid]::NewGuid().ToString('n')).txt" -Content 'x'
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m $subject --quiet
    }

    if ($WithPolicy) {
        Copy-Item -LiteralPath $policyModule -Destination (Join-Path $root 'CommitPolicy.psm1') -Force
        New-Item -ItemType Directory -Path (Join-Path $root 'scripts/lib') -Force | Out-Null
        Move-Item -LiteralPath (Join-Path $root 'CommitPolicy.psm1') `
            -Destination (Join-Path $root 'scripts/lib/CommitPolicy.psm1') -Force
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'build(policy): adopt the commit subject grammar' --quiet
    }

    # The check diffs against origin/main when it exists. A fixture has no
    # remote, so main is the fallback -- and every case below branches off it.
    Invoke-IsolatedGit -C $root branch policy-base main
    return $root
}

function Add-FixtureCommit {
    param([string] $Root, [string] $Subject, [string] $Relative)
    if (-not $Relative) { $Relative = "f-$([guid]::NewGuid().ToString('n')).txt" }
    Set-FixtureFile -Root $Root -Relative $Relative -Content ([guid]::NewGuid().ToString())
    Invoke-IsolatedGit -C $Root add -A
    Invoke-IsolatedGit -C $Root commit -m $Subject --quiet
}

function Invoke-Changelog {
    param([string] $Root, [string[]] $ExtraArgs = @())
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $changelogScript, '-RepoRoot', $Root) + $ExtraArgs
    $output = & pwsh @arguments 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($output | Out-String) }
}

function Remove-Fixture {
    param([string] $Root)
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host 'changelog assembly'

Test-Case 'entries are grouped, and no-entry types produce nothing' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Add-FixtureCommit -Root $root -Subject 'feat(ui): a recording presets row (#10)'
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains (#11)'
        Add-FixtureCommit -Root $root -Subject 'ci: pin the runner image (#12)'
        $result = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD')
        Assert-True ($result.ExitCode -eq 0) "assembly failed:`n$($result.Output)"
        Assert-True ($result.Output -match '### Added') "Added section missing:`n$($result.Output)"
        Assert-True ($result.Output -match '### Fixed') "Fixed section missing:`n$($result.Output)"
        Assert-True ($result.Output -notmatch 'pin the runner image') "a ci commit produced an entry:`n$($result.Output)"
        # Two: the ci commit above, and the fixture's own build(policy) commit.
        Assert-True ($result.Output -match 'no entry  2') "the skipped count was wrong:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'an unreadable subject is reported, not silently dropped' {
    # A changelog assembled from a history it silently dropped half of is worse
    # than none: a reader cannot tell an empty section from an unparsed one.
    $root = New-FixtureRepo -WithPolicy
    try {
        Add-FixtureCommit -Root $root -Subject 'made the drains better'
        $result = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD')
        Assert-True ($result.ExitCode -ne 0) "an unreadable subject passed:`n$($result.Output)"
        Assert-True ($result.Output -match 'cannot file') "it was not reported:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'pre-policy subjects are counted as older, not as unreadable' {
    $root = New-FixtureRepo -WithPolicy -PrePolicySubjects @('Fix gate defects')
    try {
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains (#11)'
        $result = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD')
        Assert-True ($result.ExitCode -eq 0) "pre-policy history was reported as unreadable:`n$($result.Output)"
        # Two: the fixture's initial import and the pre-policy subject.
        Assert-True ($result.Output -match 'older     2') "the older count was wrong:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'the range starts at the last version tag' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Add-FixtureCommit -Root $root -Subject 'feat(ui): before the tag (#1)'
        Invoke-IsolatedGit -C $root tag v0.9.0
        Add-FixtureCommit -Root $root -Subject 'feat(ui): after the tag (#2)'
        $result = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD')
        Assert-True ($result.Output -match 'after the tag') "the post-tag commit is missing:`n$($result.Output)"
        Assert-True ($result.Output -notmatch 'before the tag') "a released commit was re-listed:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a release candidate is not a baseline the range may start at' {
    # git's version sort ranks v0.9.1-rc5 ABOVE v0.9.0, so an unfiltered tag list
    # hands back the newest release candidate -- and the release it is a candidate
    # FOR would then be described by whatever was merged after it.
    $root = New-FixtureRepo -WithPolicy
    try {
        Add-FixtureCommit -Root $root -Subject 'feat(ui): in the release line (#1)'
        Invoke-IsolatedGit -C $root tag v0.9.0
        Add-FixtureCommit -Root $root -Subject 'fix(engine): part of the next release (#2)'
        Invoke-IsolatedGit -C $root tag v0.9.1-rc1
        Add-FixtureCommit -Root $root -Subject 'fix(ui): merged after the candidate (#3)'

        $result = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD', '-Version', '0.9.1')
        Assert-True ($result.ExitCode -eq 0) "the cut failed:`n$($result.Output)"
        Assert-True ($result.Output -match 'part of the next release') `
            "the work the candidate carried was dropped from its own release:`n$($result.Output)"
        Assert-True ($result.Output -match 'merged after the candidate') `
            "the post-candidate commit is missing:`n$($result.Output)"
        Assert-True ($result.Output -notmatch 'in the release line') `
            "a commit released in v0.9.0 was listed again:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case '-Apply writes into Unreleased and -Version opens a new one' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains (#11)'
        $applied = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD', '-Apply')
        Assert-True ($applied.ExitCode -eq 0) "apply failed:`n$($applied.Output)"
        $text = Get-Content -LiteralPath (Join-Path $root 'CHANGELOG.md') -Raw
        Assert-True ($text -match '## \[Unreleased\]') 'the Unreleased heading was lost'
        Assert-True ($text -match 'bound the capture drains') "the entry was not written:`n$text"

        $cut = Invoke-Changelog -Root $root -ExtraArgs @('-Until', 'HEAD', '-Apply', '-Version', '0.9.1', '-Date', '2026-09-13')
        Assert-True ($cut.ExitCode -eq 0) "the cut failed:`n$($cut.Output)"
        $text = Get-Content -LiteralPath (Join-Path $root 'CHANGELOG.md') -Raw
        Assert-True ($text -match '## \[0\.9\.1\] - 2026-09-13') "the release heading is missing:`n$text"
        Assert-True ($text.IndexOf('## [Unreleased]') -lt $text.IndexOf('## [0.9.1]')) `
            'the new Unreleased section is not above the release'
    }
    finally { Remove-Fixture $root }
}

Write-Host ''
Write-Host 'release notes'

Test-Case 'the notes carry the changelog section and resolve every placeholder' {
    $root = New-FixtureRepo -WithPolicy
    try {
        New-Item -ItemType Directory -Path (Join-Path $root '.github/templates') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $scriptRoot) '.github/templates/release-notes.md') `
            -Destination (Join-Path $root '.github/templates/release-notes.md') -Force
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content @'
# Changelog

## [0.9.1] - 2026-09-13

### Fixed

- **Bound the capture drains.** ([#11](https://example.invalid/r/pull/11))
'@
        Add-FixtureCommit -Root $root -Subject 'chore(release): stage the notes fixture'
        Invoke-IsolatedGit -C $root tag v0.9.0

        $output = & pwsh -NoProfile -NonInteractive -File $notesScript -RepoRoot $root -Version '0.9.1' 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0) "rendering failed:`n$output"
        Assert-True ($output -match 'Bound the capture drains') "the changelog section is missing:`n$output"
        Assert-True ($output -notmatch '\$\{') "a placeholder survived:`n$output"
        Assert-True ($output -match 'v0\.9\.0\.\.\.v0\.9\.1') "the compare link is wrong:`n$output"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'the compare link of a release skips the candidates of that release' {
    # Git's version sort ranks v0.9.1-rc1 above v0.9.0, so the nearest tag by that
    # order is the release's own candidate -- and a full-changelog link to it shows
    # what was merged after the candidate instead of what the release contains.
    $root = New-FixtureRepo -WithPolicy
    try {
        New-Item -ItemType Directory -Path (Join-Path $root '.github/templates') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $scriptRoot) '.github/templates/release-notes.md') `
            -Destination (Join-Path $root '.github/templates/release-notes.md') -Force
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content @'
# Changelog

## [0.9.1] - 2026-09-13

### Fixed

- **Bound the capture drains.** ([#11](https://example.invalid/r/pull/11))
'@
        Add-FixtureCommit -Root $root -Subject 'chore(release): stage the notes fixture'
        Invoke-IsolatedGit -C $root tag v0.9.0
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains (#11)'
        Invoke-IsolatedGit -C $root tag v0.9.1-rc1

        $output = & pwsh -NoProfile -NonInteractive -File $notesScript -RepoRoot $root -Version '0.9.1' 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0) "rendering failed:`n$output"
        Assert-True ($output -match 'v0\.9\.0\.\.\.v0\.9\.1') `
            "the release compares against something other than the previous release:`n$output"
        Assert-True ($output -notmatch 'rc1\.\.\.') "the release compares against its own candidate:`n$output"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a candidate still compares against the candidate before it' {
    $root = New-FixtureRepo -WithPolicy
    try {
        New-Item -ItemType Directory -Path (Join-Path $root '.github/templates') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $scriptRoot) '.github/templates/release-notes-candidate.md') `
            -Destination (Join-Path $root '.github/templates/release-notes-candidate.md') -Force
        Add-FixtureCommit -Root $root -Subject 'chore(release): stage the notes fixture'
        Invoke-IsolatedGit -C $root tag v0.9.0
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains (#11)'
        Invoke-IsolatedGit -C $root tag v0.9.1-rc1

        $output = & pwsh -NoProfile -NonInteractive -File $notesScript -RepoRoot $root `
            -Version '0.9.1-rc2' -Candidate 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0) "rendering failed:`n$output"
        Assert-True ($output -match 'v0\.9\.1-rc1\.\.\.v0\.9\.1-rc2') `
            "a candidate lost the window it is read against:`n$output"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a template with an unknown placeholder fails instead of publishing it' {
    # A release published with a literal ${FOO} in it cannot be edited back out
    # of the history readers already saw.
    $root = New-FixtureRepo -WithPolicy
    try {
        Set-FixtureFile -Root $root -Relative '.github/templates/release-notes.md' `
            -Content "# ExoSnap `${VERSION}`n`n`${UNKNOWN_FIELD}`n"
        $output = & pwsh -NoProfile -NonInteractive -File $notesScript -RepoRoot $root -Version '0.9.1' 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -ne 0) "an unresolved placeholder was published:`n$output"
        Assert-True ($output -match 'UNKNOWN_FIELD') "the placeholder was not named:`n$output"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a candidate reads Unreleased, and says so when there is nothing' {
    $root = New-FixtureRepo -WithPolicy
    try {
        New-Item -ItemType Directory -Path (Join-Path $root '.github/templates') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $scriptRoot) '.github/templates/release-notes-candidate.md') `
            -Destination (Join-Path $root '.github/templates/release-notes-candidate.md') -Force
        $output = & pwsh -NoProfile -NonInteractive -File $notesScript -RepoRoot $root `
            -Version '0.9.1-rc3' -Candidate 2>&1 | Out-String
        Assert-True ($LASTEXITCODE -eq 0) "rendering failed:`n$output"
        Assert-True ($output -match 'No changelog entries') "an empty section was rendered blank:`n$output"
        Assert-True ($output -match 'do not announce') "the candidate warning is missing:`n$output"
    }
    finally { Remove-Fixture $root }
}

Write-Host ''
Write-Host "$script:Passed/$($script:Passed + $script:Failed) passed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
