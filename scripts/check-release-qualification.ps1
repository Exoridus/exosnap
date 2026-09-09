#Requires -Version 7.0
<#
.SYNOPSIS
    The publish lock: decides whether a release-verification record may promote a
    commit to a final release.

.DESCRIPTION
    Run by `.github/workflows/release-candidate.yml` on a final (`vX.Y.Z`) tag, before
    anything is published. It answers one question, and it answers it fail-closed: is
    there a qualification record, signed by the release key, that measured exactly this
    commit, on exactly the bytes the RC published, with no product defect, no harness
    failure, no required gate left unanswered, and no machine left misconfigured?

    The signature is checked first, before a single field is read. Everything else in a
    record -- commit, RC tag, package hashes -- is publicly readable from the RC
    release, so all of it can be retyped by hand into a record that no campaign ever
    produced. Only the signature separates the two.

    Any doubt is a refusal. A missing record, an unsigned record, a record whose
    signature does not verify, an unreadable record, a record about a different commit
    and a record about different bytes are all the same outcome and four different
    messages: the job stops before the publish step, and the reason goes into the job
    summary.

    Nothing here publishes, uploads, tags or mutates anything.

.PARAMETER RecordPath
    The `release-verification.json` downloaded from the RC release.

.PARAMETER SignaturePath
    The detached ed25519 signature over the record's bytes. Defaults to the record
    path plus `.sig`, which is how `release-verify.ps1 qualify -Publish` names and
    attaches it.

.PARAMETER PublicKeyHex
    The release public key, 64 hex characters -- the same
    EXOSNAP_UPDATE_PUBLIC_KEY_HEX the shipped binaries embed and the update manifest
    is verified against. Without it nothing can be verified and the answer is no.

.PARAMETER ExpectedCommit
    The commit the final tag points at. The record must qualify exactly this one.

.PARAMETER ExpectedRcTag
    The RC release the record was taken from.

.PARAMETER Sha256Directory
    Directory holding the RC release's `.sha256` sidecars. Every sidecar found must be
    matched by a package in the record with an equal hash, so the campaign is proven to
    have run against the artifacts the RC actually published.

.PARAMETER SummaryPath
    Markdown summary file to append the verdict to (GITHUB_STEP_SUMMARY).

.EXAMPLE
    pwsh scripts/check-release-qualification.ps1 -RecordPath rc/release-verification.json `
        -PublicKeyHex $env:EXOSNAP_UPDATE_PUBLIC_KEY_HEX `
        -ExpectedCommit $env:GITHUB_SHA -ExpectedRcTag v0.9.1-rc1 -Sha256Directory rc
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $RecordPath,
    [string] $SignaturePath,
    [string] $PublicKeyHex,
    [string] $ExpectedCommit,
    [string] $ExpectedRcTag,
    [string] $Sha256Directory,
    [string] $SummaryPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib/ReleaseQualification.ps1')

function Write-Verdict {
    <#
    .SYNOPSIS
        Prints the verdict and appends it to the job summary, then exits with 0 for a
        qualified record and 1 for anything else.
    #>
    param(
        [Parameter(Mandatory)] [bool] $Qualified,
        [string[]] $Reasons = @(),
        [string] $Detail
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    if ($Qualified) {
        $lines.Add('## Release qualification: QUALIFIED')
        $lines.Add('')
        if ($Detail) { $lines.Add($Detail) }
        Write-Host 'Release qualification: QUALIFIED' -ForegroundColor Green
        if ($Detail) { Write-Host "  $Detail" }
    }
    else {
        $lines.Add('## Release qualification: BLOCKED -- nothing is published')
        $lines.Add('')
        if ($Detail) { $lines.Add($Detail); $lines.Add('') }
        foreach ($reason in $Reasons) { $lines.Add("- $reason") }
        Write-Host '::error::Release qualification failed; this commit must not be published.'
        foreach ($reason in $Reasons) { Write-Host "::error::$reason" }
    }

    if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
        Add-Content -LiteralPath $SummaryPath -Value ($lines -join "`n") -Encoding utf8NoBOM
    }
    exit $(if ($Qualified) { 0 } else { 1 })
}

