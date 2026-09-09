#Requires -Version 7.0
<#
.SYNOPSIS
    Tests for the release qualification record and the publish lock that reads it.

.DESCRIPTION
    Not Pester: the same homegrown harness the other script tests use, so CTest and
    `verify.ps1` run all of them the same way and a contributor reads one style.

    What is under test here is a refusal. `check-release-qualification.ps1` is the last
    thing standing between a pushed tag and a published release, so every way it can be
    wrong is a way an unverified release ships: a missing record, a record about a
    different commit, a record about different bytes, a product defect, a harness
    failure, a required gate nobody answered. Each of those is a case below, and the
    good record is only meaningful because the bad ones are proven to be refused.

    Nothing here touches the network, the machine's configuration, or a real release.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scriptRoot = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $scriptRoot 'lib/LiveVerifyState.psm1') -Force -DisableNameChecking
. (Join-Path $scriptRoot 'lib/ReleaseQualification.ps1')

$script:CheckScript = Join-Path $scriptRoot 'check-release-qualification.ps1'
$script:RepositoryRoot = Split-Path -Parent $scriptRoot
$script:Passed = 0
$script:Failed = 0

function Test-Case {
    param([Parameter(Mandatory)] [string] $Name, [Parameter(Mandatory)] [scriptblock] $Body)
    try {
        & $Body
        Write-Host "  PASS  $Name" -ForegroundColor Green
        $script:Passed++
    }
    catch {
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "        $($_.Exception.Message)" -ForegroundColor Red
        $script:Failed++
    }
}

function Assert-True { param($Condition, [string] $Message) if (-not $Condition) { throw $Message } }
function Assert-Equal {
    param($Expected, $Actual, [string] $Message)
    if ("$Expected" -ne "$Actual") { throw "$Message (expected '$Expected', got '$Actual')" }
}
function Assert-Match {
    param([string] $Pattern, [string] $Text, [string] $Message)
    if ($Text -notmatch $Pattern) { throw "$Message (no match for '$Pattern' in: $Text)" }
}
function Assert-NoMatch {
    param([string] $Pattern, [string] $Text, [string] $Message)
    if ($Text -match $Pattern) { throw "$Message (unexpected match for '$Pattern' in: $Text)" }
}

function New-TestDirectory {
    $path = Join-Path ([IO.Path]::GetTempPath()) "release-qualification-tests/$([guid]::NewGuid().ToString('n'))"
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    return $path
}

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

# RFC 8032's own key material. A published test vector cannot be mistaken for a real
# release key, and it makes the fixtures reproducible byte for byte.
$script:TestSigningKey = 'nWGxne/9WmC6hEr0kuwsxERJxWl7MmkZcDusAxyuf2A='
$script:TestPublicKey = 'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a'
$script:OtherSigningKey = 'TM0Imyj/ltqdtsNG7BFOD1uKMZ81q6Yk2oz27U+4pvs='
$script:OtherPublicKey = '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c'

$script:GoodCommit = '1111111111111111111111111111111111111111'
$script:GoodRcTag = 'v0.9.1-rc1'
$script:PortableName = 'ExoSnap-0.9.1-rc1-windows-x64-portable.zip'
$script:PortableSha = '5cb4a6d95f01ccec747b5e903589caafa6abc79ad134d135d418db022a414ebc'

function New-TestCheck {
    param(
        [Parameter(Mandatory)] [string] $Id,
        [string] $State = 'PASS',
        [bool] $Required = $true,
        [string] $RestoreResult = 'NOT_APPLICABLE'
    )
    return [ordered]@{
        id            = $Id
        title         = "scenario $Id"
        layer         = 'automated'
        state         = $State
        required      = $Required
        optIn         = -not $Required
        attempts      = 1
        message       = 'measured'
        restoreResult = $RestoreResult
        evidence      = @()
    }
}

function New-TestRecord {
    <#
    .SYNOPSIS
        A record that qualifies. Every negative case below is this one, with exactly
        one thing wrong, so a passing negative test cannot be an accident of the
        fixture.
    #>
    param([object[]] $Checks)

    if ($null -eq $Checks) {
        $Checks = @((New-TestCheck -Id 'REL-CAP-001'), (New-TestCheck -Id 'REL-SCHEMA-001'))
    }
    $record = [ordered]@{
        schema             = Get-ReleaseQualificationSchema
        generatedUtc       = [DateTime]::UtcNow.ToString('o')
        runId              = 'rel-20260908-101500'
        rcTag              = $script:GoodRcTag
        sourceCommit       = $script:GoodCommit
        packages           = @([ordered]@{
                kind     = 'portable'
                fileName = $script:PortableName
                bytes    = 1234
                sha256   = $script:PortableSha
            })
        machineFingerprint = 'f' * 64
        harness            = [ordered]@{ version = '1.0.0'; commit = 'c' * 40; dirty = $false }
        catalog            = [ordered]@{ version = '1.0.0'; digest = 'd' * 64; scenarioCount = 2 }
        promotion          = Get-ReleasePromotionDeclaration -RcTag $script:GoodRcTag
        capabilities       = [ordered]@{ 'display.count' = '2' }
        required           = [ordered]@{ ids = @($Checks | Where-Object required | ForEach-Object { $_.id }) }
        checks             = @($Checks)
    }
    $blockers = @(Get-ReleaseQualificationBlockers -Record $record)
    $record['qualification'] = [ordered]@{
        overall = if ($blockers.Count -eq 0) { 'QUALIFIED' } else { 'NOT_QUALIFIED' }
        reasons = [string[]]$blockers
    }
    return $record
}

