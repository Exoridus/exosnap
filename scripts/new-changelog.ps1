#Requires -Version 7.0
<#
.SYNOPSIS
    Assembles the changelog section for everything merged since the last release.

.DESCRIPTION
    The changelog is written here and nowhere else. A pull request that edits
    CHANGELOG.md by hand conflicts with every other pull request that did, which
    is why `check-commit-policy.ps1` fails one that tries -- and why this script
    has to be the easier path, not merely the sanctioned one.

    What it reads is the first-parent history since the last version tag: on a
    squash-merging repository that is exactly one commit per merged pull
    request, with the pull request title as the subject and its number appended.
    The Conventional Commits type files the entry, `!` marks it breaking, and
    `ci`, `build`, `test`, `chore` and `style` produce no entry at all.

    A subject the parser cannot read is REPORTED, never skipped. A changelog
    assembled from a history it silently dropped half of is worse than no
    changelog: a reader cannot tell an empty section from an unparsed one.
    Anything merged before the policy took effect is listed separately as
    grandfathered, for the same reason.

.PARAMETER RepoRoot
    Repository to read. Defaults to the repository this script lives in.

.PARAMETER Since
    Read from this ref instead of the last version tag.

.PARAMETER Until
    Read up to this ref. Defaults to origin/main, falling back to HEAD.

.PARAMETER Version
    Render the section as a released version, headed 'x.y.z - date', instead of
    'Unreleased'.

.PARAMETER Date
    The release date for -Version. Defaults to today, UTC.

.PARAMETER Apply
    Write the rendered section into CHANGELOG.md instead of printing it. With
    -Version the Unreleased section is replaced by the release section and a new
    empty Unreleased is opened above it.

.EXAMPLE
    .\scripts\new-changelog.ps1

.EXAMPLE
    .\scripts\new-changelog.ps1 -Version 0.9.1 -Apply
#>

param(
    [string] $RepoRoot,
    [string] $Since,
    [string] $Until,
    [string] $Version,
    [string] $Date,
    [switch] $Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'lib/CommitPolicy.psm1') -Force

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

if ($Version -and $Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version '$Version' is not x.y.z."
}

function Invoke-Git {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    return & git -C $RepoRoot @Arguments 2>$null
}

function Resolve-Since {
    if ($Since) { return $Since }
    # --merged, not --sort=-creatordate alone: a tag on a branch that never
    # landed would otherwise cut the range short and hide everything merged
    # since the last tag that actually is an ancestor.
    #
    # Released versions only. A release candidate is a prerelease of the very
    # version being assembled, and git's version sort ranks it ABOVE the release
    # it precedes, so an unfiltered list hands back the newest RC: cutting 0.9.1
    # against v0.9.1-rc5 would describe the two commits merged after that tag and
    # silently drop the release it is the changelog of.
    $tags = @(Invoke-Git @('tag', '--list', 'v*', '--merged', 'HEAD', '--sort=-version:refname') |
        Where-Object { $_ -and $_.Trim() -notmatch '-' })
    if ($tags.Count -gt 0 -and $tags[0]) { return $tags[0].Trim() }
    return $null
}

function Resolve-Until {
    if ($Until) { return $Until }
    $null = Invoke-Git @('rev-parse', '--verify', '--quiet', 'origin/main')
    if ($LASTEXITCODE -eq 0) { return 'origin/main' }
    return 'HEAD'
}

$sinceRef = Resolve-Since
$untilRef = Resolve-Until
$range = if ($sinceRef) { "$sinceRef..$untilRef" } else { $untilRef }

# --first-parent: on a squash-merging repository the first-parent line is one
# commit per merged pull request. Without it a branch that was merged rather
# than squashed contributes every commit it ever had.
$subjects = @(Invoke-Git @('log', '--first-parent', '--format=%H%x1f%s', $range))

$epochAdds = @(Invoke-Git @('log', '--reverse', '--diff-filter=A', '--format=%H', '--', 'scripts/lib/CommitPolicy.psm1'))
$epoch = if ($epochAdds.Count -gt 0) { $epochAdds[0] } else { $null }
$grandfathered = @()
if ($epoch) {
    # Everything reachable from the epoch's parent predates the policy. Listing
    # them as unreadable subjects would be an accusation against history that
    # was written under different rules.
    $grandfathered = @(Invoke-Git @('rev-list', "$epoch^"))
}

