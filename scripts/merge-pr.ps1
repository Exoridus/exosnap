#Requires -Version 7.0
<#
.SYNOPSIS
    Squash-merges a pull request with the exact subject the changelog cut reads.

.DESCRIPTION
    The merged subject is `type(scope): summary (#N)` -- the pull request title,
    with its number appended exactly once. GitHub's own append is what produced
    `... (#N) (#N)` on a title that already carried the number; this script
    constructs the subject itself and passes it with `--subject`, so the result
    does not depend on what the title happened to end in.

    The subject is validated as a merged subject (the number IS required there)
    before anything is merged.

.PARAMETER Number
    The pull request to merge. Defaults to the one for the current branch.

.PARAMETER Confirm
    Required. A mechanical safety catch, not authorization: without it the
    script prints the subject and merges nothing, so checking what would land
    cannot merge anything by accident. Passing it is a separate decision that
    belongs to whoever asked for this merge.

.PARAMETER DeleteBranch
    Delete the head branch after the merge.

.PARAMETER Auto
    Enable auto-merge instead of merging now, so the merge waits for the
    required checks. The subject is fixed at this point either way.

.EXAMPLE
    .\scripts\merge-pr.ps1 -Number 400            # prints the subject, merges nothing

.EXAMPLE
    .\scripts\merge-pr.ps1 -Number 400 -Confirm -DeleteBranch
#>

param(
    [int] $Number = 0,
    [switch] $Confirm,
    [switch] $DeleteBranch,
    [switch] $Auto
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/CommitPolicy.psm1') -Force

function Invoke-Gh {
    # AllowEmptyString, because Mandatory otherwise rejects an argument LIST that
    # contains an empty element -- and `gh pr merge --body ""`, which is how the
    # merge body is cleared, is exactly such a list.
    param([Parameter(Mandatory)] [AllowEmptyString()] [string[]] $Arguments)
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "gh $($Arguments -join ' ') failed with exit $LASTEXITCODE" }
    return $output
}

$selector = if ($Number -gt 0) { "$Number" } else { (& git rev-parse --abbrev-ref HEAD).Trim() }
$view = Invoke-Gh @('pr', 'view', $selector, '--repo', 'Exoridus/exosnap',
    '--json', 'number,title,state,isDraft,mergeStateStatus') | ConvertFrom-Json

if ($view.state -ne 'OPEN') { throw "pull request #$($view.number) is $($view.state), not OPEN" }
if ($view.isDraft) { throw "pull request #$($view.number) is a draft" }

$title = ConvertFrom-CommitSubject -Subject $view.title -OwnPullRequest $view.number
if (-not $title.Valid) {
    throw "pull request #$($view.number) title '$($view.title)' -- $($title.Problem). CONTRIBUTING.md has the rules."
}

$subject = Format-MergeSubject -Commit $title -PullRequest $view.number

$merged = ConvertFrom-CommitSubject -Subject $subject -RequirePullRequest
if (-not $merged.Valid) { throw "the constructed subject '$subject' -- $($merged.Problem)" }
if ($merged.PullRequest -ne $view.number) { throw "the constructed subject carries #$($merged.PullRequest), not #$($view.number)" }
if ($merged.Summary -match '\(#\d+\)$') { throw "the constructed subject repeats a pull request number: '$subject'" }

Write-Host "  #$($view.number)  merge state $($view.mergeStateStatus)"
Write-Host "  subject  $subject"
Write-Host "  files under $(if ($merged.Section) { $merged.Section } else { 'no changelog section' })"

if (-not $Confirm) {
    Write-Host ''
    Write-Host 'Nothing merged: -Confirm was not given.'
    exit 0
}

$mergeArgs = @('pr', 'merge', "$($view.number)", '--repo', 'Exoridus/exosnap', '--squash',
    '--subject', $subject, '--body', '')
if ($Auto) { $mergeArgs += '--auto' }
if ($DeleteBranch) { $mergeArgs += '--delete-branch' }

Invoke-Gh $mergeArgs | Write-Host
Write-Host "pull request #$($view.number) $(if ($Auto) { 'queued for auto-merge' } else { 'merged' })"
