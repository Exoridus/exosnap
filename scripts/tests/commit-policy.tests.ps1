#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the commit subject grammar, the commit policy check, the changelog
    assembler and the release-notes renderer.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use.

    These four share one parser, so they are tested together: a change to the
    grammar that the assembler follows and the check does not would let a
    subject pass review and then fail the cut, which is the failure the shared
    module exists to prevent.

    Every rule is tested BOTH ways. A check that has only ever been seen green
    proves nothing, so each rule gets a repository it must reject and one it
    must accept, and the accepted ones deliberately include the shapes that look
    like violations: a subject that legitimately carries no changelog entry, a
    changelog written during a declared release cut, and history from before the
    policy took effect.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
$policyCheck = Join-Path $scriptRoot 'check-commit-policy.ps1'
$changelogScript = Join-Path $scriptRoot 'new-changelog.ps1'
$notesScript = Join-Path $scriptRoot 'render-release-notes.ps1'
$policyModule = Join-Path $scriptRoot 'lib/CommitPolicy.psm1'

Import-Module $policyModule -Force

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

function Invoke-PolicyCheck {
    param([string] $Root, [string[]] $ExtraArgs = @())
    $arguments = @('-NoProfile', '-NonInteractive', '-File', $policyCheck, '-RepoRoot', $Root) + $ExtraArgs
    $output = & pwsh @arguments 2>&1
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($output | Out-String) }
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
Write-Host 'Subject grammar'

Test-Case 'a plain type and summary parses' {
    $parsed = ConvertFrom-CommitSubject -Subject 'fix: bound the capture drains'
    Assert-True $parsed.Valid "expected valid, got: $($parsed.Problem)"
    Assert-True ($parsed.Type -eq 'fix') "type was $($parsed.Type)"
    Assert-True ($null -eq $parsed.Scope) "scope was $($parsed.Scope)"
    Assert-True ($parsed.Section -eq 'Fixed') "section was $($parsed.Section)"
}

Test-Case 'scope, breaking marker and pull request number all parse' {
    $parsed = ConvertFrom-CommitSubject -Subject 'refactor(engine)!: one device generation contract (#391)'
    Assert-True $parsed.Valid "expected valid, got: $($parsed.Problem)"
    Assert-True ($parsed.Scope -eq 'engine') "scope was $($parsed.Scope)"
    Assert-True $parsed.Breaking 'breaking marker was not read'
    Assert-True ($parsed.PullRequest -eq 391) "pull request was $($parsed.PullRequest)"
    Assert-True ($parsed.Summary -eq 'one device generation contract') "summary was '$($parsed.Summary)'"
}

Test-Case 'a breaking fix files under Changed, not Fixed' {
    # A breaking change filed under Fixed reads as a bugfix a reader can take
    # blind, which is the one thing the section is supposed to prevent.
    $parsed = ConvertFrom-CommitSubject -Subject 'fix(config)!: reset the stored preset schema'
    Assert-True ($parsed.Section -eq 'Changed') "section was $($parsed.Section)"
}

Test-Case 'a no-entry type parses but produces no section' {
    $parsed = ConvertFrom-CommitSubject -Subject 'ci: pin the runner image'
    Assert-True $parsed.Valid "expected valid, got: $($parsed.Problem)"
    Assert-True ($null -eq $parsed.Section) "section was $($parsed.Section)"
}

Test-Case 'an unknown type is rejected and names the known ones' {
    $parsed = ConvertFrom-CommitSubject -Subject 'engine: selectable WGC capture backend'
    Assert-True (-not $parsed.Valid) 'an unknown type was accepted'
    Assert-True ($parsed.Problem -match 'not a known type') "problem was '$($parsed.Problem)'"
    Assert-True ($parsed.Problem -match 'refactor') 'the known types were not listed'
}

Test-Case 'a subject with no type at all is rejected' {
    $parsed = ConvertFrom-CommitSubject -Subject 'Enforce release promotion integrity'
    Assert-True (-not $parsed.Valid) 'a typeless subject was accepted'
}

