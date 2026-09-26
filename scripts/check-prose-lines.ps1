#Requires -Version 7.0
<#
.SYNOPSIS
    Finds prose that was hard-wrapped at a column.

.DESCRIPTION
    Prose is written in long lines. A break goes where a paragraph ends or where
    it carries meaning, never at column 80. Two concrete costs, both already
    paid on this tree: text wrapped at a column renders as a wall wherever it is
    rendered at a different width, and a later edit reflows every following line
    so a one-sentence change arrives as a whole-paragraph diff.

    The rule is about text people read -- Markdown under docs/ and at the
    repository root, pull request and release prose. Code comments follow the
    formatter and are not in scope; exo-dev check source-hygiene owns those.

    What is NOT a violation, and why the check is narrow enough to be blocking:

      - Fenced and indented code blocks, tables, and YAML front matter. A table
        row is not prose and a code line's width is the code's business.
      - A short line. Only a line that both is near the wrap column AND is
        continued by more prose on the next line reads as a hard wrap; a line
        that ends a paragraph is just a line.
      - Headings, list markers on their own, link reference definitions, and
        block quotes of quoted material.

    Default scope is the lines this branch adds or changes. The tree predates
    the rule by tens of thousands of wrapped lines -- AGENTS.md and most of
    docs/ among them -- and a gate that is red on arrival gets switched off
    rather than obeyed. -All is the sweep, for when that backlog is taken on.

    Staged adoption, for the same reason and by the same means as the advisory
    rules in exo-dev check source-hygiene: the gate runs this -Advisory today. Even
    the diff scope is red on this tree, because a branch that adds a paragraph
    to an already-wrapped document wraps it to match, and unwrapping only the
    touched paragraphs would leave a document half one way and half the other.
    Clearing that is a document at a time, deliberately, and the gate blocks
    from the day it is clear -- which is one argument here, not a rewrite.

.PARAMETER RepoRoot
    Repository to check. Defaults to the repository this script lives in.

.PARAMETER Base
    Commit to diff against. Defaults to the merge base with origin/next.

.PARAMETER All
    Check every tracked Markdown file instead of only added lines.

.PARAMETER Advisory
    Report findings without failing. This is how the gate runs it today; see the
    staged-adoption note above.

.PARAMETER Column
    Lines at or above this width count as candidates for a hard wrap. 60 is
    below every common wrap column (72, 79, 80, 100) and above the length of an
    ordinary sentence that simply ends.

.EXAMPLE
    .\scripts\check-prose-lines.ps1

.EXAMPLE
    .\scripts\check-prose-lines.ps1 -All
#>

param(
    [string] $RepoRoot,
    [string] $Base,
    [switch] $All,
    [switch] $Advisory,
    [int] $Column = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

# Templates are rendered into release pages and carry deliberate short lines;
# fixtures contain, by construction, the shapes this rule rejects.
$script:ExcludedPattern = '(?i)^(scripts/tests/|third_party/|\.workspace/)'

function Invoke-Git {
    param([Parameter(Mandatory)] [string[]] $Arguments)
    return & git -C $RepoRoot @Arguments 2>$null
}

function Get-ProseLineKind {
    <#
    .SYNOPSIS
        Classifies every line of a Markdown file as prose or not-prose.
    .DESCRIPTION
        Returns an array parallel to $Lines holding 'prose' or the reason a line
        is exempt. Block state (fences, front matter) makes this a scan over the
        whole file even when only a few lines are in scope: a line cannot be
        judged without knowing whether a fence above it is open.
    #>
    [OutputType([string[]])]
    param([Parameter(Mandatory)] [AllowEmptyString()] [string[]] $Lines)

    $kinds = [string[]]::new($Lines.Count)
    $inFence = $false
    $fenceMarker = $null
    $inFrontMatter = $false

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        $trimmed = $line.TrimStart()

        if ($i -eq 0 -and $trimmed -eq '---') { $inFrontMatter = $true; $kinds[$i] = 'front-matter'; continue }
        if ($inFrontMatter) {
            if ($trimmed -eq '---') { $inFrontMatter = $false }
            $kinds[$i] = 'front-matter'
            continue
        }

        if ($inFence) {
            $kinds[$i] = 'code'
            if ($fenceMarker -and $trimmed.StartsWith($fenceMarker)) { $inFence = $false; $fenceMarker = $null }
            continue
        }
        if ($trimmed -match '^(```|~~~)') {
            $inFence = $true
            $fenceMarker = $trimmed.Substring(0, 3)
            $kinds[$i] = 'code'
            continue
        }

        if (-not $trimmed) { $kinds[$i] = 'blank'; continue }
        # Four leading spaces is an indented code block only outside a list, and
        # telling those apart needs a parser. Exempting both is the fail-open
        # direction for a formatting rule.
        if ($line -match '^(    |\t)') { $kinds[$i] = 'indented'; continue }
        if ($trimmed.StartsWith('|')) { $kinds[$i] = 'table'; continue }
        if ($trimmed.StartsWith('#')) { $kinds[$i] = 'heading'; continue }
        if ($trimmed -match '^\[[^\]]+\]:\s') { $kinds[$i] = 'link-definition'; continue }
        if ($trimmed -match '^(-{3,}|\*{3,}|_{3,})$') { $kinds[$i] = 'rule'; continue }
        if ($trimmed -match '^<') { $kinds[$i] = 'html'; continue }

        $kinds[$i] = 'prose'
    }

    return , $kinds
}

