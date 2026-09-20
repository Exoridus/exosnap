#Requires -Version 7.0
<#
.SYNOPSIS
    Checks that this branch's commits can be filed by the release cut, and that
    it leaves CHANGELOG.md alone.

.DESCRIPTION
    Two rules, both about the same thing: the changelog is assembled at the
    release cut from commit subjects, so a subject the assembler cannot read is
    a release-time problem discovered at release time, and a branch that writes
    the changelog by hand conflicts with every other branch that did.

      commit-subject       Every commit since the policy took effect reads as
                           'type(scope): summary'.
      changelog-untouched  No branch writes CHANGELOG.md since the policy took
                           effect. `new-changelog.ps1` does, at the cut.

    Grandfathering is mechanical rather than a date someone maintains: the
    policy takes effect at the commit that added scripts/lib/CommitPolicy.psm1,
    and anything reachable from that commit's parent is out of scope. A tree
    where the module is not committed yet has nothing in scope at all, which is
    the honest answer during the change that introduces it -- not a pass.

    One subject line, three points in its life, and the trailing pull request
    number belongs to exactly one of them:

      local commit         type(scope): summary            no number
      pull request title   type(scope): summary            no number
      merged subject       type(scope): summary (#N)       exactly one number

    The number does not exist until the pull request is opened, and the squash
    merge appends it unconditionally, so requiring it of the title would demand
    a value that has to be filled in after creation -- which is a second edit of
    the title for every pull request, and the reason a title correction used to
    rerun the whole Windows build. `-PullRequestNumber` is the title mode: it
    rejects a title that already carries its own number, and leaves a citation of
    another pull request alone. `-RequirePullRequest` is the merged-subject mode,
    for checking a line that is already on main.

.PARAMETER RepoRoot
    Repository to check. Defaults to the repository this script lives in.

.PARAMETER Base
    Commit to diff and log against. Defaults to the merge base with origin/main.

.PARAMETER Subject
    Check this single subject instead of the branch's commits.

.PARAMETER PullRequestNumber
    The number of the pull request `-Subject` is the title of. Rejects a title
    that already ends in that number.

.PARAMETER RequirePullRequest
    Require `-Subject` to end in a pull request number. The merged-subject mode;
    not for a title.

.PARAMETER Only
    Restrict the run to the named rules.

.EXAMPLE
    .\scripts\check-commit-policy.ps1

.EXAMPLE
    .\scripts\check-commit-policy.ps1 -Subject 'fix(ci): split the title check out' -PullRequestNumber 400
#>

param(
    [string] $RepoRoot,
    [string] $Base,
    [string] $Subject,
    [int] $PullRequestNumber = 0,
    [switch] $RequirePullRequest,
    [string[]] $Only = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/CommitPolicy.psm1') -Force

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

$script:PolicyMarker = 'scripts/lib/CommitPolicy.psm1'
$script:ChangelogPath = 'CHANGELOG.md'

function Invoke-Git {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    return & git -C $RepoRoot @Arguments 2>$null
}

function Get-PolicyEpoch {
    <#
    .SYNOPSIS
        The first commit the policy applies to, or $null when it applies to none.
    .DESCRIPTION
        The commit that ADDED the policy module is the last grandfathered one:
        it is itself written under the rules it introduces, but the history
        behind it was not. --diff-filter=A can report more than one commit when
        a file was added, removed and re-added; the oldest is the epoch, because
        a re-add does not restart a policy.
    #>
    $adds = @(Invoke-Git @('log', '--reverse', '--diff-filter=A', '--format=%H', '--', $script:PolicyMarker))
    if ($adds.Count -eq 0) { return $null }
    return $adds[0]
}

function Resolve-Base {
    if ($Base) { return $Base }
    $mergeBase = (Invoke-Git @('merge-base', 'HEAD', 'origin/main'))
    if ($LASTEXITCODE -eq 0 -and $mergeBase) { return $mergeBase.Trim() }
    $mergeBase = (Invoke-Git @('merge-base', 'HEAD', 'main'))
    if ($LASTEXITCODE -eq 0 -and $mergeBase) { return $mergeBase.Trim() }
    return $null
}

$violations = [System.Collections.Generic.List[string]]::new()
$notes = [System.Collections.Generic.List[string]]::new()

function Test-RuleEnabled {
    param([string] $Name)
    return ($Only.Count -eq 0) -or ($Only -contains $Name)
}

# ---------------------------------------------------------------------------
# commit-subject
# ---------------------------------------------------------------------------

if (Test-RuleEnabled 'commit-subject') {
    if ($Subject) {
        $parsed = ConvertFrom-CommitSubject -Subject $Subject `
            -RequirePullRequest:$RequirePullRequest -OwnPullRequest $PullRequestNumber
        if (-not $parsed.Valid) {
            [void]$violations.Add("commit-subject: '$Subject' -- $($parsed.Problem)")
        }
        else {
            [void]$notes.Add("commit-subject: '$Subject' files under $(if ($parsed.Section) { $parsed.Section } else { 'no changelog section' })")
        }
    }
    else {
        $epoch = Get-PolicyEpoch
        if (-not $epoch) {
            [void]$notes.Add("commit-subject: the policy module is not committed yet; no commit is in scope")
        }
        else {
            $baseRef = Resolve-Base
            if (-not $baseRef) {
                [void]$violations.Add('commit-subject: neither origin/main nor main could be resolved, so the branch range is unknown')
            }
            else {
                # Two ranges intersected: what this branch adds, and what the
                # policy covers. A branch older than the epoch contributes
                # nothing to the first; a branch that predates it contributes
                # nothing to the second.
                $range = @('log', '--format=%H %s', "$baseRef..HEAD", "^$epoch")
                $lines = @(Invoke-Git $range)
                $checked = 0
                foreach ($line in $lines) {
                    if (-not $line) { continue }
                    $hash, $subjectText = $line -split ' ', 2
                    if (-not $subjectText) { $subjectText = '' }
                    $checked++
                    $parsed = ConvertFrom-CommitSubject -Subject $subjectText
                    if (-not $parsed.Valid) {
                        [void]$violations.Add("commit-subject: $($hash.Substring(0, 8)) '$subjectText' -- $($parsed.Problem)")
                    }
                }
                [void]$notes.Add("commit-subject: $checked commit(s) in scope since $($epoch.Substring(0, 8))")
            }
        }
    }
}

# ---------------------------------------------------------------------------
# changelog-untouched
# ---------------------------------------------------------------------------

if (Test-RuleEnabled 'changelog-untouched') {
    $epoch = Get-PolicyEpoch
    if ($env:EXOSNAP_CHANGELOG_CUT -eq '1') {
        [void]$notes.Add('changelog-untouched: skipped, EXOSNAP_CHANGELOG_CUT=1 declares this the release cut')
    }
    elseif ($Subject) {
        [void]$notes.Add('changelog-untouched: not applicable when checking a single subject')
    }
    elseif (-not $epoch) {
        # The same epoch as commit-subject, for the same reason and one more: the
        # commit that introduces the policy is also the one that creates the file
        # the policy is about. Scoping the rule to what came after it is what lets
        # it be adopted without an escape hatch on its own first commit.
        [void]$notes.Add('changelog-untouched: the policy module is not committed yet; no change is in scope')
    }
    else {
        $touched = $false
        $changed = @(Invoke-Git @('diff', '--name-only', "$epoch..HEAD"))
        if ($changed -contains $script:ChangelogPath) { $touched = $true }
        foreach ($range in @(@('diff', '--name-only'), @('diff', '--cached', '--name-only'))) {
            if (@(Invoke-Git $range) -contains $script:ChangelogPath) { $touched = $true }
        }
        if ($touched) {
            [void]$violations.Add("changelog-untouched: this branch writes $script:ChangelogPath. The release cut assembles it from commit subjects (scripts/new-changelog.ps1); set EXOSNAP_CHANGELOG_CUT=1 when that is what this is.")
        }
        else {
            [void]$notes.Add("changelog-untouched: $script:ChangelogPath is unchanged")
        }
    }
}

# ---------------------------------------------------------------------------

foreach ($note in $notes) { Write-Host "  $note" }

if ($violations.Count -gt 0) {
    Write-Host ''
    foreach ($violation in $violations) { Write-Host "FAIL  $violation" }
    Write-Host ''
    Write-Host "$($violations.Count) violation(s). CONTRIBUTING.md has the rules."
    exit 1
}

Write-Host 'commit policy: OK'
exit 0