Test-Case 'a subject that is only a type and a pull request number is rejected' {
    # The grammar's own '.+' already refuses a trailing-whitespace summary, because
    # the subject is trimmed before it is matched. The summary-is-empty branch is
    # reachable through the optional pull request group alone, and this is the
    # case that reaches it -- an earlier version of this test used 'fix(engine): '
    # and passed against a parser with that branch removed.
    $parsed = ConvertFrom-CommitSubject -Subject 'fix:   (#12)'
    Assert-True (-not $parsed.Valid) 'a subject with no summary was accepted'
    Assert-True ($parsed.Problem -match 'summary is empty') "problem was '$($parsed.Problem)'"

    $trailing = ConvertFrom-CommitSubject -Subject 'fix(engine): '
    Assert-True (-not $trailing.Valid) 'a subject with nothing after the colon was accepted'
}

Test-Case 'the pull request number is required only when asked for' {
    $without = ConvertFrom-CommitSubject -Subject 'fix: bound the capture drains'
    Assert-True $without.Valid 'a local commit was required to carry a pull request number'
    $required = ConvertFrom-CommitSubject -Subject 'fix: bound the capture drains' -RequirePullRequest
    Assert-True (-not $required.Valid) 'a merge-time subject passed without its pull request number'
}

Test-Case 'a title carrying its own number is rejected, another number is not' {
    $own = ConvertFrom-CommitSubject -Subject 'fix: bound the drains (#390)' -OwnPullRequest 390
    Assert-True (-not $own.Valid) 'a title ending in its own number was accepted'
    Assert-True ($own.Problem -match 'its own pull request number') "the problem did not name the cause: $($own.Problem)"

    $other = ConvertFrom-CommitSubject -Subject 'fix: bound the drains (#370)' -OwnPullRequest 390
    Assert-True $other.Valid "a trailing citation of another pull request was rejected: $($other.Problem)"

    $none = ConvertFrom-CommitSubject -Subject 'fix: bound the drains' -OwnPullRequest 390
    Assert-True $none.Valid "a title with no number at all was rejected: $($none.Problem)"
}

Test-Case 'the merge subject appends the number exactly once' {
    $plain = ConvertFrom-CommitSubject -Subject 'fix(engine): bound the drains'
    Assert-True ((Format-MergeSubject -Commit $plain -PullRequest 390) -eq 'fix(engine): bound the drains (#390)') `
        'a title without a number did not gain exactly one'

    # The title form this rejects at check time, reconstructed anyway: the helper
    # is what makes ' (#390) (#390)' unreachable even if one slipped through.
    $carried = ConvertFrom-CommitSubject -Subject 'fix(engine): bound the drains (#390)'
    Assert-True ((Format-MergeSubject -Commit $carried -PullRequest 390) -eq 'fix(engine): bound the drains (#390)') `
        'a title that already carried its number produced it twice'

    $breaking = ConvertFrom-CommitSubject -Subject 'refactor(engine)!: one device generation contract'
    Assert-True ((Format-MergeSubject -Commit $breaking -PullRequest 391) -eq 'refactor(engine)!: one device generation contract (#391)') `
        'the scope or the breaking marker was lost'

    $cited = ConvertFrom-CommitSubject -Subject 'fix: finish what (#370) started'
    Assert-True ((Format-MergeSubject -Commit $cited -PullRequest 391) -eq 'fix: finish what (#370) started (#391)') `
        'a citation of another pull request did not survive the merge subject'
}

Test-Case 'an entry links its pull request and marks breaking changes' {
    $parsed = ConvertFrom-CommitSubject -Subject 'feat(ui)!: one settings surface (#392)'
    $line = Format-ChangelogEntry -Commit $parsed -RepositoryUrl 'https://example.invalid/r'
    Assert-True ($line -match '^\- \*\*BREAKING: ') "line was '$line'"
    Assert-True ($line -match '\(\[#392\]\(https://example\.invalid/r/pull/392\)\)') "line was '$line'"
}