function Get-MarkdownFile {
    $files = @(Invoke-Git @('ls-files', '*.md'))
    return @($files | Where-Object { $_ -and $_ -notmatch $script:ExcludedPattern })
}

function Get-ChangedLine {
    <#
    .SYNOPSIS
        Added line numbers per Markdown file, as a path -> int[] map.
    #>
    param([Parameter(Mandatory)] [string] $BaseRef)

    $map = @{}
    $current = $null
    $lineNumber = 0
    foreach ($line in @(Invoke-Git @('diff', '--unified=0', "$BaseRef...HEAD", '--', '*.md'))) {
        if ($line -match '^\+\+\+ b/(.+)$') {
            $current = $Matches[1]
            if ($current -match $script:ExcludedPattern) { $current = $null }
            elseif (-not $map.ContainsKey($current)) { $map[$current] = [System.Collections.Generic.List[int]]::new() }
            continue
        }
        if ($line -match '^@@ -[0-9,]+ \+([0-9]+)') { $lineNumber = [int]$Matches[1]; continue }
        if ($null -eq $current) { continue }
        if ($line.StartsWith('+') -and -not $line.StartsWith('+++')) {
            [void]$map[$current].Add($lineNumber)
            $lineNumber++
        }
    }
    return $map
}

$violations = [System.Collections.Generic.List[string]]::new()
$scanned = 0

if ($All) {
    $targets = @{}
    foreach ($path in Get-MarkdownFile) { $targets[$path] = $null }
}
else {
    $baseRef = $Base
    if (-not $baseRef) {
        $mergeBase = $null
        foreach ($candidate in @('origin/next', 'origin/main', 'next', 'main')) {
            $mergeBase = (Invoke-Git @('merge-base', 'HEAD', $candidate))
            if ($LASTEXITCODE -eq 0 -and $mergeBase) { break }
        }
        $baseRef = if ($mergeBase) { $mergeBase.Trim() } else { $null }
    }
    if (-not $baseRef) {
        Write-Host 'prose lines: no base to diff against; nothing in scope.'
        exit 0
    }
    $targets = Get-ChangedLine -BaseRef $baseRef
}

foreach ($path in ($targets.Keys | Sort-Object)) {
    $full = Join-Path $RepoRoot $path
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
    $lines = @((Get-Content -LiteralPath $full -Raw) -split "`r?`n")
    $kinds = Get-ProseLineKind -Lines $lines
    $scanned++

    $inScope = if ($null -eq $targets[$path]) { 1..$lines.Count } else { $targets[$path] }
    foreach ($number in $inScope) {
        $index = $number - 1
        if ($index -lt 0 -or $index -ge $lines.Count) { continue }
        if ($kinds[$index] -ne 'prose') { continue }

        $line = $lines[$index]
        if ($line.Length -lt $Column) { continue }

        # The decisive half: a long line followed by more prose was wrapped. A
        # long line that ends its paragraph is a long line, which is the point.
        $next = $index + 1
        if ($next -ge $lines.Count) { continue }
        if ($kinds[$next] -ne 'prose') { continue }

        # A list item starting on the next line is a new item, not a
        # continuation, and the same holds for a block quote marker.
        $nextTrimmed = $lines[$next].TrimStart()
        if ($nextTrimmed -match '^([-*+]\s|[0-9]+[.)]\s|>)') { continue }

        [void]$violations.Add("${path}:${number}: prose hard-wrapped at column $($line.Length); join the paragraph into one line")
    }
}

Write-Host "prose lines: $scanned file(s) in scope"

if ($violations.Count -gt 0) {
    Write-Host ''
    $label = if ($Advisory) { 'ADVISORY' } else { 'FAIL' }
    foreach ($violation in $violations) { Write-Host "$label  $violation" }
    Write-Host ''
    Write-Host "$($violations.Count) finding(s). CONTRIBUTING.md explains why prose is written in long lines."
    if (-not $Advisory) { exit 1 }
    Write-Host 'Advisory: not failing the run. Clearing a document is a change of its own.'
    exit 0
}

Write-Host 'prose lines: OK'
exit 0
