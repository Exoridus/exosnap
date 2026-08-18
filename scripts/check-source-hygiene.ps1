#Requires -Version 7.0
<#
.SYNOPSIS
    Finds development provenance in source comments and doc comments.

.DESCRIPTION
    The policy this enforces is one paragraph long and lives in CLAUDE.md: a
    comment carries the durable technical reason a change is the way it is, never
    the history of how it was arrived at. This script is the mechanical half --
    the part that does not need judgement and should not cost prompt space.

    It reads COMMENTS ONLY. A task identifier in a string literal (a diagnostic
    ID, a rack position, a test name) is data and is left alone; the same token
    inside a comment is a pointer into a tracker that outlives nothing.

    Default scope is the lines this branch adds or changes, so the rules can
    block immediately without first requiring a sweep of everything written
    before them. -All scans every tracked source file.

.PARAMETER RepoRoot
    Repository to scan. Defaults to the repository this script lives in.

.PARAMETER Base
    Commit to diff against. Defaults to the merge base with origin/main.

.PARAMETER All
    Scan every tracked source file instead of only added/changed lines.

.PARAMETER Only
    Restrict the run to the named rules.

    Named -Only rather than -Rule on purpose: PowerShell variable names are
    case-insensitive, so a [string[]] parameter $Rule and a loop variable $rule
    are the SAME variable, and the loop would silently coerce each rule object to
    a string.

.EXAMPLE
    .\scripts\check-source-hygiene.ps1

.EXAMPLE
    .\scripts\check-source-hygiene.ps1 -All
#>