function Invoke-CheckScript {
    <#
    .SYNOPSIS
        Runs the publish lock in a child pwsh and returns its exit code and output.
    .DESCRIPTION
        A child process rather than a dot-source: the script's whole contract is the
        exit code the workflow gates on, and a function call could never prove that
        `exit 1` actually leaves the process non-zero.
    #>
    param(
        [Parameter(Mandatory)] [string] $Directory,
        $Record,
        [string] $ExpectedCommit = $script:GoodCommit,
        [string] $ExpectedRcTag = $script:GoodRcTag,
        [hashtable] $Sidecars,
        [switch] $OmitRecord,
        # The three ways a signature stops being evidence, each its own message.
        [switch] $OmitSignature,
        [switch] $CorruptSignature,
        [switch] $OmitPublicKey,
        [string] $PublicKeyHex = $script:TestPublicKey
    )

    $recordPath = Join-Path $Directory 'release-verification.json'
    if (-not $OmitRecord) {
        Set-Content -LiteralPath $recordPath -Value ($Record | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
        if (-not $OmitSignature) {
            New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
                -SigningKeyBase64 $script:TestSigningKey | Out-Null
            if ($CorruptSignature) {
                # One flipped nibble: still 128 hex characters, so this is refused by
                # the arithmetic and not by a shape check.
                $signaturePath = "$recordPath.sig"
                $signature = (Get-Content -LiteralPath $signaturePath -Raw).Trim()
                $flipped = if ($signature[0] -eq '0') { '1' } else { '0' }
                Set-Content -LiteralPath $signaturePath -Value ($flipped + $signature.Substring(1)) -NoNewline
            }
        }
    }
    if ($null -eq $Sidecars) { $Sidecars = @{ $script:PortableName = $script:PortableSha } }
    foreach ($name in $Sidecars.Keys) {
        Set-Content -LiteralPath (Join-Path $Directory "$name.sha256") `
            -Value "$($Sidecars[$name])  $name" -Encoding utf8NoBOM
    }
    $summary = Join-Path $Directory 'summary.md'
    $key = if ($OmitPublicKey) { '' } else { $PublicKeyHex }
    $output = & pwsh -NoProfile -NonInteractive -File $script:CheckScript `
        -RecordPath $recordPath -PublicKeyHex $key `
        -ExpectedCommit $ExpectedCommit -ExpectedRcTag $ExpectedRcTag `
        -Sha256Directory $Directory -SummaryPath $summary 2>&1 | Out-String
    $code = $LASTEXITCODE
    $summaryText = if (Test-Path -LiteralPath $summary) { Get-Content -LiteralPath $summary -Raw } else { '' }
    return @{ ExitCode = $code; Output = $output; Summary = $summaryText }
}

Write-Host ''
Write-Host '== The publish lock' -ForegroundColor Cyan

Test-Case 'a good record qualifies' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord)
    Assert-Equal 0 $result.ExitCode "a clean record must qualify: $($result.Output)"
    Assert-Match 'QUALIFIED' $result.Summary 'the job summary must state the verdict'
    Assert-NoMatch 'BLOCKED' $result.Summary 'a qualified record must not be reported as blocked'
}

Test-Case 'missing and duplicate required verdicts block qualification' {
    $record = New-TestRecord
    $record.required.ids += 'REL-MISSING-001'
    Assert-True (@(Get-ReleaseQualificationBlockers -Record $record).Count -gt 0) 'a missing required row must block'
    $record = New-TestRecord
    $record.checks += $record.checks[0]
    Assert-True (@(Get-ReleaseQualificationBlockers -Record $record).Count -gt 0) 'duplicate verdict rows must block'
    $record = New-TestRecord
    $record.checks[0].required = $false
    $record.checks[0].state = 'SKIPPED'
    Assert-True (@(Get-ReleaseQualificationBlockers -Record $record).Count -gt 0) 'a row cannot override the required ID set'
}

Test-Case 'the four ways a record can fail the lock are four different messages' {
    # The point of the case: an operator reading the job summary has to be able to
    # tell "nobody ran the campaign" from "the record was not signed" from "this
    # record is not the one the key signed" from "the campaign found something".
    $missing = Invoke-CheckScript -Directory (New-TestDirectory) -Record (New-TestRecord) -OmitRecord
    $unsigned = Invoke-CheckScript -Directory (New-TestDirectory) -Record (New-TestRecord) -OmitSignature
    $bad = Invoke-CheckScript -Directory (New-TestDirectory) -Record (New-TestRecord) -CorruptSignature
    $notQualified = Invoke-CheckScript -Directory (New-TestDirectory) -Record (New-TestRecord -Checks @(
            (New-TestCheck -Id 'REL-CAP-001'),
            (New-TestCheck -Id 'REL-UPD-PORTABLE-001' -State 'FAIL')))

    foreach ($result in @($missing, $unsigned, $bad, $notQualified)) {
        Assert-Equal 1 $result.ExitCode 'every one of the four must block'
    }
    Assert-Match 'No qualification record' $missing.Summary 'a missing record says so'
    Assert-Match 'UNSIGNED' $unsigned.Summary 'an unsigned record says so'
    Assert-NoMatch 'UNSIGNED' $bad.Summary 'a bad signature is not an unsigned record'
    Assert-Match 'DOES NOT VERIFY' $bad.Summary 'a bad signature says so'
    Assert-Match 'product defect' $notQualified.Summary 'a record that does not qualify names the finding'
    Assert-NoMatch 'signature' $notQualified.Summary 'a signed record must not be reported as a signature problem'
}

