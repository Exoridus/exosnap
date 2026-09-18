#Requires -Version 7.0
<#
.SYNOPSIS
    Reads a commit subject the way the changelog and the release notes read it.

.DESCRIPTION
    One parser, three consumers: the commit-policy check that fails a subject the
    release cut could not file, the changelog assembler that files it, and the
    release-notes renderer that prints it. A second parser would be a second
    opinion about what ships in the notes.

    The repository squash-merges with the pull request title alone, so a merged
    commit has no body: everything the cut ever reads has to be in the subject.
    That is why the trailing pull request number is part of the grammar rather
    than a convention, and why a breaking change is marked with `!` -- a
    `BREAKING CHANGE:` footer would have nowhere to survive the squash.

    The types and their changelog sections are the Keep a Changelog mapping;
    `ci`, `build`, `test`, `chore` and `style` deliberately produce no entry,
    because a reader of the changelog is asking what changed for them.
#>

Set-StrictMode -Version Latest

# Section $null means "this type produces no changelog entry". Absence would be
# indistinguishable from an unknown type, which has to be reported, not skipped.
$script:TypeSection = [ordered]@{
    feat     = 'Added'
    fix      = 'Fixed'
    perf     = 'Fixed'
    refactor = 'Changed'
    docs     = 'Documentation'
    ci       = $null
    build    = $null
    test     = $null
    chore    = $null
    style    = $null
}

# The order sections appear in a release. Breaking changes lead because they are
# the only entries a reader has to act on before upgrading.
$script:SectionOrder = @('Changed', 'Added', 'Fixed', 'Documentation')

$script:SubjectPattern = '^(?<type>[a-z]+)(?:\((?<scope>[^()]+)\))?(?<breaking>!)?: (?<summary>.+?)(?: \(#(?<pr>[0-9]+)\))?$'

function Get-CommitPolicyType {
    <#
    .SYNOPSIS
        The accepted Conventional Commits types, in declaration order.
    #>
    [OutputType([string[]])]
    param()
    return , @($script:TypeSection.Keys)
}

function Get-CommitPolicySectionOrder {
    <#
    .SYNOPSIS
        The changelog sections in the order a release renders them.
    #>
    [OutputType([string[]])]
    param()
    return , @($script:SectionOrder)
}

function ConvertFrom-CommitSubject {
    <#
    .SYNOPSIS
        Parses one commit subject into the fields the changelog reads.

    .DESCRIPTION
        Always returns an object. `Valid` says whether the subject matched the
        grammar, and `Problem` says what a contributor has to change; a caller
        that only looks at `Section` would silently drop everything malformed,
        which is the failure this parser exists to prevent.

        `Section` is $null both for a type that produces no entry and for a
        subject that could not be parsed. The two are told apart by `Valid`.

    .PARAMETER Subject
        The commit subject line, or a pull request title.

    .PARAMETER RequirePullRequest
        Treat a missing trailing pull request number as a violation. Off for a
        pull request title, written before the squash appends the number.
    #>
    [OutputType([pscustomobject])]
    param(
        [Parameter(Mandatory)] [AllowEmptyString()] [string] $Subject,
        [switch] $RequirePullRequest
    )

    $result = [pscustomobject]@{
        Subject       = $Subject
        Valid         = $false
        Type          = $null
        Scope         = $null
        Breaking      = $false
        Summary       = $null
        PullRequest   = $null
        Section       = $null
        Problem       = $null
    }

    $trimmed = $Subject.Trim()
    if (-not $trimmed) {
        $result.Problem = 'the subject is empty'
        return $result
    }

    $match = [regex]::Match($trimmed, $script:SubjectPattern)
    if (-not $match.Success) {
        $result.Problem = "does not read as 'type(scope): summary' -- see CONTRIBUTING.md"
        return $result
    }

    $type = $match.Groups['type'].Value
    if (-not $script:TypeSection.Contains($type)) {
        $known = (Get-CommitPolicyType) -join ', '
        $result.Type = $type
        $result.Problem = "'$type' is not a known type ($known)"
        return $result
    }

    $result.Type = $type
    $result.Scope = if ($match.Groups['scope'].Success) { $match.Groups['scope'].Value } else { $null }
    $result.Breaking = $match.Groups['breaking'].Success
    $result.Summary = $match.Groups['summary'].Value.Trim()
    $result.PullRequest = if ($match.Groups['pr'].Success) { [int]$match.Groups['pr'].Value } else { $null }

    # The squash append is unconditional, so a pull request title that already
    # ended in its own number arrives with that number twice. Only the repetition
    # is redundant: a different number in the same position cites another pull
    # request and has to survive.
    if ($result.PullRequest) {
        $repeated = " (#$($result.PullRequest))"
        if ($result.Summary.EndsWith($repeated)) {
            $result.Summary = $result.Summary.Substring(0, $result.Summary.Length - $repeated.Length).Trim()
        }
    }

    if (-not $result.Summary) {
        $result.Problem = 'the summary is empty'
        return $result
    }

    if ($RequirePullRequest -and -not $result.PullRequest) {
        $result.Problem = 'the subject does not end in its pull request number, in parentheses after the summary'
        return $result
    }

    # A breaking change is a Changed entry whatever its type says: a breaking fix
    # filed under Fixed would be read as a bugfix a reader can take blind.
    $result.Section = if ($result.Breaking) { 'Changed' } else { $script:TypeSection[$type] }
    $result.Valid = $true
    return $result
}

function Format-ChangelogEntry {
    <#
    .SYNOPSIS
        One changelog line for a parsed subject.

    .DESCRIPTION
        The summary is bolded and the pull request linked; the description is not
        copied in. The changelog is the quick read, the pull request is where the
        detail lives, and duplicating it means maintaining it twice.

    .PARAMETER Commit
        A ConvertFrom-CommitSubject result with Valid = $true.

    .PARAMETER RepositoryUrl
        Base URL the pull request number links against.
    #>
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [pscustomobject] $Commit,
        [string] $RepositoryUrl = 'https://github.com/Exoridus/exosnap'
    )

    $summary = $Commit.Summary
    if ($summary -notmatch '[.!?]$') { $summary += '.' }
    if ($Commit.Breaking) { $summary = "BREAKING: $summary" }

    $line = "- **$summary**"
    if ($Commit.PullRequest) {
        $line += " ([#$($Commit.PullRequest)]($RepositoryUrl/pull/$($Commit.PullRequest)))"
    }
    return $line
}

Export-ModuleMember -Function Get-CommitPolicyType, Get-CommitPolicySectionOrder,
ConvertFrom-CommitSubject, Format-ChangelogEntry
