<#
.SYNOPSIS
    Validates that the crash-report tag allowlist (kAllowedTagKeys) matches the
    keys documented in PRIVACY.md and docs/product-spec.md §14.

.DESCRIPTION
    ExoSnap's crash-report "before_send" hook only forwards a fixed allowlist of
    Sentry tag keys (libs/crash_capture/include/crash_capture/crash_scrubber.h,
    `kAllowedTagKeys`). PRIVACY.md and docs/product-spec.md §14 separately
    enumerate "what is sent" in plain language. Nothing enforced that the two
    stayed in sync (docs/privacy-review.md, ADR 0045) — a key added to the code
    allowlist without a doc update, or a doc claim with no matching code key,
    would silently drift the public promise away from the actual behavior.

    This is a dependency-free regex/line-based check (no build, no C++ parse):

      1. Extract the code's allowlist from crash_scrubber.h, between the
         `// PRIVACY-ALLOWLIST-BEGIN` / `// PRIVACY-ALLOWLIST-END` marker
         comments (a plain array of quoted string_view keys).
      2. Extract the documented key set from PRIVACY.md and
         docs/product-spec.md, each between a
         `<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->` /
         `<!-- PRIVACY-ALLOWLIST-TABLE-END -->` marker pair, reading every
         `` `key` `` backtick-quoted token in a markdown table row.
      3. Assert all three sets are IDENTICAL (as sets; order does not matter).
         A key present in code but missing from either doc, or documented but
         absent from code, is a FAIL with the exact key named.

    Self-check (do not commit): temporarily remove one key from PRIVACY.md's
    table (or add an extra one to crash_scrubber.h without updating the docs)
    and re-run this script — it must fail, naming the mismatched key(s). Revert
    before committing.

    Cheap and build-free by design, so it runs in the `lint` job on every PR
    (ci.yml) alongside the other validate-*.ps1 checks.
#>

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$headerPath      = Join-Path $repoRoot 'libs/crash_capture/include/crash_capture/crash_scrubber.h'
$privacyPath     = Join-Path $repoRoot 'PRIVACY.md'
$productSpecPath = Join-Path $repoRoot 'docs/product-spec.md'

$script:Errors = [System.Collections.Generic.List[string]]::new()
function Add-Error { param([string]$Message) $script:Errors.Add($Message) | Out-Null }

function Get-MarkedBlock {
    param(
        [string]$Text,
        [string]$BeginMarker,
        [string]$EndMarker,
        [string]$SourcePath
    )
    $beginIdx = $Text.IndexOf($BeginMarker)
    $endIdx   = $Text.IndexOf($EndMarker)
    if ($beginIdx -lt 0 -or $endIdx -lt 0 -or $endIdx -le $beginIdx) {
        Add-Error "Could not find '$BeginMarker' / '$EndMarker' markers in $SourcePath"
        return $null
    }
    return $Text.Substring($beginIdx, $endIdx - $beginIdx)
}

# ---------------------------------------------------------------------------
# 1. Code allowlist (crash_scrubber.h)
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $headerPath -PathType Leaf)) {
    Write-Host "  [FAIL] crash_scrubber.h not found: $headerPath" -ForegroundColor Red
    exit 1
}
$headerText = Get-Content -LiteralPath $headerPath -Raw
$codeBlock = Get-MarkedBlock -Text $headerText -BeginMarker '// PRIVACY-ALLOWLIST-BEGIN' `
    -EndMarker '// PRIVACY-ALLOWLIST-END' -SourcePath $headerPath

$codeKeys = [System.Collections.Generic.List[string]]::new()
if ($null -ne $codeBlock) {
    $matches = [regex]::Matches($codeBlock, '"([a-z][a-z0-9_.]*)"')
    foreach ($m in $matches) { $codeKeys.Add($m.Groups[1].Value) | Out-Null }
}
$codeKeySet = [System.Collections.Generic.HashSet[string]]::new([string[]]$codeKeys)

if ($codeKeySet.Count -eq 0) {
    Add-Error "No allowlist keys parsed from $headerPath — marker regex may be stale."
}

# ---------------------------------------------------------------------------
# 2. Documented key sets (PRIVACY.md, docs/product-spec.md §14)
# ---------------------------------------------------------------------------
function Get-DocKeySet {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Error "Doc not found: $Path"
        return [System.Collections.Generic.HashSet[string]]::new()
    }
    $text = Get-Content -LiteralPath $Path -Raw
    $block = Get-MarkedBlock -Text $text -BeginMarker '<!-- PRIVACY-ALLOWLIST-TABLE-BEGIN -->' `
        -EndMarker '<!-- PRIVACY-ALLOWLIST-TABLE-END -->' -SourcePath $Path
    $keys = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $block) {
        $matches = [regex]::Matches($block, '`([a-z][a-z0-9_.]*)`')
        foreach ($m in $matches) { $keys.Add($m.Groups[1].Value) | Out-Null }
    }
    return [System.Collections.Generic.HashSet[string]]::new([string[]]$keys)
}

$privacyKeySet     = Get-DocKeySet -Path $privacyPath
$productSpecKeySet = Get-DocKeySet -Path $productSpecPath

# ---------------------------------------------------------------------------
# 3. Bidirectional set comparison
# ---------------------------------------------------------------------------
function Compare-KeySets {
    param(
        [System.Collections.Generic.HashSet[string]]$Left,
        [string]$LeftName,
        [System.Collections.Generic.HashSet[string]]$Right,
        [string]$RightName
    )
    foreach ($key in $Left) {
        if (-not $Right.Contains($key)) {
            Add-Error "Key '$key' is in $LeftName but missing from $RightName."
        }
    }
    foreach ($key in $Right) {
        if (-not $Left.Contains($key)) {
            Add-Error "Key '$key' is in $RightName but missing from $LeftName."
        }
    }
}

Compare-KeySets -Left $codeKeySet -LeftName 'kAllowedTagKeys (crash_scrubber.h)' `
    -Right $privacyKeySet -RightName 'PRIVACY.md'
Compare-KeySets -Left $codeKeySet -LeftName 'kAllowedTagKeys (crash_scrubber.h)' `
    -Right $productSpecKeySet -RightName 'docs/product-spec.md §14'

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
if ($script:Errors.Count -gt 0) {
    foreach ($e in $script:Errors) { Write-Host "  [FAIL] $e" -ForegroundColor Red }
    Write-Host "Privacy allowlist validation FAILED ($($script:Errors.Count) mismatch(es))." -ForegroundColor Red
    exit 1
}

Write-Host "Privacy allowlist validation PASSED ($($codeKeySet.Count) keys match code, PRIVACY.md, and product-spec §14)." -ForegroundColor Green
exit 0
