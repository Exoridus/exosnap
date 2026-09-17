#Requires -Version 7.0
<#
.SYNOPSIS
    Renders a release's notes from a template and the changelog.

.DESCRIPTION
    The notes used to be a paragraph-long string literal inside
    release-candidate.yml, which is a bad place for text people read: it renders
    as a wall on the releases page, a wording change is a workflow diff, and
    nothing could show what a release would say before the tag was pushed.

    The template is the text; this fills in the identity. The changelog section
    is read out of CHANGELOG.md rather than regenerated, so the releases page
    and the file cannot disagree about what shipped.

    An unresolved placeholder is an error. A release published with a literal
    ${VERSION} in it cannot be edited back into history readers already saw.

.PARAMETER Version
    The full release identity, as in 0.9.1 or 0.9.1-rc3. The changelog section
    is looked up under the x.y.z part.

.PARAMETER Tag
    The git tag. Defaults to 'v' + Version.

.PARAMETER PreviousTag
    The tag the compare link starts at. Defaults to the newest version tag below
    this one that is an ancestor of HEAD.

.PARAMETER Commit
    The commit the artifacts were built from. Defaults to HEAD.

.PARAMETER Candidate
    Render the release-candidate template.

.PARAMETER RepoRoot
    Repository to read. Defaults to the repository this script lives in.

.PARAMETER RepositoryUrl
    Base URL for the compare and releases links.

.PARAMETER OutFile
    Write the notes here instead of to standard output.

.EXAMPLE
    .\scripts\render-release-notes.ps1 -Version 0.9.1

.EXAMPLE
    .\scripts\render-release-notes.ps1 -Version 0.9.1-rc3 -Candidate -OutFile notes.md
#>

param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $Tag,
    [string] $PreviousTag,
    [string] $Commit,
    [switch] $Candidate,
    [string] $RepoRoot,
    [string] $RepositoryUrl = 'https://github.com/Exoridus/exosnap',
    [string] $OutFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

function Invoke-Git {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    return & git -C $RepoRoot @Arguments 2>$null
}

if (-not $Tag) { $Tag = "v$Version" }
if (-not $Commit) {
    $head = (Invoke-Git @('rev-parse', 'HEAD'))
    $Commit = if ($LASTEXITCODE -eq 0 -and $head) { $head.Trim() } else { 'an unknown commit' }
}

if (-not $PreviousTag) {
    # The tag being released is excluded by name: it exists by the time this
    # runs, and a compare link from a tag to itself is empty.
    $tags = @(@(Invoke-Git @('tag', '--list', 'v*', '--merged', 'HEAD', '--sort=-version:refname')) |
        Where-Object { $_ -and $_.Trim() -ne $Tag })
    $PreviousTag = if ($tags.Count -gt 0) { $tags[0].Trim() } else { '' }
}

$productVersion = ($Version -split '-', 2)[0]

function Get-ChangelogSection {
    <#
    .SYNOPSIS
        The body of one version's changelog section, without its heading.
    .DESCRIPTION
        A candidate reads Unreleased, because its changes are by definition not
        cut yet; a final release reads its own section, and falls back to
        Unreleased for the window between the cut's assembly and its heading
        being stamped.
    #>
    param([string] $ProductVersion, [switch] $PreferUnreleased)

    $path = Join-Path $RepoRoot 'CHANGELOG.md'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $lines = @((Get-Content -LiteralPath $path -Raw) -split "`r?`n")

    $wanted = if ($PreferUnreleased) { @('## [Unreleased]') } else { @("## [$ProductVersion]", '## [Unreleased]') }
    foreach ($heading in $wanted) {
        $start = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i].StartsWith($heading)) { $start = $i; break }
        }
        if ($start -lt 0) { continue }
        $end = $lines.Count
        for ($i = $start + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^## \[') { $end = $i; break }
        }
        $body = (($lines[($start + 1)..($end - 1)]) -join "`n").Trim()
        if ($body) { return $body }
    }
    return $null
}

$section = Get-ChangelogSection -ProductVersion $productVersion -PreferUnreleased:$Candidate
if (-not $section) {
    # Not an error: a candidate can legitimately precede any changelog-bearing
    # merge. Saying so is better than an empty heading the reader has to guess at.
    $section = '_No changelog entries were recorded for this release._'
}

$templateName = if ($Candidate) { 'release-notes-candidate.md' } else { 'release-notes.md' }
$templatePath = Join-Path $RepoRoot ".github/templates/$templateName"
if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "Release notes template $templatePath is missing."
}

$values = @{
    VERSION           = $Version
    TAG               = $Tag
    PREVIOUS_TAG      = $PreviousTag
    COMMIT            = $Commit
    REPO_URL          = $RepositoryUrl
    CHANGELOG_SECTION = $section
}

$notes = Get-Content -LiteralPath $templatePath -Raw
foreach ($key in $values.Keys) {
    $notes = $notes.Replace('${' + $key + '}', [string]$values[$key])
}

$unresolved = @([regex]::Matches($notes, '\$\{[A-Z_]+\}') | ForEach-Object { $_.Value } | Sort-Object -Unique)
if ($unresolved.Count -gt 0) {
    throw "Release notes still contain unresolved placeholders: $($unresolved -join ', ')."
}

if (-not $PreviousTag) {
    # The compare link degrades to a link at the repository, which is wrong but
    # harmless; a first release genuinely has nothing to compare against.
    Write-Warning 'No previous version tag was found; the compare link has an empty left side.'
}

if ($OutFile) {
    [IO.File]::WriteAllText($OutFile, $notes, [Text.UTF8Encoding]::new($false))
    Write-Host "wrote $OutFile"
}
else {
    Write-Output $notes
}