Test-Case 'a record signed by a key that is not the release key blocks the release' {
    $directory = New-TestDirectory
    $recordPath = Join-Path $directory 'release-verification.json'
    Set-Content -LiteralPath $recordPath -Value ((New-TestRecord) | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
    New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
        -SigningKeyBase64 $script:OtherSigningKey | Out-Null
    Set-Content -LiteralPath (Join-Path $directory "$($script:PortableName).sha256") `
        -Value "$($script:PortableSha)  $($script:PortableName)" -Encoding utf8NoBOM
    $summary = Join-Path $directory 'summary.md'
    & pwsh -NoProfile -NonInteractive -File $script:CheckScript -RecordPath $recordPath `
        -PublicKeyHex $script:TestPublicKey -ExpectedCommit $script:GoodCommit `
        -ExpectedRcTag $script:GoodRcTag -Sha256Directory $directory -SummaryPath $summary 2>&1 | Out-Null
    Assert-Equal 1 $LASTEXITCODE 'a foreign signature must block'
    Assert-Match 'DOES NOT VERIFY' (Get-Content -LiteralPath $summary -Raw) 'the reason must say the signature failed'
}

Test-Case 'no release public key means nothing can be verified, so nothing is published' {
    $result = Invoke-CheckScript -Directory (New-TestDirectory) -Record (New-TestRecord) -OmitPublicKey
    Assert-Equal 1 $result.ExitCode 'a missing public key must block'
    Assert-Match 'EXOSNAP_UPDATE_PUBLIC_KEY_HEX' $result.Summary 'the reason must name the variable to set'
}

Test-Case 'a record edited after signing blocks the release' {
    # The attack the signature exists for: the record is the one the campaign
    # produced, its signature is the real one, and a single field was retyped.
    $directory = New-TestDirectory
    $recordPath = Join-Path $directory 'release-verification.json'
    $record = New-TestRecord
    Set-Content -LiteralPath $recordPath -Value ($record | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
    New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
        -SigningKeyBase64 $script:TestSigningKey | Out-Null
    $record.checks[0].state = 'FAIL'
    $record.qualification.overall = 'QUALIFIED'
    Set-Content -LiteralPath $recordPath -Value ($record | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $directory "$($script:PortableName).sha256") `
        -Value "$($script:PortableSha)  $($script:PortableName)" -Encoding utf8NoBOM
    $summary = Join-Path $directory 'summary.md'
    & pwsh -NoProfile -NonInteractive -File $script:CheckScript -RecordPath $recordPath `
        -PublicKeyHex $script:TestPublicKey -ExpectedCommit $script:GoodCommit `
        -ExpectedRcTag $script:GoodRcTag -Sha256Directory $directory -SummaryPath $summary 2>&1 | Out-Null
    Assert-Equal 1 $LASTEXITCODE 'an edited record must block'
    Assert-Match 'DOES NOT VERIFY' (Get-Content -LiteralPath $summary -Raw) 'the reason must be the signature, not the contents'
}

Test-Case 'signing refuses a key that is not the private half of the release key' {
    $directory = New-TestDirectory
    $recordPath = Join-Path $directory 'release-verification.json'
    Set-Content -LiteralPath $recordPath -Value '{}' -Encoding utf8NoBOM
    $threw = $false
    try {
        New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
            -SigningKeyBase64 $script:OtherSigningKey -ExpectedPublicKeyHex $script:TestPublicKey | Out-Null
    }
    catch { $threw = $true }
    Assert-True $threw 'signing with the wrong key must fail while it is still cheap'
}

Test-Case 'a missing record blocks the release' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord) -OmitRecord
    Assert-Equal 1 $result.ExitCode 'a missing record must block'
    Assert-Match 'No qualification record' $result.Summary 'the reason must name the missing record'
}

Test-Case 'an unparseable record blocks the release' {
    $directory = New-TestDirectory
    $recordPath = Join-Path $directory 'release-verification.json'
    Set-Content -LiteralPath $recordPath -Value '{ this is not json' -Encoding utf8NoBOM
    # Correctly signed garbage, so the refusal is provably about the JSON and not
    # about the signature that is checked before it.
    New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
        -SigningKeyBase64 $script:TestSigningKey | Out-Null
    Set-Content -LiteralPath (Join-Path $directory "$($script:PortableName).sha256") `
        -Value "$($script:PortableSha)  $($script:PortableName)" -Encoding utf8NoBOM
    $summary = Join-Path $directory 'summary.md'
    & pwsh -NoProfile -NonInteractive -File $script:CheckScript -RecordPath $recordPath `
        -PublicKeyHex $script:TestPublicKey `
        -ExpectedCommit $script:GoodCommit -ExpectedRcTag $script:GoodRcTag `
        -Sha256Directory $directory -SummaryPath $summary 2>&1 | Out-Null
    Assert-Equal 1 $LASTEXITCODE 'an unparseable record must block'
    Assert-Match 'could not be parsed' (Get-Content -LiteralPath $summary -Raw) 'the reason must say so'
}

Test-Case 'a product defect blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-UPD-PORTABLE-001' -State 'FAIL'))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'a FAIL must block'
    Assert-Match 'FAIL' $result.Summary 'the reason must name the failure'
    Assert-Match 'REL-UPD-PORTABLE-001' $result.Summary 'the reason must name the scenario'
}