Test-Case 'a title that already carried its number does not print it twice' {
    $parsed = ConvertFrom-CommitSubject -Subject 'fix: eighteen defects from a source audit (#385) (#385)'
    Assert-True $parsed.Valid 'a doubled pull request number made the subject unreadable'
    Assert-True ($parsed.PullRequest -eq 385) "pull request was '$($parsed.PullRequest)'"
    Assert-True ($parsed.Summary -eq 'eighteen defects from a source audit') "summary was '$($parsed.Summary)'"
    $line = Format-ChangelogEntry -Commit $parsed -RepositoryUrl 'https://example.invalid/r'
    Assert-True ($line -notmatch '\(#385\)\s*\(\[#385\]') "line was '$line'"
}

Test-Case 'a summary citing a different pull request keeps that reference' {
    $parsed = ConvertFrom-CommitSubject -Subject 'fix: finish what (#370) started (#391)'
    Assert-True ($parsed.PullRequest -eq 391) "pull request was '$($parsed.PullRequest)'"
    Assert-True ($parsed.Summary -eq 'finish what (#370) started') "summary was '$($parsed.Summary)'"
}

Write-Host ''
Write-Host 'commit-subject rule'

Test-Case 'a branch of well-formed subjects is accepted' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains'
        Add-FixtureCommit -Root $root -Subject 'test(engine): cover the drain timeout'
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -eq 0) "expected acceptance, got:`n$($result.Output)"
        Assert-True ($result.Output -match '2 commit\(s\) in scope') "scope line missing:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a malformed subject on the branch is rejected' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'made the drains better'
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -ne 0) "expected rejection, got:`n$($result.Output)"
        Assert-True ($result.Output -match 'commit-subject') "the rule did not name itself:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a merge commit on the branch is counted, not judged' {
    # `gh pr update-branch` merges main into the branch and git writes the
    # subject. Judging it fails the branch over a line no author wrote, and
    # `merge-pr.ps1` squashes every pull request, so that line can never reach
    # the changelog. The failure this guards against is worse than one refusal:
    # the range is rescanned on every later commit, so one merge commit locks
    # the branch against all further work.
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains'
        Invoke-IsolatedGit -C $root checkout -q main
        Add-FixtureCommit -Root $root -Subject 'fix(app): unrelated work on main' -Relative 'on-main.txt'
        Invoke-IsolatedGit -C $root checkout -q work
        Invoke-IsolatedGit -C $root merge main --no-ff -m "Merge branch 'main' into work" --quiet
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -eq 0) "a merge commit was judged as an authored subject:`n$($result.Output)"
        Assert-True ($result.Output -match '1 merge commit\(s\) not judged') `
            "the merge commit was dropped silently instead of reported:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a malformed subject is still rejected when a merge commit is present' {
    # The exemption is for merge commits only. A branch that carries both must
    # still fail, or the first case above would have bought acceptance for the
    # whole range.
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'made the drains better'
        Invoke-IsolatedGit -C $root checkout -q main
        Add-FixtureCommit -Root $root -Subject 'fix(app): unrelated work on main' -Relative 'on-main.txt'
        Invoke-IsolatedGit -C $root checkout -q work
        Invoke-IsolatedGit -C $root merge main --no-ff -m "Merge branch 'main' into work" --quiet
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -ne 0) "the malformed subject was excused by the merge commit:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'history before the policy commit is grandfathered' {
    # The whole reason the epoch exists: adopting the rule must not turn every
    # existing branch red, or the rule gets switched off instead of obeyed.
    $root = New-FixtureRepo -WithPolicy -PrePolicySubjects @('Fix gate defects', 'made the drains better')
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'fix(engine): bound the capture drains'
        $result = Invoke-PolicyCheck -Root $root -ExtraArgs @('-Base', 'policy-base~3')
        Assert-True ($result.ExitCode -eq 0) "pre-policy history was reported:`n$($result.Output)"
        Assert-True ($result.Output -match '1 commit\(s\) in scope') "scope was not narrowed:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a repository without the policy commit has nothing in scope' {
    $root = New-FixtureRepo
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Add-FixtureCommit -Root $root -Subject 'made the drains better'
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -eq 0) "expected no scope, got:`n$($result.Output)"
        Assert-True ($result.Output -match 'not committed yet') "the reason was not stated:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a pull request title is accepted without a number and rejected with its own' {
    $root = New-FixtureRepo -WithPolicy
    try {
        $accepted = Invoke-PolicyCheck -Root $root -ExtraArgs @(
            '-Subject', 'fix(engine): bound the drains', '-PullRequestNumber', '390')
        Assert-True ($accepted.ExitCode -eq 0) "a title without its number was rejected:`n$($accepted.Output)"

        $rejected = Invoke-PolicyCheck -Root $root -ExtraArgs @(
            '-Subject', 'fix(engine): bound the drains (#390)', '-PullRequestNumber', '390')
        Assert-True ($rejected.ExitCode -ne 0) "a title carrying its own number was accepted:`n$($rejected.Output)"
        Assert-True ($rejected.Output -match 'its own pull request number') "the reason was not stated:`n$($rejected.Output)"

        # The number is what the squash merge appends. A title citing ANOTHER
        # pull request at the end is a different statement and has to survive.
        $cited = Invoke-PolicyCheck -Root $root -ExtraArgs @(
            '-Subject', 'fix(engine): finish what (#370) started', '-PullRequestNumber', '390')
        Assert-True ($cited.ExitCode -eq 0) "a citation of another pull request was rejected:`n$($cited.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a merged subject still has to carry exactly one number' {
    $root = New-FixtureRepo -WithPolicy
    try {
        $accepted = Invoke-PolicyCheck -Root $root -ExtraArgs @(
            '-Subject', 'fix(engine): bound the drains (#390)', '-RequirePullRequest')
        Assert-True ($accepted.ExitCode -eq 0) "a well-formed merged subject was rejected:`n$($accepted.Output)"

        $rejected = Invoke-PolicyCheck -Root $root -ExtraArgs @(
            '-Subject', 'fix(engine): bound the drains', '-RequirePullRequest')
        Assert-True ($rejected.ExitCode -ne 0) "a merged subject without its number was accepted:`n$($rejected.Output)"
    }
    finally { Remove-Fixture $root }
}

