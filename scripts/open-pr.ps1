#Requires -Version 7.0
<#
.SYNOPSIS
    Opens the pull request for the current branch with a title the release cut
    can file.

.DESCRIPTION
    The canonical way a pull request is created in this repository. A title
    assembled by hand at the command line is how a malformed subject reaches
    GitHub, and correcting it afterwards is an edit of live pull request
    metadata -- which is exactly what this script exists to make unnecessary.

    The title is the Conventional Commits subject with NO trailing pull request
    number: the number does not exist yet, and the squash merge appends it
    (`merge-pr.ps1`). The title is validated against the same parser the
    changelog cut reads, before the pull request is created.

    The pull request is created as a draft and marked ready only after its
    stored metadata has been read back and re-validated. A draft does not start
    the heavy Windows legs, so a title this script somehow got wrong costs
    nothing but the draft.

    Nothing here merges anything. Merging is `merge-pr.ps1`, and it requires an
    explicit approval of that specific merge.

.PARAMETER Subject
    The pull request title. Defaults to the subject of the newest commit on this
    branch that is not on the base.

.PARAMETER Body
    The pull request description. Defaults to the body file when one is given,
    otherwise to a placeholder the author is expected to replace.

.PARAMETER BodyFile
    Read the description from this file instead.

.PARAMETER Base
    Base branch. Defaults to main.

.PARAMETER KeepDraft
    Leave the pull request in draft instead of marking it ready for review.

.PARAMETER NoPush
    Do not push the branch first. Fails if the branch has no upstream.

.EXAMPLE
    .\scripts\open-pr.ps1 -BodyFile pr-body.md

.EXAMPLE
    .\scripts\open-pr.ps1 -Subject 'fix(ci): stop reruns on a title edit' -Body 'Splits the title check out of ci.yml.'
#>

param(
    [string] $Subject,
    [string] $Body,
    [string] $BodyFile,
    [string] $Base = 'main',
    [switch] $KeepDraft,
    [switch] $NoPush
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/CommitPolicy.psm1') -Force

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Invoke-Git {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    $output = & git -C $repoRoot @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git $($Arguments -join ' ') failed with exit $LASTEXITCODE" }
    return $output
}

function Invoke-Gh {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "gh $($Arguments -join ' ') failed with exit $LASTEXITCODE" }
    return $output
}

$branch = (Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')).Trim()
if ($branch -eq $Base -or $branch -eq 'HEAD') {
    throw "refusing to open a pull request from '$branch'; check out a topic branch first"
}

if (-not $Subject) {
    # The newest commit the branch adds, not HEAD unconditionally: on a branch
    # that has already been rebased onto a newer base, HEAD is still the branch's
    # own tip, but on a merge commit it would not be.
    $mergeBase = (Invoke-Git @('merge-base', 'HEAD', "origin/$Base")).Trim()
    $subjects = @(Invoke-Git @('log', '--format=%s', "$mergeBase..HEAD"))
    if ($subjects.Count -eq 0) { throw "this branch adds no commit over origin/$Base" }
    $Subject = $subjects[0]
}

$parsed = ConvertFrom-CommitSubject -Subject $Subject
if (-not $parsed.Valid) {
    throw "the title '$Subject' -- $($parsed.Problem). CONTRIBUTING.md has the rules."
}
if ($parsed.PullRequest) {
    # It cannot be the own number yet -- there is no pull request -- so this is a
    # citation, and a citation at the END of the title is indistinguishable from
    # the number the squash merge is about to append.
    throw "the title ends in ' (#$($parsed.PullRequest))'. The squash merge appends the pull request number; a citation of another pull request belongs inside the summary, not at the end."
}

if ($BodyFile) {
    $bodyPath = (Resolve-Path -LiteralPath $BodyFile).Path
    $Body = Get-Content -LiteralPath $bodyPath -Raw
}
if (-not $Body) {
    $Body = "What changed, why, and what validated it.`n`nReplace this before marking the pull request ready."
}

if (-not $NoPush) {
    Invoke-Git @('push', '--set-upstream', 'origin', $branch) | Write-Host
}

$bodyTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("exosnap-pr-body-" + [guid]::NewGuid().ToString('n') + '.md')
Set-Content -LiteralPath $bodyTemp -Value $Body -NoNewline -Encoding utf8
try {
    Invoke-Gh @('pr', 'create', '--repo', 'Exoridus/exosnap', '--base', $Base, '--head', $branch,
        '--title', $Subject, '--body-file', $bodyTemp, '--draft') | Write-Host
}
finally {
    Remove-Item -LiteralPath $bodyTemp -ErrorAction SilentlyContinue
}

# Read the stored title back rather than trusting what was sent: GitHub is what
# the squash merge and the changelog will read, and a title that arrived altered
# (or a pull request that attached to the wrong head) has to be found here, while
# it is still a draft.
$view = Invoke-Gh @('pr', 'view', $branch, '--repo', 'Exoridus/exosnap', '--json', 'number,title,isDraft,headRefName,baseRefName') | ConvertFrom-Json

$problems = [System.Collections.Generic.List[string]]::new()
if ($view.title -ne $Subject) { [void]$problems.Add("stored title '$($view.title)' is not the title that was sent") }
if ($view.headRefName -ne $branch) { [void]$problems.Add("head is '$($view.headRefName)', not '$branch'") }
if ($view.baseRefName -ne $Base) { [void]$problems.Add("base is '$($view.baseRefName)', not '$Base'") }

$stored = ConvertFrom-CommitSubject -Subject $view.title -OwnPullRequest $view.number
if (-not $stored.Valid) { [void]$problems.Add("stored title -- $($stored.Problem)") }

if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Write-Host "FAIL  $problem" }
    throw "pull request #$($view.number) was created but its metadata is wrong; it is still a draft. Fix it, then re-run with -NoPush."
}

Write-Host "  #$($view.number)  $($view.title)"
Write-Host "  files under $(if ($stored.Section) { $stored.Section } else { 'no changelog section' })"

if (-not $KeepDraft) {
    Invoke-Gh @('pr', 'ready', "$($view.number)", '--repo', 'Exoridus/exosnap') | Write-Host
    Write-Host "pull request #$($view.number) is ready for review"
}
else {
    Write-Host "pull request #$($view.number) left in draft"
}