Test-Case 'an infrastructure error blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-PRESENT-001' -State 'INFRA_ERROR'))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'an INFRA_ERROR must block'
    Assert-Match 'INFRA_ERROR' $result.Summary 'the reason must name the infrastructure failure'
    Assert-NoMatch 'product defect' $result.Summary 'an INFRA_ERROR must not be reported as a product defect'
}

Test-Case 'a required gate reported UNAVAILABLE blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-DISP-DPI-001' -State 'UNAVAILABLE'))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'a required UNAVAILABLE must block'
    Assert-Match 'REL-DISP-DPI-001 is UNAVAILABLE' $result.Summary 'the reason must name the unanswered gate'
}

Test-Case 'a required gate reported DEFERRED blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-UPD-MSI-DECLINE-001' -State 'DEFERRED'))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'a required DEFERRED must block'
    Assert-Match 'REL-UPD-MSI-DECLINE-001 is DEFERRED' $result.Summary 'the reason must name the unanswered gate'
}

Test-Case 'an opt-in gate nobody named does not block the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-AUD-CLOCK-001' -State 'UNAVAILABLE' -Required $false))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 0 $result.ExitCode "an unnamed opt-in gate must not block: $($result.Output)"
}

Test-Case 'a machine left misconfigured blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @(
        (New-TestCheck -Id 'REL-CAP-001'),
        (New-TestCheck -Id 'REL-DISP-HDR-001' -RestoreResult 'RESTORE_FAILED'))
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'an unrestored environment must block'
    Assert-Match 'RESTORE_FAILED' $result.Summary 'the reason must name the restore verdict'
}

Test-Case 'a record about another commit blocks the release' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord) `
        -ExpectedCommit '0123456789abcdef0123456789abcdef01234567'
    Assert-Equal 1 $result.ExitCode 'a commit mismatch must block'
    Assert-Match 'the tag points at' $result.Summary 'the reason must contrast both commits'
}

Test-Case 'a record about another release candidate blocks the release' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord) -ExpectedRcTag 'v0.9.1-rc9'
    Assert-Equal 1 $result.ExitCode 'an RC tag mismatch must block'
    Assert-Match 'v0.9.1-rc9' $result.Summary 'the reason must name the RC that published it'
}

Test-Case 'a record about other bytes blocks the release' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord) `
        -Sidecars @{ $script:PortableName = ('9' * 64) }
    Assert-Equal 1 $result.ExitCode 'an artifact hash mismatch must block'
    Assert-Match 'the RC published' $result.Summary 'the reason must contrast both hashes'
}

Test-Case 'a published asset the record never describes blocks the release' {
    $directory = New-TestDirectory
    $result = Invoke-CheckScript -Directory $directory -Record (New-TestRecord) -Sidecars @{
        $script:PortableName                     = $script:PortableSha
        'ExoSnap-0.9.1-rc1-windows-x64.msi'      = ('a' * 64)
    }
    Assert-Equal 1 $result.ExitCode 'an unqualified published asset must block'
    Assert-Match 'does not describe it' $result.Summary 'the reason must name the undescribed asset'
}

Test-Case 'a record with no package hashes blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord
    $record['packages'] = @()
    $record['qualification'] = [ordered]@{ overall = 'QUALIFIED'; reasons = @() }
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'a record unbound to the published bytes must block'
    Assert-Match 'no release package SHA-256' $result.Summary 'the reason must say the record is unbound'
}

Test-Case 'a record that never claimed qualification blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord
    $record['qualification'] = [ordered]@{ overall = 'NOT_QUALIFIED'; reasons = @('the campaign was abandoned') }
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'an unqualified record must block'
    Assert-Match "record's own verdict" $result.Summary 'the reason must cite the record itself'
}

Test-Case 'a forged QUALIFIED verdict does not survive re-derivation' {
    $directory = New-TestDirectory
    $record = New-TestRecord -Checks @((New-TestCheck -Id 'REL-CAP-001' -State 'FAIL'))
    $record['qualification'] = [ordered]@{ overall = 'QUALIFIED'; reasons = @() }
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'the verdict is re-derived, never trusted'
    Assert-Match 'FAIL' $result.Summary 'the re-derived reason must name the defect'
}

Test-Case 'an unknown schema blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord
    $record['schema'] = 'exosnap.release-verification/99'
    $record['qualification'] = [ordered]@{ overall = 'QUALIFIED'; reasons = @() }
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'an unreadable shape must block'
    Assert-Match 'schema' $result.Summary 'the reason must name the schema'
}

Test-Case 'a record missing its harness identity blocks the release' {
    $directory = New-TestDirectory
    $record = New-TestRecord
    $record['harness'] = [ordered]@{ version = '1.0.0'; commit = '' }
    $record['qualification'] = [ordered]@{ overall = 'QUALIFIED'; reasons = @() }
    $result = Invoke-CheckScript -Directory $directory -Record $record
    Assert-Equal 1 $result.ExitCode 'an unattributable harness must block'
    Assert-Match 'harness.commit' $result.Summary 'the reason must name the missing field'
}

# ---------------------------------------------------------------------------
# The INFRA_ERROR mapping
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== FAIL means only that the product is wrong' -ForegroundColor Cyan

Test-Case 'a scenario that threw is an infrastructure error, not a product defect' {
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
        SetupErrorCode = $null; SetupError = $null; Error = 'the named pipe closed'; Product = $null
    }
    Assert-Equal 'INFRA_ERROR' $outcome.Result 'a thrown scenario measured nothing'
    Assert-Match 'Scenario threw' $outcome.Message 'the message must say what happened'
}