Write-Host ''
Write-Host 'changelog-untouched rule'

Test-Case 'a branch that writes the changelog is rejected' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content "# Changelog`n`n## [Unreleased]`n`n- hand written`n"
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'docs: add a changelog line' --quiet
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -ne 0) "expected rejection, got:`n$($result.Output)"
        Assert-True ($result.Output -match 'changelog-untouched') "the rule did not name itself:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'a declared release cut may write the changelog' {
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content "# Changelog`n`n## [Unreleased]`n`n- cut`n"
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'chore(release): assemble the changelog' --quiet
        $env:EXOSNAP_CHANGELOG_CUT = '1'
        try { $result = Invoke-PolicyCheck -Root $root }
        finally { Remove-Item Env:EXOSNAP_CHANGELOG_CUT -ErrorAction SilentlyContinue }
        Assert-True ($result.ExitCode -eq 0) "a declared cut was rejected:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'the commit that introduces the policy may create the changelog' {
    # The policy commit is also the commit that creates the file the policy is
    # about. Without this the rule could not be adopted without an escape hatch
    # on its own first commit, which is exactly the shape that gets a rule
    # switched off.
    $root = New-FixtureRepo
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content "# Changelog`n`n## [Unreleased]`n"
        Invoke-IsolatedGit -C $root add -A
        Invoke-IsolatedGit -C $root commit -m 'build(policy): open a changelog' --quiet
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -eq 0) "the introducing commit was rejected:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
}

Test-Case 'an uncommitted changelog edit is rejected too' {
    # The hook runs before the commit exists, so a rule that only read committed
    # history would never fire where it is cheapest to obey.
    $root = New-FixtureRepo -WithPolicy
    try {
        Invoke-IsolatedGit -C $root checkout -q -b work main
        Set-FixtureFile -Root $root -Relative 'CHANGELOG.md' -Content "# Changelog`n`n## [Unreleased]`n`n- uncommitted`n"
        $result = Invoke-PolicyCheck -Root $root
        Assert-True ($result.ExitCode -ne 0) "an uncommitted edit passed:`n$($result.Output)"
    }
    finally { Remove-Fixture $root }
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
