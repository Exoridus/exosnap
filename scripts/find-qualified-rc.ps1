#Requires -Version 7.0
<#
.SYNOPSIS
    Finds the release candidate a commit may be promoted from, and downloads the
    evidence attached to it.

.DESCRIPTION
    One rule, in one place, because two gates ask the same question and a second copy
    of it would eventually answer differently: `release-candidate.yml`'s
    `require-qualification` before a final tag publishes, and `sign-manifest.yml`'s
    standalone dispatch before it may attach a re-signed update manifest to a release
    that is already public.

    The rule is the NEWEST published candidate whose tag points at exactly this
    commit. Not merely some candidate that was once qualified at it: cutting a later
    candidate from the same source and never verifying it must not become publishable
    on the strength of an older record.

    Downloads, into -OutputDirectory: the qualification record and its detached
    signature, the `.sha256` sidecars, and the candidate's build and toolchain
    inventories. A file that is not there is left not there -- the checks that read
    them say what a missing one means, in words that name the fix, which a download
    error cannot.

    Reads only. Nothing is uploaded, tagged, published or edited.

.PARAMETER Repository
    owner/name, as GITHUB_REPOSITORY carries it.

.PARAMETER BaseVersion
    The numeric X.Y.Z base whose candidates are considered.

.PARAMETER Commit
    The full commit SHA the candidate's tag must point at.

.PARAMETER OutputDirectory
    Directory the evidence is downloaded into. Created if absent.

.PARAMETER GhCommand
    The `gh` executable. Overridable so the tests can drive the discovery without a
    network or a real release.

.OUTPUTS
    The RC tag, on success. Throws with an operator-readable reason otherwise.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Repository,
    [Parameter(Mandatory)] [string] $BaseVersion,
    [Parameter(Mandatory)] [string] $Commit,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [string] $GhCommand = 'gh'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Gh {
    param([string[]] $Arguments)
    $output = & $GhCommand @Arguments 2>&1
    return @{ ExitCode = $LASTEXITCODE; Output = @($output) }
}

$tags = Invoke-Gh -Arguments @('api', "repos/$Repository/releases", '--paginate',
    '--jq', '.[] | select(.draft == false) | .tag_name')
if ($tags.ExitCode -ne 0) {
    throw "Could not list the releases of $Repository : $($tags.Output -join ' ')"
}

$pattern = "^v$([regex]::Escape($BaseVersion))-rc(\d+)$"
$best = $null
$bestNumber = -1
foreach ($tag in $tags.Output) {
    $text = "$tag".Trim()
    $match = [regex]::Match($text, $pattern)
    if (-not $match.Success) { continue }
    $resolved = Invoke-Gh -Arguments @('api', "repos/$Repository/commits/$text", '--jq', '.sha')
    if ($resolved.ExitCode -ne 0) { continue }
    if ("$($resolved.Output -join '')".Trim() -ne $Commit) { continue }
    $number = [int]$match.Groups[1].Value
    if ($number -gt $bestNumber) { $bestNumber = $number; $best = $text }
}

if ($null -eq $best) {
    throw "No published release candidate points at $Commit. A final release is promoted from a " +
    'qualified RC of the same commit (docs/release-checklist.md section 3a), never tagged directly.'
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$record = Invoke-Gh -Arguments @('release', 'download', $best, '--repo', $Repository,
    '--dir', $OutputDirectory, '--clobber', '--pattern', 'release-verification.json')
if ($record.ExitCode -ne 0) {
    throw "Release candidate $best carries no release-verification.json. Run the campaign against it " +
    "and attach the record with 'release-verify.ps1 qualify -Publish' before promoting."
}
$sidecars = Invoke-Gh -Arguments @('release', 'download', $best, '--repo', $Repository,
    '--dir', $OutputDirectory, '--clobber', '--pattern', '*.sha256')
if ($sidecars.ExitCode -ne 0) {
    throw "Release candidate $best publishes no .sha256 sidecars, so the qualified bytes cannot be " +
    'compared against the published ones.'
}
foreach ($pattern in @('release-verification.json.sig', 'artifact-manifest.json', 'toolchain-manifest.json')) {
    Invoke-Gh -Arguments @('release', 'download', $best, '--repo', $Repository,
        '--dir', $OutputDirectory, '--clobber', '--pattern', $pattern) | Out-Null
}

return $best