param(
    [string] $RepoRoot,
    [string] $Base,
    [switch] $All,
    [string[]] $Only = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

# ---------------------------------------------------------------------------
# Repository-specific vocabulary
#
# The generic "\b[A-Z]{1,3}-[0-9]{1,3}\b looks like a ticket" heuristic is wrong
# here: this repository legitimately writes SHA-256, UTF-8, CRC-32, BSD-3 and,
# more importantly, ships diagnostic identifiers of exactly that shape as product
# data (ART-001, ENV-001, REC-001, DSK-02, ...). Naming the tracker prefixes is
# both more precise and cheaper than describing a pattern and then excluding half
# of what it matches.
# ---------------------------------------------------------------------------

$script:TrackerPrefixes = @('QCR', 'BUG', 'TC', 'VR', 'TASK', 'TICKET', 'ISSUE', 'JIRA')

# Staged adoption. Every rule reports; these do not yet fail the run. task-id and
# non-ascii-punctuation are here because the tree they were written for already
# contains 222 tracker references and 512 non-ASCII dashes: a gate that is red on
# arrival gets switched off rather than obeyed, so they stay advisory until the
# backlog is cleared. user-request-phrasing is here for the opposite reason, given
# with the rule itself: it is imprecise by nature, not by backlog. The precise
# rules -- the ones measured at a handful of hits -- block from the start.
$script:AdvisoryRules = @('task-id', 'non-ascii-punctuation', 'user-request-phrasing')

# Test fixtures are excluded for the same reason check-drift.ps1 excludes them: a
# rule with no rejected fixture has never been shown to reject anything, so the
# fixtures contain, by construction, every shape the rules reject. Scanning them
# reports the evidence as the offence -- which is exactly what happened on this
# scanner's own first commit.
$script:ExcludedPattern = '(?i)^scripts/tests/'

# ---------------------------------------------------------------------------
# Comment extraction
# ---------------------------------------------------------------------------

$script:CommentSyntax = @(
    @{ Extensions = @('.cpp', '.cc', '.cxx', '.h', '.hpp', '.hxx', '.inl', '.qml', '.js', '.mjs', '.ts')
        Line      = '//'; BlockOpen = '/*'; BlockClose = '*/' }
    @{ Extensions = @('.ps1', '.psm1', '.psd1')
        Line      = '#'; BlockOpen = '<#'; BlockClose = '#>' }
    @{ Extensions = @('.cmake', '.py', '.yml', '.yaml', '.toml', '.cfg')
        Line      = '#'; BlockOpen = $null; BlockClose = $null }
    @{ Names      = @('CMakeLists.txt')
        Line      = '#'; BlockOpen = $null; BlockClose = $null }
)

function Get-CommentSyntax {
    param([Parameter(Mandatory)] [string] $Path)
    $name = Split-Path -Leaf $Path
    $extension = [IO.Path]::GetExtension($name)
    foreach ($syntax in $script:CommentSyntax) {
        if ($syntax.Contains('Names') -and $syntax.Names -contains $name) { return $syntax }
        if ($syntax.Contains('Extensions') -and $extension -and $syntax.Extensions -contains $extension.ToLowerInvariant()) {
            return $syntax
        }
    }
    return $null
}

function Get-CommentRegion {
    <#
    .SYNOPSIS
        The comment text of a file, as a line number -> comment text map.
    .DESCRIPTION
        Deliberately a scanner, not a parser. It tracks block-comment state and
        skips string literals well enough that a URL in code does not read as a
        line comment; it does not attempt to be correct for raw string literals
        containing comment openers, which would fail closed (a missed comment)
        rather than open (a false report).
    #>
    [OutputType([hashtable])]
    param(
        # AllowEmptyString: a file whose only line is blank arrives as @('') and
        # a plain mandatory [string[]] rejects it.
        [Parameter(Mandatory)] [AllowEmptyString()] [string[]] $Lines,
        [Parameter(Mandatory)] [hashtable] $Syntax
    )

    $regions = @{}
    $inBlock = $false

    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        $number = $i + 1
        $comment = ''
        $position = 0

        while ($position -lt $line.Length) {
            if ($inBlock) {
                $close = if ($Syntax.BlockClose) { $line.IndexOf($Syntax.BlockClose, $position) } else { -1 }
                if ($close -lt 0) {
                    $comment += $line.Substring($position)
                    $position = $line.Length
                }
                else {
                    $comment += $line.Substring($position, $close - $position)
                    $position = $close + $Syntax.BlockClose.Length
                    $inBlock = $false
                }
                continue
            }

            $char = $line[$position]

            # Skip over string literals so their contents are never treated as
            # comment text -- a task identifier in a string is data.
            if ($char -eq '"' -or $char -eq "'") {
                $quote = $char
                $position++
                while ($position -lt $line.Length) {
                    if ($line[$position] -eq '\' -and $quote -eq '"') { $position += 2; continue }
                    if ($line[$position] -eq $quote) { $position++; break }
                    $position++
                }
                continue
            }

            if ($Syntax.Line -and $position + $Syntax.Line.Length -le $line.Length -and
                $line.Substring($position, $Syntax.Line.Length) -eq $Syntax.Line) {
                $comment += $line.Substring($position)
                break
            }

            if ($Syntax.BlockOpen -and $position + $Syntax.BlockOpen.Length -le $line.Length -and
                $line.Substring($position, $Syntax.BlockOpen.Length) -eq $Syntax.BlockOpen) {
                $position += $Syntax.BlockOpen.Length
                $inBlock = $true
                continue
            }

            $position++
        }

        if ($comment.Trim()) { $regions[$number] = $comment }
    }

    return $regions
}

# ---------------------------------------------------------------------------
# Rules
# ---------------------------------------------------------------------------

function Get-RepoBranchName {
    param([string] $Root)
    $names = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @(& git -C $Root for-each-ref --format='%(refname:short)' refs/heads refs/remotes 2>$null)) {
        if (-not $name) { continue }
        $short = $name -replace '^origin/', ''
        # main and master name themselves in prose constantly; a branch has to be
        # distinctive before its appearance in a comment means anything.
        if ($short -in @('main', 'master', 'HEAD')) { continue }
        if ($short.Length -lt 6) { continue }
        [void]$names.Add([regex]::Escape($short))
    }
    return , @($names | Sort-Object -Unique)
}

function Test-CommitHash {
    <#
    .SYNOPSIS
        True when the candidate names an object in this repository.
    .DESCRIPTION
        Regex alone cannot tell a short hash from an ordinary hex-looking word
        ("defaced" is seven characters of [a-f]). Asking git removes the guess:
        the rule fires on values that really are commits of this repository,
        which is exactly the provenance worth removing.
    #>
    param([string] $Root, [string] $Candidate)
    $null = & git -C $Root rev-parse --verify --quiet "$Candidate^{commit}" 2>$null
    return $LASTEXITCODE -eq 0
}

function New-HygieneRule {
    param([string] $Name, [string] $Pattern, [string] $Fix, [scriptblock] $Confirm)
    # [pscustomobject], not [ordered]@{}: @(...) around a dictionary enumerates
    # its ENTRIES, so a rule list would arrive at the caller as loose key/value
    # pairs with no .Pattern on them.
    return [pscustomobject]@{ Name = $Name; Pattern = $Pattern; Fix = $Fix; Confirm = $Confirm }
}

