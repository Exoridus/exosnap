#Requires -Version 7.0
<#
.SYNOPSIS
    What each package channel is serving, against what the publication policy says it
    should be. Advisory: it reports, it never publishes.
.DESCRIPTION
    Four channels carry ExoSnap to users -- the GitHub Release every other one
    downloads from, Chocolatey, WinGet and Scoop -- and three of them are currently
    held behind the repository version on purpose. "On purpose" is the part that goes
    stale: without a written policy, a channel serving an old version is
    indistinguishable from a submission somebody forgot, and the only way to tell was
    to remember why.

    `packaging/publication-policy.json` states the intent per channel, and this script
    is the other half. It runs in two parts:

      - The policy itself, offline and deterministic: every packaging surface in the
        tree is covered, every version parses, a channel meant to publish names the
        version the tree declares, and a hold resumes at a version ahead of where it
        is held. A defect here fails the run, because it is a defect in the statement
        itself and needs no network to see.

      - The feeds, over the network: what each one actually serves. Everything here is
        advisory. A feed that moved without the policy moving is worth knowing about
        the day it happens, but it is not a reason to fail a build -- the feeds are
        outside this repository, and the answer to drift is a decision, never an
        automatic submission. `-Offline` skips this half entirely.

    Nothing in here submits, pushes or publishes anything, and nothing in this
    repository does: every submission is a step in docs/release-checklist.md section 8
    that a person runs.
.PARAMETER Offline
    Check only the policy. No network.
.PARAMETER PolicyPath
    The policy to check. Defaults to packaging/publication-policy.json.
.PARAMETER RepoRoot
    The tree to check. Defaults to the repository this script is in.
.PARAMETER TimeoutSeconds
    Per-feed request timeout.
#>
[CmdletBinding()]
param(
    [switch] $Offline,
    [string] $PolicyPath,
    [string] $RepoRoot,
    [int] $TimeoutSeconds = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not $PolicyPath) { $PolicyPath = Join-Path $RepoRoot 'packaging/publication-policy.json' }

# The surfaces the repository actually carries. A channel added to packaging/ without
# a line in the policy is the case this list exists to catch: the policy would still
# be internally consistent and would simply say nothing about it.
$script:PackagedChannels = @{
    chocolatey = 'packaging/chocolatey/exosnap.nuspec'
    winget     = 'packaging/winget/manifests'
    scoop      = 'packaging/scoop/exosnap.json'
}

$script:Problems = [System.Collections.Generic.List[string]]::new()
$script:Advisories = [System.Collections.Generic.List[string]]::new()

function Add-Problem { param([string] $Message) $script:Problems.Add($Message) }
function Add-Advisory { param([string] $Message) $script:Advisories.Add($Message) }

function ConvertTo-ComparableVersion {
    <#
    .SYNOPSIS
        A version for ordering, or $null when the string is not x.y.z.
    #>
    param([string] $Value)
    if ($Value -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { return $null }
    return [version]$Value
}

function Get-FeedVersion {
    <#
    .SYNOPSIS
        The version a channel's public feed is serving, or $null with a reason.
    .DESCRIPTION
        One reader per channel because each feed answers a different question:
        Chocolatey redirects its package endpoint to the newest .nupkg, the WinGet
        repository keeps one directory per published version, Scoop's bucket entry is
        a manifest, and GitHub answers with the release itself. An unreachable feed is
        reported as unknown -- the network is not the subject of this check.
    .OUTPUTS
        @{ Version = [string]; Detail = [string] }
    #>
    param([Parameter(Mandatory)] [string] $Channel, [Parameter(Mandatory)] [string] $Feed)

    try {
        switch ($Channel) {
            'chocolatey' {
                # The redirect IS the answer: the package endpoint answers 302 to the
                # newest .nupkg, whose name carries the version, and following it would
                # download several megabytes nobody here needs. HttpClient rather than
                # Invoke-WebRequest because the latter cannot hand back an unfollowed
                # redirect -- reading its Location throws instead.
                $handler = [Net.Http.HttpClientHandler]::new()
                $handler.AllowAutoRedirect = $false
                $client = [Net.Http.HttpClient]::new($handler)
                try {
                    $client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
                    $response = $client.GetAsync($Feed, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).
                        GetAwaiter().GetResult()
                    $location = "$($response.Headers.Location)"
                    if ($location -match '/exosnap\.([0-9]+\.[0-9]+\.[0-9]+)\.nupkg') {
                        return @{ Version = $Matches[1]; Detail = $location }
                    }
                    return @{ Version = $null; Detail = "the package endpoint answered $([int]$response.StatusCode) without a .nupkg redirect" }
                }
                finally { $client.Dispose(); $handler.Dispose() }
            }
            'winget' {
                $entries = Invoke-RestMethod -Uri $Feed -TimeoutSec $TimeoutSeconds -ErrorAction Stop
                $versions = @($entries | Where-Object { $_.type -eq 'dir' } |
                        ForEach-Object { ConvertTo-ComparableVersion -Value $_.name } |
                        Where-Object { $null -ne $_ } | Sort-Object)
                if ($versions.Count -eq 0) { return @{ Version = $null; Detail = 'the package directory holds no version directory' } }
                return @{ Version = "$($versions[-1])"; Detail = "$($versions.Count) version(s) published" }
            }
            'scoop' {
                $manifest = Invoke-RestMethod -Uri $Feed -TimeoutSec $TimeoutSeconds -ErrorAction Stop
                return @{ Version = "$($manifest.version)"; Detail = "$($manifest.architecture.'64bit'.url)" }
            }
            'github' {
                $release = Invoke-RestMethod -Uri $Feed -TimeoutSec $TimeoutSeconds -ErrorAction Stop
                return @{ Version = ("$($release.tag_name)" -replace '^v', ''); Detail = "published $($release.published_at)" }
            }
            default { return @{ Version = $null; Detail = "no reader for channel '$Channel'" } }
        }
    }
    catch {
        return @{ Version = $null; Detail = "unreachable: $($_.Exception.Message)" }
    }
}

# ---------------------------------------------------------------------------
# The policy
# ---------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $PolicyPath -PathType Leaf)) {
    Write-Host "No publication policy at '$PolicyPath'." -ForegroundColor Red
    exit 1
}

$policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
$cmakeText = Get-Content -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmakeText -notmatch 'project\(\s*exosnap\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not parse project(exosnap VERSION x.y.z) from the root CMakeLists.txt.'
}
$repositoryVersion = $Matches[1]
$repositoryComparable = ConvertTo-ComparableVersion -Value $repositoryVersion

Write-Host "Publication policy: the tree declares $repositoryVersion." -ForegroundColor Cyan
Write-Host ''

$channelNames = @($policy.channels.PSObject.Properties.Name)
foreach ($required in ($script:PackagedChannels.Keys | Sort-Object)) {
    if ($channelNames -notcontains $required) {
        Add-Problem "packaging/$required is in the tree and the policy says nothing about it"
        continue
    }
    $marker = Join-Path $RepoRoot $script:PackagedChannels[$required]
    if (-not (Test-Path -LiteralPath $marker)) {
        Add-Problem "the policy covers '$required', and '$($script:PackagedChannels[$required])' is not in the tree"
    }
}

$due = [System.Collections.Generic.List[string]]::new()

foreach ($name in ($channelNames | Sort-Object)) {
    $channel = $policy.channels.$name
    $intent = "$($channel.intent)"
    $expected = "$($channel.expectedVersion)"
    $expectedComparable = ConvertTo-ComparableVersion -Value $expected
    if ($null -eq $expectedComparable) {
        Add-Problem "$name declares expectedVersion '$expected', which is not x.y.z"
        continue
    }

    switch ($intent) {
        'publish' {
            if ($expected -ne $repositoryVersion) {
                Add-Problem ("$name is meant to publish, so its expectedVersion must be the version the tree " +
                    "declares ($repositoryVersion); it says $expected")
            }
        }
        'hold' {
            $resumeAt = "$($channel.resumeAt)"
            $resumeComparable = ConvertTo-ComparableVersion -Value $resumeAt
            if ($null -eq $resumeComparable) {
                Add-Problem "$name is held and declares resumeAt '$resumeAt', which is not x.y.z"
                break
            }
            if ($resumeComparable -le $expectedComparable) {
                Add-Problem "$name is held at $expected and resumes at $resumeAt, which is not ahead of it"
            }
            if ($resumeComparable -lt $repositoryComparable) {
                Add-Problem ("$name is held until $resumeAt, and the tree already declares ${repositoryVersion}: " +
                    'the hold was overtaken rather than lifted')
            }
            if (-not "$($channel.reason)".Trim()) {
                Add-Problem "$name is held and states no reason; a hold nobody wrote down is indistinguishable from a forgotten submission"
            }
            if ($resumeComparable -eq $repositoryComparable) {
                $due.Add("$name resumes at $resumeAt, which is the version the tree declares")
            }
        }
        default { Add-Problem "$name declares intent '$intent'; it must be 'publish' or 'hold'" }
    }
}

foreach ($problem in $script:Problems) { Write-Host "  policy: $problem" -ForegroundColor Red }
if ($script:Problems.Count -eq 0) {
    Write-Host "  policy: OK ($($channelNames.Count) channel(s) declared)" -ForegroundColor Green
}
foreach ($entry in $due) { Write-Host "  due:    $entry" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# The feeds
# ---------------------------------------------------------------------------

if (-not $Offline) {
    Write-Host ''
    foreach ($name in ($channelNames | Sort-Object)) {
        $channel = $policy.channels.$name
        $expected = "$($channel.expectedVersion)"
        $observed = Get-FeedVersion -Channel $name -Feed "$($channel.feed)"
        if (-not $observed.Version) {
            Write-Host ("  {0,-12} expected {1,-8} observed ?        ({2})" -f $name, $expected, $observed.Detail)
            Add-Advisory "$name could not be read: $($observed.Detail)"
            continue
        }
        $agrees = $observed.Version -eq $expected
        Write-Host ("  {0,-12} expected {1,-8} observed {2,-8} {3}" -f $name, $expected, $observed.Version,
            $(if ($agrees) { 'as declared' } else { 'DRIFT' })) `
            -ForegroundColor $(if ($agrees) { 'Gray' } else { 'Yellow' })
        if (-not $agrees) {
            Add-Advisory ("$name serves $($observed.Version) and the policy says $expected; " +
                'either the feed moved or the policy did not')
        }
    }
}

Write-Host ''
foreach ($advisory in $script:Advisories) { Write-Host "  advisory: $advisory" -ForegroundColor Yellow }

if ($script:Problems.Count -gt 0) {
    Write-Host ''
    Write-Host ("Publication policy FAILED: $($script:Problems.Count) problem(s) in " +
        "packaging/publication-policy.json.") -ForegroundColor Red
    exit 1
}

Write-Host ("Publication policy OK." + $(if ($script:Advisories.Count -gt 0) {
            " $($script:Advisories.Count) advisory finding(s) above; publishing is a decision, never automatic."
        } else { '' })) -ForegroundColor Green
exit 0