function Get-PublishedPackageHashes {
    <#
    .SYNOPSIS
        fileName -> lowercase SHA-256, read from the release's `.sha256` sidecars.
    .DESCRIPTION
        Sidecar format is the sha256sum convention: the hash, whitespace, the file
        name it describes. The name in the sidecar is used rather than the sidecar's
        own name, because that is the name the manifest and the record both cite.
    #>
    param([string] $Directory)

    $hashes = @{}
    if ([string]::IsNullOrWhiteSpace($Directory)) { return $hashes }
    if (-not (Test-Path -LiteralPath $Directory)) {
        throw "No such directory for the published SHA-256 sidecars: '$Directory'"
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $Directory -Filter '*.sha256' -File)) {
        $text = (Get-Content -LiteralPath $file.FullName -Raw).Trim()
        if ([string]::IsNullOrWhiteSpace($text)) {
            throw "Sidecar '$($file.Name)' is empty; the published assets cannot be verified."
        }
        $parts = @($text -split '\s+' | Where-Object { $_ })
        if ($parts.Count -lt 2 -or $parts[0] -notmatch '^[0-9a-fA-F]{64}$') {
            throw "Sidecar '$($file.Name)' is not in '<sha256>  <file name>' form: '$text'"
        }
        $hashes[$parts[1]] = $parts[0].ToLowerInvariant()
    }
    return $hashes
}

if (-not (Test-Path -LiteralPath $RecordPath -PathType Leaf)) {
    Write-Verdict -Qualified $false -Reasons @(
        "No qualification record at '$RecordPath'. A final release requires a release-verification.json " +
        'produced by a completed release-verify campaign against the RC built from this commit, and ' +
        'uploaded to that RC release.')
}

# Before anything is parsed: an unsigned or wrongly signed record is not evidence,
# and reading its fields would only lend them credibility they do not have.
$signature = Test-ReleaseQualificationSignature -RecordPath $RecordPath `
    -SignaturePath $SignaturePath -PublicKeyHex $PublicKeyHex
if (-not $signature.Verified) {
    Write-Verdict -Qualified $false -Reasons @($signature.Reason)
}

try {
    $record = Get-Content -LiteralPath $RecordPath -Raw | ConvertFrom-Json
}
catch {
    Write-Verdict -Qualified $false -Reasons @(
        "The qualification record at '$RecordPath' could not be parsed: $($_.Exception.Message)")
}
if ($null -eq $record) {
    Write-Verdict -Qualified $false -Reasons @("The qualification record at '$RecordPath' is empty.")
}

try {
    $published = Get-PublishedPackageHashes -Directory $Sha256Directory
}
catch {
    Write-Verdict -Qualified $false -Reasons @($_.Exception.Message)
}

$verdict = Test-ReleaseQualification -Record $record -ExpectedCommit $ExpectedCommit `
    -ExpectedRcTag $ExpectedRcTag -ExpectedPackageSha256 $published

$detail = "RC ``$(Get-ReleaseQualificationField -Object $record -Name 'rcTag')``, commit " +
"``$(Get-ReleaseQualificationField -Object $record -Name 'sourceCommit')``, campaign " +
"``$(Get-ReleaseQualificationField -Object $record -Name 'runId')``, harness " +
"``$(Get-ReleaseQualificationField -Object (Get-ReleaseQualificationField -Object $record -Name 'harness') -Name 'version')``, " +
"catalog ``$(Get-ReleaseQualificationField -Object (Get-ReleaseQualificationField -Object $record -Name 'catalog') -Name 'version')``."

Write-Verdict -Qualified $verdict.Qualified -Reasons $verdict.Reasons -Detail $detail