function Get-HygieneRule {
    param([Parameter(Mandatory)] [string] $Root)

    $trackers = ($script:TrackerPrefixes | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $branches = Get-RepoBranchName -Root $Root

    $rules = [System.Collections.Generic.List[object]]::new()

    $rules.Add((New-HygieneRule 'task-id' "\b(?:$trackers)-\d{1,4}\b" `
                'Remove the tracker reference; keep the technical reason it produced.'))

    # GetNewClosure: the confirmation runs later, outside this function's scope,
    # so $Root has to travel with the scriptblock.
    $confirmHash = { param($value) Test-CommitHash -Root $Root -Candidate $value }.GetNewClosure()
    $rules.Add((New-HygieneRule 'commit-hash' '\b[0-9a-fA-F]{7,40}\b' `
                'Remove the commit reference; a comment outlives the history it points at.' `
                $confirmHash))

    $rules.Add((New-HygieneRule 'issue-reference' '(?:^|[\s([])#\d+\b' `
                'Remove the issue or PR number; state the constraint it describes instead.'))

    $rules.Add((New-HygieneRule 'private-workspace' '(?:^|[\s(\\/])\.workspace(?:[\\/]|\b)' `
                '.workspace/ is untracked planning context; committed source must not point at it.'))

    $rules.Add((New-HygieneRule 'absolute-path' '\b[A-Za-z]:[\\/]' `
                'Remove the machine-specific path; name the thing, not where it sits on one machine.'))

    # The negative lookahead keeps the Win32 device namespace out of it: \\.\pipe\name
    # and the \\?\ long-path prefix are API surface, not somebody's file server.
    $rules.Add((New-HygieneRule 'unc-path' '\\{2,4}(?!(?:[.?]|pipe)\\+)[^\\\s]+\\' `
                'Remove the network path; it is meaningless outside one network.'))

    # Split in two on purpose. The phrasings below name the exchange itself and
    # cannot describe anything the running product does, so they block.
    # "previous attempt" is first-person only: "the previous attempt's failure" is
    # retry state, not provenance.
    $rules.Add((New-HygieneRule 'conversation-provenance' `
                '\b(?:user (?:said|decided|mentioned)|as (?:discussed|requested)|per (?:our|the) (?:discussion|conversation)|(?:[Mm]y|[Oo]ur) previous attempt|earlier session)\b' `
                'Drop how the decision was reached; keep what the decision is.'))

    # "the user asked for X" is the plainest way to write provenance AND the
    # plainest way to describe what an end user of a recording application did.
    # Comments are matched a line at a time, so sentence position cannot separate
    # the two -- wrapping puts either one at the start of a line. Every hit on the
    # tree this rule was written against was product behaviour, so blocking on it
    # would spend the gate's credibility on nothing. It reports for a human to
    # read and never fails the run.
    $rules.Add((New-HygieneRule 'user-request-phrasing' `
                '\buser (?:asked|requested|wanted)\b' `
                'If this records who asked for the change, drop it; if it describes what an end user did, keep it.'))

    $rules.Add((New-HygieneRule 'agent-provenance' `
                '\b(?:Claude|Codex|ChatGPT|Copilot|Gemini)\b\s*(?:Code\s*)?(?:said|suggested|decided|added|implemented|changed|wrote|generated)\b' `
                'Source does not record who wrote it.'))

    if ($branches.Count -gt 0) {
        $rules.Add((New-HygieneRule 'branch-reference' "\b(?:$($branches -join '|'))\b" `
                    'Remove the branch name; branches are deleted, the code is not.'))
    }

    $rules.Add((New-HygieneRule 'non-ascii-punctuation' '[‐-―‘’“”…]' `
                'Developer-facing source documentation uses plain ASCII punctuation.'))

    if ($Only.Count -gt 0) {
        return , @($rules | Where-Object { $Only -contains $_.Name })
    }
    return , @($rules)
}

# ---------------------------------------------------------------------------
# Scope
# ---------------------------------------------------------------------------

function Get-ScannedFile {
    param([string] $Root)
    return , @(& git -C $Root ls-files -- 'app' 'libs' 'apps' 'tools' 'scripts' 'cmake' 'tests' 2>$null |
            Where-Object { $_ -and (Get-CommentSyntax -Path $_) })
}

function Get-ChangedLine {
    <#
    .SYNOPSIS
        Added or changed line numbers per file, from the diff against Base.
    .DESCRIPTION
        Scoping to the diff is what makes these rules adoptable: they block on
        what is being written now without first demanding a sweep of everything
        written before them. -All is the sweep.
    #>
    [OutputType([hashtable])]
    param([string] $Root, [string] $Base)

    $changed = @{}
    $arguments = @('diff', '--unified=0', '--diff-filter=ACMR')
    if ($Base) { $arguments += "$Base...HEAD" }
    $diff = @(& git -C $Root @arguments -- 'app' 'libs' 'apps' 'tools' 'scripts' 'cmake' 'tests' 2>$null)
    $diff += @(& git -C $Root diff --unified=0 --diff-filter=ACMR -- 'app' 'libs' 'apps' 'tools' 'scripts' 'cmake' 'tests' 2>$null)
    $diff += @(& git -C $Root diff --cached --unified=0 --diff-filter=ACMR -- 'app' 'libs' 'apps' 'tools' 'scripts' 'cmake' 'tests' 2>$null)

    $file = $null
    foreach ($line in $diff) {
        if ($line -match '^\+\+\+ b/(.+)$') {
            $file = $Matches[1]
            if (-not $changed.ContainsKey($file)) { $changed[$file] = [System.Collections.Generic.HashSet[int]]::new() }
            continue
        }
        if ($file -and $line -match '^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@') {
            $start = [int]$Matches[1]
            $count = if ($Matches[2]) { [int]$Matches[2] } else { 1 }
            for ($n = $start; $n -lt $start + $count; $n++) { [void]$changed[$file].Add($n) }
        }
    }
    return $changed
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

if (-not $All -and -not $Base) {
    foreach ($candidate in @('origin/main', 'origin/HEAD', 'main')) {
        $merge = (& git -C $RepoRoot merge-base $candidate HEAD 2>$null | Select-Object -First 1)
        if ($merge) { $Base = $merge; break }
    }
}

$rules = Get-HygieneRule -Root $RepoRoot
$changedLines = if ($All) { $null } else { Get-ChangedLine -Root $RepoRoot -Base $Base }
$files = if ($All) { Get-ScannedFile -Root $RepoRoot } else { @($changedLines.Keys | Where-Object { Get-CommentSyntax -Path $_ }) }

$findings = [System.Collections.Generic.List[object]]::new()

foreach ($relative in ($files | Sort-Object)) {
    $path = Join-Path $RepoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }

    if ($relative -match $script:ExcludedPattern) { continue }

    $syntax = Get-CommentSyntax -Path $relative
    if (-not $syntax) { continue }

    $lines = @(Get-Content -LiteralPath $path)
    if ($lines.Count -eq 0 -or -not ($lines -join '').Trim()) { continue }

    $regions = Get-CommentRegion -Lines $lines -Syntax $syntax

    foreach ($number in ($regions.Keys | Sort-Object)) {
        if (-not $All -and -not $changedLines[$relative].Contains($number)) { continue }
        $comment = $regions[$number]

        foreach ($rule in $rules) {
            foreach ($match in [regex]::Matches($comment, $rule.Pattern)) {
                $value = $match.Value.Trim()
                if ($rule.Confirm -and -not (& $rule.Confirm $value)) { continue }
                $findings.Add([pscustomobject]@{
                        File = $relative; Line = $number; Rule = $rule.Name; Value = $value; Fix = $rule.Fix
                    })
                break
            }
        }
    }
}

$blocking = @($findings | Where-Object { $script:AdvisoryRules -notcontains $_.Rule })
$advisory = @($findings | Where-Object { $script:AdvisoryRules -contains $_.Rule })

# One finding, three lines: where, what, what to do. Nothing else -- this output
# is read by an agent on every hook retry, and a policy essay costs tokens each
# time without adding information.
foreach ($finding in $blocking) {
    Write-Host ""
    Write-Host "source-hygiene: $($finding.File):$($finding.Line)"
    Write-Host "$($finding.Rule) in source comment: `"$($finding.Value)`""
    Write-Host $finding.Fix
}

if ($advisory.Count -gt 0) {
    Write-Host ""
    foreach ($group in ($advisory | Group-Object Rule | Sort-Object Count -Descending)) {
        Write-Host ("source-hygiene ADVISORY: {0} x {1} (see: check-source-hygiene.ps1 -Only {1})" -f
            $group.Count, $group.Name)
    }
}

if ($blocking.Count -eq 0) {
    $scope = if ($All) { "$($files.Count) tracked source file(s)" } else { 'the changed lines' }
    Write-Host ""
    Write-Host "source-hygiene: OK ($scope)"
    exit 0
}

Write-Host ""
Write-Host "$($blocking.Count) blocking finding(s)."
exit 1