Test-Case 'a scenario that returned no verdict is an infrastructure error' {
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
        SetupErrorCode = $null; SetupError = $null; Error = $null; Product = $null
    }
    Assert-Equal 'INFRA_ERROR' $outcome.Result 'no verdict is not a product defect'
}

Test-Case 'a verdict-less result object is an infrastructure error' {
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
        SetupErrorCode = $null; SetupError = $null; Error = $null; Product = @{ Message = 'oops' }
    }
    Assert-Equal 'INFRA_ERROR' $outcome.Result 'a result object without a verdict measured nothing'
}

Test-Case 'a setter whose read-back disagreed is an infrastructure error' {
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
        SetupErrorCode = 'verify_mismatch'; SetupError = 'requested 240, read back 144'
        Error          = $null; Product = $null
    }
    Assert-Equal 'INFRA_ERROR' $outcome.Result 'a lying mechanism is not a product defect'
    Assert-Match 'verify_mismatch' $outcome.Message 'the message must carry the code'
}

Test-Case 'a capability this machine does not have stays UNAVAILABLE' {
    foreach ($code in @('apply_rejected', 'device_not_present', 'unknown_property', 'not_mutable')) {
        $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
            SetupErrorCode = $code; SetupError = 'no'; Error = $null; Product = $null
        }
        Assert-Equal 'UNAVAILABLE' $outcome.Result "'$code' is a fact about the desk"
    }
}

Test-Case 'a real product verdict passes through untouched' {
    $outcome = Resolve-ReleaseScenarioOutcome -Transaction @{
        SetupErrorCode = $null; SetupError = $null; Error = $null
        Product        = @{ Result = 'FAIL'; Message = 'no video track'; Evidence = @('checks/x/ffprobe.json') }
    }
    Assert-Equal 'FAIL' $outcome.Result 'a measured defect stays a FAIL'
    Assert-Equal 'no video track' $outcome.Message 'the product message is not rewritten'
    Assert-Equal 1 $outcome.Evidence.Count 'the product evidence is carried through'
}

Test-Case 'INFRA_ERROR is a state the runner may record, and a junit failure' {
    Assert-True ('INFRA_ERROR' -in (Get-LiveVerifyCheckStates)) 'INFRA_ERROR must be a known check state'
    $directory = New-TestDirectory
    $catalog = @([pscustomobject]@{ Id = 'REL-X-001'; Title = 'x'; Layer = 'automated' })
    $run = New-LiveVerifyRun -RunId 'rel-test' -RunDirectory $directory -Catalog $catalog
    Complete-LiveVerifyCheck -Run $run -Id 'REL-X-001' -Result 'INFRA_ERROR' -Message 'PresentMon never started' | Out-Null
    Write-LiveVerifyReport -Run $run | Out-Null
    $junit = Get-Content -LiteralPath (Join-Path $directory 'junit.xml') -Raw
    Assert-Match 'failures="1"' $junit 'an INFRA_ERROR must count as a junit failure, never a skip'
    Assert-Match 'INFRA_ERROR: PresentMon never started' $junit 'the failure must say nothing was measured'
}

# ---------------------------------------------------------------------------
# The record producer
# ---------------------------------------------------------------------------

Write-Host ''
Write-Host '== The qualification record' -ForegroundColor Cyan

