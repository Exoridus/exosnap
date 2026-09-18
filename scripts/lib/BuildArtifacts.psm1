<#
.SYNOPSIS
    Locating a binary this repository builds, across the build trees that exist.
.DESCRIPTION
    Four trees can hold the same tool: a Ninja and a Visual Studio tree, each in a
    Debug and a Release configuration. They are built at different times and none of
    them is authoritative, so "the first path that exists" resolves to whichever tree
    was configured earliest -- which is how a campaign came to run a week-old
    exosnap-envctl while a build from the same afternoon sat in the Ninja tree.

    For an instrument -- envctl, a probe -- the newest build is the right answer, and
    that is what Resolve-BuiltArtifact returns. For the product binary UNDER test the
    answer is a deliberate choice between Debug and Release, so that caller keeps its
    own order and uses Test-NewerArtifactExists to say out loud when it passed over
    something newer.

    The two tree layouts differ: MSBuild puts the configuration in a directory of its
    own (tools/envctl/Debug/exosnap-envctl.exe), Ninja does not
    (tools/envctl/exosnap-envctl.exe). Callers name the Ninja shape and both are
    searched.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Every tree a local build can land in, with the configuration directory MSBuild
# inserts. Order carries no meaning here -- selection is by build time.
$script:BuildTrees = @(
    @{ Directory = 'build/windows-x64-ninja-release'; Configurations = @() },
    @{ Directory = 'build/windows-x64-ninja-debug';   Configurations = @() },
    @{ Directory = 'build/windows-x64-release';       Configurations = @('Release') },
    @{ Directory = 'build/windows-x64-debug';         Configurations = @('Debug') }
)

function Get-RepositoryRoot {
    param([string] $RepoRoot)

    if (-not [string]::IsNullOrWhiteSpace($RepoRoot)) {
        return (Resolve-Path -LiteralPath $RepoRoot).Path
    }
    return (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}

function Get-ArtifactCandidate {
    <#
    .SYNOPSIS
        Every path a tree could hold this artifact at, whether or not it exists.
    #>
    param(
        [Parameter(Mandatory)] [string] $RelativePath,
        [Parameter(Mandatory)] [string] $Root
    )

    $directory = Split-Path -Parent $RelativePath
    $leaf = Split-Path -Leaf $RelativePath

    $candidates = [System.Collections.Generic.List[string]]::new()
    foreach ($tree in $script:BuildTrees) {
        $candidates.Add((Join-Path $Root (Join-Path $tree.Directory $RelativePath)))
        foreach ($configuration in $tree.Configurations) {
            $withConfiguration = if ($directory) { Join-Path (Join-Path $directory $configuration) $leaf } else { Join-Path $configuration $leaf }
            $candidates.Add((Join-Path $Root (Join-Path $tree.Directory $withConfiguration)))
        }
    }
    return $candidates
}

function Resolve-BuiltArtifact {
    <#
    .SYNOPSIS
        The most recently built copy of an artifact, or $null when no tree holds one.
    .DESCRIPTION
        Returns $null rather than throwing: a machine that never built the tool can
        still run everything that does not need it, and a caller that reports
        UNAVAILABLE with a reason states something true about that run.
    .PARAMETER RelativePath
        Path inside a build tree, in the Ninja layout (no configuration directory),
        for example 'tools/envctl/exosnap-envctl.exe'.
    .PARAMETER RepoRoot
        Repository root. Defaults to the one containing this module.
    .OUTPUTS
        A record with Path, Tree and BuiltAt, or $null.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $RelativePath,
        [string] $RepoRoot = ''
    )

    $root = Get-RepositoryRoot -RepoRoot $RepoRoot
    $found = @(Get-ArtifactCandidate -RelativePath $RelativePath -Root $root |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        ForEach-Object { Get-Item -LiteralPath $_ })

    if ($found.Count -eq 0) { return $null }

    $newest = $found | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $tree = ''
    foreach ($candidate in $script:BuildTrees) {
        $prefix = Join-Path $root $candidate.Directory
        if ($newest.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $tree = $candidate.Directory
            break
        }
    }

    return [pscustomobject]@{
        Path    = $newest.FullName
        Tree    = $tree
        BuiltAt = $newest.LastWriteTime
    }
}

function Test-NewerArtifactExists {
    <#
    .SYNOPSIS
        Whether some other tree holds a newer build of the artifact just chosen.
    .DESCRIPTION
        For the binary under test, preferring Release over Debug is a decision, not
        an accident, so this reports rather than overrides. What it prevents is the
        silent case: a campaign verifying a Release build from last week while the
        change it is meant to cover sits unbuilt in another tree.
    .OUTPUTS
        The newer record (Path, Tree, BuiltAt), or $null when the choice is current.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] [string] $ChosenPath,
        [Parameter(Mandatory)] [string] $RelativePath,
        [string] $RepoRoot = ''
    )

    $newest = Resolve-BuiltArtifact -RelativePath $RelativePath -RepoRoot $RepoRoot
    if ($null -eq $newest) { return $null }
    if ($newest.Path -ieq (Resolve-Path -LiteralPath $ChosenPath).Path) { return $null }

    $chosenAt = (Get-Item -LiteralPath $ChosenPath).LastWriteTime
    if ($newest.BuiltAt -le $chosenAt) { return $null }
    return $newest
}

Export-ModuleMember -Function Resolve-BuiltArtifact, Test-NewerArtifactExists
