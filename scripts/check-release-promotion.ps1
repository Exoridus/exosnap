#Requires -Version 7.0
<#
.SYNOPSIS
    The promotion lock: decides whether the artifacts a final tag is about to publish
    are the artifacts that were qualified.

.DESCRIPTION
    `check-release-qualification.ps1` proves that a signed record qualified this
    commit against the bytes the release candidate published. It cannot prove anything
    about the bytes the final tag built, because those are a different build: the
    release identity is compiled in, so `0.9.1-rc1` and `0.9.1` are different
    artifacts by construction and their package hashes can never match.

    This is the check that closes that gap. Both builds write a per-file inventory of
    the portable install tree (`artifact-manifest.json`) and a record of the toolchain
    that produced it (`toolchain-manifest.json`). Publishing is allowed only when the
    two install trees hold the same files, every file is byte-identical except the
    ones the record's promotion contract names, both builds came from the same commit,
    and both were built by the same compiler, CMake, Qt, WiX and vendored FFmpeg.

    What it does not cover, stated so nobody reads more into a green result than is
    there: the three compiled binaries are not compared to anything, because they
    cannot be. Their correctness rests on the identical commit and the identical
    toolchain, both checked here. A regression that the release build introduces into
    one of them while changing nothing else is out of reach until the release identity
    stops being compiled in and a final tag can ship the qualified bytes unchanged.

    Nothing here publishes, uploads, tags or mutates anything.

.PARAMETER RecordPath
    The qualification record downloaded from the RC release. Its signature is verified
    again here, so this script is fail-closed on its own rather than on the assumption
    that an earlier job already checked.

.PARAMETER PublicKeyHex
    The release public key, 64 hex characters.

.PARAMETER QualifiedManifestPath
    `artifact-manifest.json` as published by the qualified release candidate.

.PARAMETER CandidateManifestPath
    `artifact-manifest.json` produced by this run's build job.

.PARAMETER QualifiedToolchainPath
    `toolchain-manifest.json` as published by the qualified release candidate.

.PARAMETER CandidateToolchainPath
    `toolchain-manifest.json` produced by this run's build job.

.PARAMETER CandidateVersion
    The full release identity this tag publishes, e.g. `0.9.1`.

.PARAMETER SummaryPath
    Markdown summary file to append the verdict to (GITHUB_STEP_SUMMARY).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $RecordPath,
    [string] $PublicKeyHex,
    [Parameter(Mandatory)] [string] $QualifiedManifestPath,
    [Parameter(Mandatory)] [string] $CandidateManifestPath,
    [string] $QualifiedToolchainPath,
    [string] $CandidateToolchainPath,
    [Parameter(Mandatory)] [string] $CandidateVersion,
    [string] $SummaryPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib/ReleaseQualification.ps1')

function Write-PromotionVerdict {
    param(
        [Parameter(Mandatory)] [bool] $Promotable,
        [string[]] $Reasons = @(),
        [string[]] $Notes = @(),
        [string] $Detail
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    if ($Promotable) {
        $lines.Add('## Release promotion: the shipped tree matches the qualified tree')
        Write-Host 'Release promotion: the shipped tree matches the qualified tree' -ForegroundColor Green
    }
    else {
        $lines.Add('## Release promotion: BLOCKED -- nothing is published')
        Write-Host '::error::The artifacts this tag would publish are not the artifacts that were qualified.'
    }
    $lines.Add('')
    if ($Detail) { $lines.Add($Detail); $lines.Add('') }
    foreach ($reason in $Reasons) {
        $lines.Add("- $reason")
        Write-Host "::error::$reason"
    }
    foreach ($note in $Notes) {
        $lines.Add("- (permitted) $note")
        Write-Host "  (permitted) $note"
    }

    if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
        Add-Content -LiteralPath $SummaryPath -Value ($lines -join "`n") -Encoding utf8NoBOM
    }
    exit $(if ($Promotable) { 0 } else { 1 })
}

function Read-JsonOrNull {
    param([string] $Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) }
    catch { return $null }
}

if (-not (Test-Path -LiteralPath $RecordPath -PathType Leaf)) {
    Write-PromotionVerdict -Promotable $false -Reasons @(
        "No qualification record at '$RecordPath', so there is nothing to compare this release against.")
}

$signature = Test-ReleaseQualificationSignature -RecordPath $RecordPath -PublicKeyHex $PublicKeyHex
if (-not $signature.Verified) {
    Write-PromotionVerdict -Promotable $false -Reasons @($signature.Reason)
}

$record = Read-JsonOrNull -Path $RecordPath
if ($null -eq $record) {
    Write-PromotionVerdict -Promotable $false -Reasons @(
        "The qualification record at '$RecordPath' could not be parsed.")
}

$promotion = Get-ReleaseQualificationField -Object $record -Name 'promotion'
$contractId = "$(Get-ReleaseQualificationField -Object $promotion -Name 'contract')"
if ($contractId -ne (Get-ReleasePromotionContract).Id) {
    Write-PromotionVerdict -Promotable $false -Reasons @(
        "The record declares promotion contract '$(if ($contractId) { $contractId } else { 'none' })', " +
        "which this checkout does not implement (it implements '$((Get-ReleasePromotionContract).Id)'). " +
        'A record may not widen its own difference budget.')
}

$qualifiedVersion = "$(Get-ReleaseQualificationField -Object $promotion -Name 'qualifiedVersion')"
$mutableEntries = [string[]]@(Get-ReleaseQualificationField -Object $promotion -Name 'mutableEntries')

$qualifiedManifest = Read-JsonOrNull -Path $QualifiedManifestPath
if ($null -eq $qualifiedManifest) {
    Write-PromotionVerdict -Promotable $false -Reasons @(
        "The qualified release candidate published no readable artifact-manifest.json at " +
        "'$QualifiedManifestPath'. A candidate cut before the release workflow attached that " +
        'inventory cannot be promoted from; cut a new candidate from this commit and qualify it.')
}
$candidateManifest = Read-JsonOrNull -Path $CandidateManifestPath
if ($null -eq $candidateManifest) {
    Write-PromotionVerdict -Promotable $false -Reasons @(
        "This run produced no readable artifact-manifest.json at '$CandidateManifestPath'.")
}

$tree = Compare-ReleaseInstallTree -QualifiedManifest $qualifiedManifest -CandidateManifest $candidateManifest `
    -QualifiedVersion $qualifiedVersion -CandidateVersion $CandidateVersion -MutableEntries $mutableEntries
$toolchain = Compare-ReleaseToolchain `
    -QualifiedToolchain (Read-JsonOrNull -Path $QualifiedToolchainPath) `
    -CandidateToolchain (Read-JsonOrNull -Path $CandidateToolchainPath)

$reasons = @($tree.Differences) + @($toolchain.Differences)
$notes = @($tree.Notes) + @($toolchain.Notes)
$detail = "Qualified ``$qualifiedVersion``, publishing ``$CandidateVersion``, contract ``$contractId``. " +
'Every file in the portable install tree outside the contract must be byte-identical.'

Write-PromotionVerdict -Promotable ($reasons.Count -eq 0) -Reasons $reasons -Notes $notes -Detail $detail