function New-ProducerRun {
    <#
    .SYNOPSIS
        A campaign whose verdicts the test sets directly, with no scenario bodies and
        no machine involved.
    #>
    param([Parameter(Mandatory)] [string] $Directory, [hashtable] $Artifact)

    $catalog = @(
        [pscustomobject]@{ Id = 'REL-CAP-001'; Title = 'capture'; Layer = 'automated' },
        [pscustomobject]@{ Id = 'REL-AUD-CLOCK-001'; Title = 'soak'; Layer = 'operator'; OptIn = $true }
    )
    if ($null -eq $Artifact) {
        $Artifact = @{
            kind = 'release'; tag = $script:GoodRcTag; sourceCommit = $script:GoodCommit
            exeSha256 = 'e' * 64; installTree = $true; productVersion = '0.9.1-rc1'
        }
    }
    $environment = @{ osVersion = '10.0.26200.0'; monitorCount = 2; envctl = 'available'; ffprobeVersion = '7.1.1' }
    $run = New-LiveVerifyRun -RunId 'rel-20260908-120000' -RunDirectory $Directory -Catalog $catalog `
        -Artifact $Artifact -Environment $environment
    return @{ Run = $run; Catalog = $catalog }
}

Test-Case 'the record carries everything the publish lock has to check' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-CAP-001' -Result 'PASS' -Message 'ok' | Out-Null
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $packages = @([ordered]@{ kind = 'portable'; fileName = $script:PortableName; sha256 = $script:PortableSha })

    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages $packages

    Assert-Equal (Get-ReleaseQualificationSchema) $record.schema 'the record must name its schema'
    Assert-Equal $script:GoodRcTag $record.rcTag 'the RC tag comes from the artifact identity'
    Assert-Equal $script:GoodCommit $record.sourceCommit 'the source commit comes from the artifact identity'
    Assert-True (-not [string]::IsNullOrWhiteSpace($record.machineFingerprint)) 'the machine must be fingerprinted'
    Assert-True (-not [string]::IsNullOrWhiteSpace($record.harness.commit)) 'the harness commit must be recorded'
    Assert-Equal '1.0.0' $record.catalog.version 'the catalog version must be recorded'
    Assert-True (-not [string]::IsNullOrWhiteSpace($record.catalog.digest)) 'the catalog must be digested'
    Assert-True ($record.capabilities.Contains('display.count')) 'the capabilities must be summarised'
    Assert-True (-not [string]::IsNullOrWhiteSpace($record.generatedUtc)) 'the record must be timestamped'
    Assert-Equal 1 ($record.summary['PASS']) 'the verdicts must be summarised'
    Assert-True ($record.restoreSummary.Contains('NOT_APPLICABLE')) 'the restore verdicts must be summarised'
}

Test-Case 'a scenario that is not opt-in is required; an unnamed opt-in one is not' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages @()

    $capture = @($record.checks | Where-Object { $_.id -eq 'REL-CAP-001' })[0]
    $soak = @($record.checks | Where-Object { $_.id -eq 'REL-AUD-CLOCK-001' })[0]
    Assert-True $capture.required 'a scenario that is not opt-in is always required'
    Assert-True (-not $soak.required) 'an opt-in scenario nobody named is not required'
    Assert-True $soak.optIn 'the record must say which scenarios are opt-in'
}

Test-Case 'naming an opt-in scenario makes it required for this release' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RequiredOptIn @('REL-AUD-CLOCK-001') -RepositoryRoot $script:RepositoryRoot -Packages @()

    $soak = @($record.checks | Where-Object { $_.id -eq 'REL-AUD-CLOCK-001' })[0]
    Assert-True $soak.required 'a named opt-in scenario is required'
    Assert-True ('REL-AUD-CLOCK-001' -in $record.required.namedOptIn) 'the record must say who named it'
}

Test-Case 'naming a scenario that does not exist is refused' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    $run = Get-LiveVerifyRun -RunDirectory $directory
    try {
        New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
            -RequiredOptIn @('REL-TYPO-001') -RepositoryRoot $script:RepositoryRoot -Packages @() | Out-Null
    }
    catch {
        Assert-Match 'REL-TYPO-001' $_.Exception.Message 'the error must name the unknown id'
        return
    }
    throw 'a typo that quietly required nothing must not be accepted'
}

Test-Case 'a pending campaign does not qualify' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $packages = @([ordered]@{ kind = 'portable'; fileName = $script:PortableName; sha256 = $script:PortableSha })
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages $packages

    Assert-Equal 'NOT_QUALIFIED' $record.qualification.overall 'a campaign nobody ran qualifies nothing'
    Assert-Match 'REL-CAP-001 is PENDING' ($record.qualification.reasons -join '; ') 'the reason must name the gate'
}

Test-Case 'a campaign with no source commit does not qualify' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory -Artifact @{
        kind = 'release'; tag = $script:GoodRcTag; sourceCommit = $null; exeSha256 = 'e' * 64; installTree = $true
    }
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-CAP-001' -Result 'PASS' -Message 'ok' | Out-Null
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-AUD-CLOCK-001' -Result 'PASS' -Message 'ok' | Out-Null
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $packages = @([ordered]@{ kind = 'portable'; fileName = $script:PortableName; sha256 = $script:PortableSha })
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages $packages

    Assert-Equal 'NOT_QUALIFIED' $record.qualification.overall 'an unattributable campaign qualifies nothing'
    Assert-Match "'sourceCommit' is empty" ($record.qualification.reasons -join '; ') 'the reason must name the field'
}

Test-Case 'evidence is hashed, and evidence that is gone blocks the release' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    New-Item -ItemType Directory -Path (Join-Path $directory 'checks/REL-CAP-001') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $directory 'checks/REL-CAP-001/ffprobe.json') `
        -Value '{"streams":[]}' -Encoding utf8NoBOM
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-CAP-001' -Result 'PASS' -Message 'ok' `
        -Evidence @('checks/REL-CAP-001/ffprobe.json', 'checks/REL-CAP-001/gone.json') | Out-Null
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-AUD-CLOCK-001' -Result 'PASS' -Message 'ok' | Out-Null
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $packages = @([ordered]@{ kind = 'portable'; fileName = $script:PortableName; sha256 = $script:PortableSha })
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages $packages

    $capture = @($record.checks | Where-Object { $_.id -eq 'REL-CAP-001' })[0]
    $present = @($capture.evidence | Where-Object { $_.path -like '*ffprobe.json' })[0]
    Assert-True ($present.sha256 -match '^[0-9a-f]{64}$') 'evidence that exists must be hashed'
    Assert-Equal 'NOT_QUALIFIED' $record.qualification.overall 'a verdict citing absent evidence qualifies nothing'
    Assert-Match 'gone.json' ($record.qualification.reasons -join '; ') 'the reason must name the missing file'
}