$sections = [ordered]@{}
foreach ($name in (Get-CommitPolicySectionOrder)) { $sections[$name] = [System.Collections.Generic.List[string]]::new() }

$unreadable = [System.Collections.Generic.List[string]]::new()
$skipped = 0
$older = 0

foreach ($line in $subjects) {
    if (-not $line) { continue }
    $hash, $subject = $line -split "`u{001f}", 2
    if ($null -eq $subject) { $subject = '' }

    $parsed = ConvertFrom-CommitSubject -Subject $subject
    if (-not $parsed.Valid) {
        if ($grandfathered -contains $hash) { $older++; continue }
        [void]$unreadable.Add("$($hash.Substring(0, 8))  $subject  -- $($parsed.Problem)")
        continue
    }
    if (-not $parsed.Section) { $skipped++; continue }
    [void]$sections[$parsed.Section].Add((Format-ChangelogEntry -Commit $parsed))
}

$heading = if ($Version) {
    $when = if ($Date) { $Date } else { [DateTime]::UtcNow.ToString('yyyy-MM-dd') }
    "## [$Version] - $when"
}
else { '## [Unreleased]' }

$rendered = [System.Collections.Generic.List[string]]::new()
[void]$rendered.Add($heading)
foreach ($name in $sections.Keys) {
    $entries = $sections[$name]
    if ($entries.Count -eq 0) { continue }
    [void]$rendered.Add('')
    [void]$rendered.Add("### $name")
    [void]$rendered.Add('')
    foreach ($entry in $entries) { [void]$rendered.Add($entry) }
}

$entryCount = ($sections.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum

Write-Host "range     $range"
Write-Host "entries   $entryCount in $(@($sections.Keys | Where-Object { $sections[$_].Count -gt 0 }).Count) section(s)"
Write-Host "no entry  $skipped (ci/build/test/chore/style)"
if ($older -gt 0) { Write-Host "older     $older merged before the policy took effect" }

if ($unreadable.Count -gt 0) {
    Write-Host ''
    Write-Host "$($unreadable.Count) subject(s) the cut cannot file:"
    foreach ($item in $unreadable) { Write-Host "  $item" }
    Write-Host ''
    Write-Host 'Fix the subject on the merged commit, or file the entry by hand and say why in the pull request.'
}

if (-not $Apply) {
    Write-Host ''
    $rendered | ForEach-Object { Write-Host $_ }
    if ($unreadable.Count -gt 0) { exit 1 }
    exit 0
}

$changelogPath = Join-Path $RepoRoot 'CHANGELOG.md'
if (-not (Test-Path -LiteralPath $changelogPath -PathType Leaf)) {
    throw "$changelogPath does not exist."
}

$existing = Get-Content -LiteralPath $changelogPath -Raw
if ($existing -notmatch '(?m)^## \[Unreleased\]') {
    throw "$changelogPath has no '## [Unreleased]' section to write into."
}

$lines = $existing -split "`r?`n"
$startIndex = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^## \[Unreleased\]') { $startIndex = $i; break }
}
$endIndex = $lines.Count
for ($i = $startIndex + 1; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^## \[') { $endIndex = $i; break }
}

$head = if ($startIndex -gt 0) { $lines[0..($startIndex - 1)] } else { @() }
$tail = if ($endIndex -lt $lines.Count) { $lines[$endIndex..($lines.Count - 1)] } else { @() }

$body = [System.Collections.Generic.List[string]]::new()
if ($Version) {
    # A release cut closes Unreleased and opens an empty one above it, so the
    # next merge has somewhere to go without anyone editing the file's shape.
    [void]$body.Add('## [Unreleased]')
    [void]$body.Add('')
}
foreach ($line in $rendered) { [void]$body.Add($line) }
[void]$body.Add('')

$updated = (@($head) + @($body) + @($tail)) -join "`n"
# LF: .gitattributes pins this tree to eol=lf, and Set-Content -Raw would
# otherwise leave whatever the split produced.
[IO.File]::WriteAllText($changelogPath, $updated.TrimEnd("`n") + "`n", [Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host "wrote     $changelogPath"
if ($unreadable.Count -gt 0) { exit 1 }
exit 0