Test-Case 'a clean campaign qualifies, and the lock accepts its record' {
    $directory = New-TestDirectory
    $fixture = New-ProducerRun -Directory $directory
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-CAP-001' -Result 'PASS' -Message 'ok' | Out-Null
    Complete-LiveVerifyCheck -Run $fixture.Run -Id 'REL-AUD-CLOCK-001' -Result 'UNAVAILABLE' `
        -Message 'no 44.1 kHz endpoint' | Out-Null
    $run = Get-LiveVerifyRun -RunDirectory $directory
    $packages = @([ordered]@{ kind = 'portable'; fileName = $script:PortableName; sha256 = $script:PortableSha })
    $record = New-ReleaseQualificationRecord -Run $run -Catalog $fixture.Catalog -CatalogVersion '1.0.0' `
        -RepositoryRoot $script:RepositoryRoot -Packages $packages

    Assert-Equal 'QUALIFIED' $record.qualification.overall `
        "a clean campaign must qualify: $($record.qualification.reasons -join '; ')"

    $verdict = Test-ReleaseQualification -Record $record -ExpectedCommit $script:GoodCommit `
        -ExpectedRcTag $script:GoodRcTag -ExpectedPackageSha256 @{ $script:PortableName = $script:PortableSha }
    Assert-True $verdict.Qualified "the lock must accept the record it produced: $($verdict.Reasons -join '; ')"
}

Write-Host ''
Write-Host '== The promotion lock: what ships is what was qualified' -ForegroundColor Cyan

$script:PromotionScript = Join-Path $scriptRoot 'check-release-promotion.ps1'

function New-TestBuildManifest {
    <#
    .SYNOPSIS
        An artifact-manifest.json for one build of the portable tree, in the shape
        build-release-artifacts.ps1 writes it.
    #>
    param(
        [Parameter(Mandatory)] [string] $Version,
        [string] $SourceCommit = $script:GoodCommit,
        [System.Collections.IDictionary] $Files
    )

    if ($null -eq $Files) {
        $Files = [ordered]@{
            'exosnap.exe'          = 'a' * 64
            'exosnap-updater.exe'  = 'b' * 64
            'Qt6Core.dll'          = 'c' * 64
            'qml/ExoSnap/Main.qml' = 'd' * 64
        }
    }
    $entries = @()
    foreach ($name in $Files.Keys) {
        $entries += [ordered]@{
            path   = "ExoSnap-$Version-windows-x64-portable/$name"
            size   = 1024
            sha256 = $Files[$name]
        }
    }
    return [ordered]@{
        product         = 'ExoSnap'
        version         = $Version
        baseVersion     = ($Version -split '-')[0]
        platform        = 'windows-x64'
        sourceCommit    = $SourceCommit
        portableArchive = "ExoSnap-$Version-windows-x64-portable.zip"
        fileCount       = $entries.Count
        files           = $entries
    }
}

function New-TestToolchainManifest {
    param([string] $Version = '0.9.1-rc1', [string] $ClVersion = 'Version 19.44.35207 for x64')
    return [ordered]@{
        product      = 'ExoSnap'
        version      = $Version
        sourceCommit = $script:GoodCommit
        runner       = [ordered]@{ imageLabel = 'windows-2022'; imageOs = 'Windows'; imageVersion = '20260901.1' }
        msvc         = [ordered]@{ clVersion = $ClVersion }
        cmake        = [ordered]@{ version = 'cmake version 3.31.6' }
        qt           = [ordered]@{ version = '6.9.3'; rootDir = 'C:/Qt/6.9.3/msvc2022_64' }
        wix          = [ordered]@{ version = '4.0.5' }
        ffmpeg       = [ordered]@{ url = 'https://example.invalid/ffmpeg.zip'; sha256 = 'e' * 64 }
    }
}

function Invoke-PromotionScript {
    <#
    .SYNOPSIS
        Runs the promotion lock in a child pwsh; the exit code is its whole contract.
    #>
    param(
        [Parameter(Mandatory)] [string] $Directory,
        $Record,
        $QualifiedManifest,
        $CandidateManifest,
        $QualifiedToolchain,
        $CandidateToolchain,
        [string] $CandidateVersion = '0.9.1',
        [switch] $OmitSignature
    )

    if ($null -eq $Record) { $Record = New-TestRecord }
    if ($null -eq $QualifiedManifest) { $QualifiedManifest = New-TestBuildManifest -Version '0.9.1-rc1' }
    if ($null -eq $CandidateManifest) { $CandidateManifest = New-TestBuildManifest -Version $CandidateVersion }
    if ($null -eq $QualifiedToolchain) { $QualifiedToolchain = New-TestToolchainManifest -Version '0.9.1-rc1' }
    if ($null -eq $CandidateToolchain) { $CandidateToolchain = New-TestToolchainManifest -Version $CandidateVersion }

    $recordPath = Join-Path $Directory 'release-verification.json'
    Set-Content -LiteralPath $recordPath -Value ($Record | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
    if (-not $OmitSignature) {
        New-ReleaseQualificationSignatureFile -RecordPath $recordPath `
            -SigningKeyBase64 $script:TestSigningKey | Out-Null
    }
    $paths = @{}
    foreach ($pair in @(
            @{ Key = 'QualifiedManifest'; Name = 'rc-artifact-manifest.json'; Value = $QualifiedManifest },
            @{ Key = 'CandidateManifest'; Name = 'artifact-manifest.json'; Value = $CandidateManifest },
            @{ Key = 'QualifiedToolchain'; Name = 'rc-toolchain-manifest.json'; Value = $QualifiedToolchain },
            @{ Key = 'CandidateToolchain'; Name = 'toolchain-manifest.json'; Value = $CandidateToolchain })) {
        $path = Join-Path $Directory $pair.Name
        Set-Content -LiteralPath $path -Value ($pair.Value | ConvertTo-Json -Depth 30) -Encoding utf8NoBOM
        $paths[$pair.Key] = $path
    }

    $summary = Join-Path $Directory 'promotion.md'
    $output = & pwsh -NoProfile -NonInteractive -File $script:PromotionScript `
        -RecordPath $recordPath -PublicKeyHex $script:TestPublicKey `
        -QualifiedManifestPath $paths['QualifiedManifest'] -CandidateManifestPath $paths['CandidateManifest'] `
        -QualifiedToolchainPath $paths['QualifiedToolchain'] -CandidateToolchainPath $paths['CandidateToolchain'] `
        -CandidateVersion $CandidateVersion -SummaryPath $summary 2>&1 | Out-String
    $code = $LASTEXITCODE
    $summaryText = if (Test-Path -LiteralPath $summary) { Get-Content -LiteralPath $summary -Raw } else { '' }
    return @{ ExitCode = $code; Output = $output; Summary = $summaryText }
}

Test-Case 'a rebuild that differs only where the contract permits is promotable' {
    # The realistic case: the executables the release version is compiled into
    # changed, nothing else did.
    $candidate = New-TestBuildManifest -Version '0.9.1' -Files ([ordered]@{
            'exosnap.exe'          = '1' * 64
            'exosnap-updater.exe'  = '2' * 64
            'Qt6Core.dll'          = 'c' * 64
            'qml/ExoSnap/Main.qml' = 'd' * 64
        })
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -CandidateManifest $candidate
    Assert-Equal 0 $result.ExitCode "a permitted rebuild must promote: $($result.Output)"
    Assert-Match 'as the promotion contract permits' $result.Summary 'the permitted differences must be listed'
}

Test-Case 'a changed shipped file that is not in the contract blocks the release' {
    $candidate = New-TestBuildManifest -Version '0.9.1' -Files ([ordered]@{
            'exosnap.exe'          = '1' * 64
            'exosnap-updater.exe'  = '2' * 64
            'Qt6Core.dll'          = '9' * 64
            'qml/ExoSnap/Main.qml' = 'd' * 64
        })
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -CandidateManifest $candidate
    Assert-Equal 1 $result.ExitCode 'a changed third-party file must block'
    Assert-Match 'Qt6Core\.dll changed' $result.Summary 'the reason must name the file'
}

Test-Case 'a file that appears or disappears blocks the release' {
    $added = New-TestBuildManifest -Version '0.9.1' -Files ([ordered]@{
            'exosnap.exe'          = '1' * 64
            'exosnap-updater.exe'  = '2' * 64
            'Qt6Core.dll'          = 'c' * 64
            'qml/ExoSnap/Main.qml' = 'd' * 64
            'Qt6Sql.dll'           = '8' * 64
        })
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -CandidateManifest $added
    Assert-Equal 1 $result.ExitCode 'an added file must block'
    Assert-Match 'Qt6Sql\.dll is in this release' $result.Summary 'the reason must name the added file'

    $removed = New-TestBuildManifest -Version '0.9.1' -Files ([ordered]@{
            'exosnap.exe'         = '1' * 64
            'exosnap-updater.exe' = '2' * 64
            'Qt6Core.dll'         = 'c' * 64
        })
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -CandidateManifest $removed
    Assert-Equal 1 $result.ExitCode 'a dropped file must block'
    Assert-Match 'was in the qualified candidate and is not' $result.Summary 'the reason must name the dropped file'
}

Test-Case 'a release built from another commit blocks the release' {
    $candidate = New-TestBuildManifest -Version '0.9.1' -SourceCommit ('7' * 40)
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -CandidateManifest $candidate
    Assert-Equal 1 $result.ExitCode 'a moved commit must block'
    Assert-Match 'source commit moved' $result.Summary 'the reason must say the commit moved'
}

Test-Case 'a release built by another toolchain blocks the release' {
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) `
        -CandidateToolchain (New-TestToolchainManifest -Version '0.9.1' -ClVersion 'Version 19.50.00000 for x64')
    Assert-Equal 1 $result.ExitCode 'a moved compiler must block'
    Assert-Match 'msvc\.clVersion changed' $result.Summary 'the reason must name the toolchain field'
}

Test-Case 'a candidate that published no build inventory cannot be promoted from' {
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) `
        -QualifiedManifest ([ordered]@{ version = '0.9.1-rc1'; files = @() })
    Assert-Equal 1 $result.ExitCode 'an inventory-less candidate must block'
    Assert-Match 'lists no files' $result.Summary 'the reason must name the unusable inventory'
}

Test-Case 'the promotion lock refuses an unsigned record too' {
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -OmitSignature
    Assert-Equal 1 $result.ExitCode 'an unsigned record must block promotion'
    Assert-Match 'UNSIGNED' $result.Summary 'the reason must say the record is unsigned'
}

Test-Case 'a record may not widen its own difference budget' {
    $record = New-TestRecord
    $record.promotion.mutableEntries = @('exosnap.exe', 'Qt6Core.dll')
    $record.promotion.contract = 'exosnap.release-promotion/99'
    $result = Invoke-PromotionScript -Directory (New-TestDirectory) -Record $record
    Assert-Equal 1 $result.ExitCode 'an unimplemented contract must block'
    Assert-Match 'may not widen its own difference budget' $result.Summary 'the reason must say so'
}

Test-Case 'a record with no promotion contract does not qualify at all' {
    $record = New-TestRecord
    $record.Remove('promotion')
    Assert-True (@(@(Get-ReleaseQualificationBlockers -Record $record) -match 'promotion contract').Count -gt 0) `
        'a record that declares no contract permits everything and must not qualify'
}

Write-Host ''
Write-Host "  $($script:Passed) passed, $($script:Failed) failed."
if ($script:Failed -gt 0) { exit 1 }
exit 0
