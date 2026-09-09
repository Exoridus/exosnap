#Requires -Version 7.0
<#
.SYNOPSIS
    The qualification record a release is promoted on, and the rules that read it.

.DESCRIPTION
    Its own file so both sides of the lock can reach it: the campaign runner that
    WRITES `release-verification.json`, and `scripts/check-release-qualification.ps1`,
    which the release workflow runs on a clean checkout before it is allowed to
    publish anything.

    One rule decides both. A release is QUALIFIED only when every gate that had to be
    answered was answered by a measurement, against the bytes the tag will ship, on a
    machine that was put back the way it was found. Everything else -- a product
    defect, a broken harness, a gate nobody could run, a missing evidence file --
    blocks promotion.

    Three outcomes are deliberately kept apart, because a release report that merges
    them cannot be trusted where it matters most:

        FAIL          ExoSnap is wrong. Nothing else may claim this.
        INFRA_ERROR   nothing measured the product: the scenario threw, an external
                      tool was missing or unparseable at the moment it was needed, a
                      bounded wait expired, an environment mechanism reported success
                      and then read back something else.
        UNAVAILABLE   this machine cannot offer what the scenario declared it needs.

    All three block a promotion. Only the first one accuses the product.
#>

. (Join-Path $PSScriptRoot 'Ed25519.ps1')

function Get-ReleaseQualificationSignatureExtension {
    <#
    .SYNOPSIS
        The suffix of a record's detached signature sidecar.
    .DESCRIPTION
        Detached, and the same convention as the update manifest's `.sig`: signer and
        verifier never have to agree on a canonical JSON serialisation, because the
        signature covers the file's bytes exactly as they were written and exactly as
        they are read back.
    #>
    return '.sig'
}

function Test-ReleaseQualificationSignature {
    <#
    .SYNOPSIS
        Whether a qualification record carries a signature by the release key.
    .DESCRIPTION
        The lock's first question, and the one every other check depends on. Schema,
        commit identity, RC identity and package hashes are all readable from the RC
        release, so any of them can be reproduced by hand in a record that never had a
        campaign behind it. Only the signature distinguishes a record the tool produced
        from one a person typed.

        The four outcomes are kept apart on purpose. "There is no record" and "the
        record is not signed" are different operator mistakes with different fixes, and
        "the signature does not verify" is not a mistake at all.
    .OUTPUTS
        @{ Verified = [bool]; Reason = [string] } -- Reason is $null when verified.
    #>
    param(
        [Parameter(Mandatory)] [string] $RecordPath,
        [string] $SignaturePath,
        [string] $PublicKeyHex
    )

    if ([string]::IsNullOrWhiteSpace($SignaturePath)) {
        $SignaturePath = $RecordPath + (Get-ReleaseQualificationSignatureExtension)
    }

    if ([string]::IsNullOrWhiteSpace($PublicKeyHex)) {
        return @{ Verified = $false; Reason =
            'no release public key was supplied, so the record signature cannot be verified. ' +
            'The publish gate passes the EXOSNAP_UPDATE_PUBLIC_KEY_HEX repository variable; ' +
            'set it, or pass -PublicKeyHex.'
        }
    }
    if (-not (Test-Path -LiteralPath $SignaturePath -PathType Leaf)) {
        return @{ Verified = $false; Reason =
            "the qualification record is UNSIGNED: no detached signature at '$SignaturePath'. " +
            'A record is signed by `release-verify.ps1 qualify -Publish`, which attaches both files ' +
            'to the RC release. An unsigned record proves nothing about who produced it.'
        }
    }

    $message = [System.IO.File]::ReadAllBytes($RecordPath)
    $signature = (Get-Content -LiteralPath $SignaturePath -Raw).Trim()
    if (-not (Test-Ed25519Signature -Message $message -SignatureHex $signature -PublicKeyHex $PublicKeyHex)) {
        return @{ Verified = $false; Reason =
            'the qualification record signature DOES NOT VERIFY against the release public key. ' +
            'The record, the signature or the key does not belong to the others; nothing about this ' +
            'record may be trusted and nothing is published.'
        }
    }
    return @{ Verified = $true; Reason = $null }
}

function New-ReleaseQualificationSignatureFile {
    <#
    .SYNOPSIS
        Signs a record file in place, writing the detached signature beside it.
    .DESCRIPTION
        Signs the bytes on disk rather than the in-memory record, so the signature
        covers exactly what a verifier will read, serialisation included.
    .PARAMETER SigningKeyBase64
        The base64-encoded 32-byte ed25519 seed, the same form the
        EXOSNAP_UPDATE_SIGNING_KEY secret carries.
    .PARAMETER ExpectedPublicKeyHex
        When set, the signing key must be the private half of it. A record signed with
        the wrong key would be refused by the publish gate at the worst possible
        moment; failing here says so while it is still cheap.
    .OUTPUTS
        The path of the signature file.
    #>
    param(
        [Parameter(Mandatory)] [string] $RecordPath,
        [Parameter(Mandatory)] [string] $SigningKeyBase64,
        [string] $ExpectedPublicKeyHex
    )

    try { $seed = [System.Convert]::FromBase64String($SigningKeyBase64.Trim()) }
    catch { throw 'The release signing key is not valid base64. It is the base64-encoded 32-byte ed25519 seed, exactly as the EXOSNAP_UPDATE_SIGNING_KEY secret holds it.' }
    if ($seed.Length -ne 32) {
        throw "The release signing key decodes to $($seed.Length) bytes; an ed25519 seed is 32."
    }

    $publicKey = ConvertTo-Ed25519Hex -Bytes (Get-Ed25519PublicKey -Seed $seed)
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPublicKeyHex)) {
        $expected = $ExpectedPublicKeyHex.Trim().ToLowerInvariant()
        if ($publicKey -ne $expected) {
            throw "The signing key is not the private half of the expected release public key " +
            "(derives $($publicKey.Substring(0, 16))..., expected $($expected.Substring(0, [Math]::Min(16, $expected.Length)))...). " +
            'The publish gate would refuse every record signed with it.'
        }
    }

    $signaturePath = $RecordPath + (Get-ReleaseQualificationSignatureExtension)
    $signature = New-Ed25519Signature -Seed ([byte[]]$seed) -Message ([System.IO.File]::ReadAllBytes($RecordPath))
    [System.IO.File]::WriteAllText($signaturePath, $signature)
    return $signaturePath
}

function Get-ReleaseQualificationSchema {
    <#
    .SYNOPSIS
        The identifier every qualification record carries, so a reader can refuse a
        shape it does not understand instead of misreading it.
    .DESCRIPTION
        Still /1 although the shape gained a required promotion declaration and a
        detached signature. Both additions are refused by their own rules, each with a
        message that names the fix, and both refusals reach a reader before a schema
        mismatch would. Bumping the identifier belongs with the C# harness gaining the
        ability to produce a promotable record; doing it here first would replace those
        messages with an unhelpful one.
    #>
    return 'exosnap.release-verification/1'
}

function Get-ReleaseHarnessVersion {
    <#
    .SYNOPSIS
        The version of the campaign harness that produced a record. Bump it whenever
        a verdict's meaning changes; the record cites it so an old record can never
        be read under new rules.
    #>
    return '1.1.0'
}

function Get-ReleasePromotionContract {
    <#
    .SYNOPSIS
        What a final release is allowed to differ from the release candidate that was
        qualified, and nothing else.
    .DESCRIPTION
        A final tag does not ship the bytes the campaign measured. It rebuilds them,
        because the release identity is compiled in: `EXOSNAP_RELEASE_VERSION` becomes
        `kVersion` and the executables' VERSIONINFO ProductVersion string, and the
        packaging is named for it, so `0.9.1-rc1` and `0.9.1` are different artifacts
        by construction. Re-publishing the RC's bytes under the final tag would ship
        binaries that still call themselves a release candidate.

        So the rebuild is permitted and its difference budget is declared here, in the
        record, and enforced at publish time against the per-file inventory both builds
        write (`artifact-manifest.json`). Every file in the portable install tree must
        be byte-identical between the qualified RC and the final build except the ones
        named below, which cannot be identical: each embeds the release version string,
        and every build's `kBuildId` is the CI run id.

        What this does NOT establish is that those three files are correct -- their
        bytes are compared to nothing. What narrows that gap is the rest of the
        contract: the same source commit, and the same toolchain, both checked
        alongside. A defect that the release build introduces into one of the three
        binaries without changing the commit, the compiler, Qt, WiX, the vendored
        FFmpeg or any other shipped file remains out of reach, and closing it needs a
        release identity that stops being compiled in.
    .OUTPUTS
        @{ Id; Policy; MutableEntries; MutableReason }
    #>
    return [ordered]@{
        Id             = 'exosnap.release-promotion/1'
        Policy         = 'the final tag rebuilds the qualified commit; only the declared entries may differ'
        MutableEntries = [string[]]@('exosnap.exe', 'exosnap-updater.exe', 'crashpad_handler.exe')
        MutableReason  = 'compiled from this commit, so each carries the release version string and the build id of the run that produced it'
    }
}

function Get-ReleasePromotionDeclaration {
    <#
    .SYNOPSIS
        The promotion contract as a record states it, bound to the version that was
        qualified.
    .DESCRIPTION
        The contract is carried in the record rather than only read from the repository
        at publish time, so a reader can see what a given campaign was allowed to
        differ by. The publish step still refuses a contract id it does not implement,
        which is what stops a record from widening its own budget.
    #>
    param([string] $RcTag)

    $contract = Get-ReleasePromotionContract
    return [ordered]@{
        contract         = $contract.Id
        policy           = $contract.Policy
        qualifiedVersion = "$RcTag" -replace '^v', ''
        mutableEntries   = $contract.MutableEntries
        mutableReason    = $contract.MutableReason
    }
}

function Get-ReleaseQualificationField {
    <#
    .SYNOPSIS
        One field of a record or transaction, or $null, whether it arrived as a
        hashtable or as JSON-parsed objects.
    .DESCRIPTION
        A record is written as a hashtable and read back as PSCustomObject trees, and
        StrictMode throws on a property that a record from an older schema simply does
        not have. Everything here goes through this accessor for that reason.
    #>
    param($Object, [Parameter(Mandatory)] [string] $Name)

    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $null
}

function Resolve-ReleaseScenarioOutcome {
    <#
    .SYNOPSIS
        The product verdict for one finished environment transaction.
    .DESCRIPTION
        The split this function exists for: FAIL means ExoSnap is wrong, and nothing
        else is allowed to claim it. A scenario body that threw, an environment
        mechanism whose read-back disagreed with its own setter, a scenario that
        returned no verdict at all -- none of those measured the product, so none of
        them may be recorded as a product defect. They are INFRA_ERROR, which blocks a
        release just as hard while saying something true.

        A requirement the machine cannot meet is the third answer, and the setup error
        codes are what separates it: `apply_rejected`, `device_not_present`,
        `unknown_property` and `not_mutable` are facts about the desk (UNAVAILABLE);
        every other code is a mechanism that misbehaved (INFRA_ERROR).
    .OUTPUTS
        @{ Result; Message; Evidence }
    #>
    param($Transaction)

    $outcome = @{ Result = 'INFRA_ERROR'; Message = $null; Evidence = @() }

    $setupCode = Get-ReleaseQualificationField -Object $Transaction -Name 'SetupErrorCode'
    if ($null -ne $setupCode -and -not [string]::IsNullOrWhiteSpace("$setupCode")) {
        $setupError = Get-ReleaseQualificationField -Object $Transaction -Name 'SetupError'
        $absent = @('apply_rejected', 'device_not_present', 'unknown_property', 'not_mutable')
        $outcome.Result = if ("$setupCode" -in $absent) { 'UNAVAILABLE' } else { 'INFRA_ERROR' }
        $outcome.Message = "${setupCode}: $setupError"
        return $outcome
    }

    $thrown = Get-ReleaseQualificationField -Object $Transaction -Name 'Error'
    if ($null -ne $thrown -and -not [string]::IsNullOrWhiteSpace("$thrown")) {
        $outcome.Result = 'INFRA_ERROR'
        $outcome.Message = "Scenario threw: $thrown"
        return $outcome
    }

    $product = Get-ReleaseQualificationField -Object $Transaction -Name 'Product'
    if ($null -eq $product) {
        $outcome.Result = 'INFRA_ERROR'
        $outcome.Message = 'The scenario returned no verdict'
        return $outcome
    }

    $result = Get-ReleaseQualificationField -Object $product -Name 'Result'
    if ($null -eq $result -or [string]::IsNullOrWhiteSpace("$result")) {
        $outcome.Result = 'INFRA_ERROR'
        $outcome.Message = 'The scenario returned a result object with no verdict in it'
        return $outcome
    }

    $outcome.Result = "$result"
    $outcome.Message = Get-ReleaseQualificationField -Object $product -Name 'Message'
    $evidence = Get-ReleaseQualificationField -Object $product -Name 'Evidence'
    if ($null -ne $evidence) { $outcome.Evidence = @($evidence) }
    return $outcome
}

function Get-ReleaseRequiredScenarioIds {
    <#
    .SYNOPSIS
        The gates a promotion depends on.
    .DESCRIPTION
        Required is every scenario that is not opt-in, plus the opt-in scenarios the
        developer named for this release. Opt-in classes are the long and the
        physically disruptive ones; they stay out of a default sweep, so making them
        silently required would block every promotion, and making them permanently
        optional would mean a soak or an unplug gate never has to be answered. Naming
        them per release is the only honest form.

        An unknown id is an error rather than an empty set: a typo that quietly
        required nothing is the exact failure this lock exists to prevent.
    #>
    param(
        [Parameter(Mandatory)] [object[]] $Catalog,
        [string[]] $NamedOptIn = @()
    )

    $named = @(@($NamedOptIn) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $known = @($Catalog | ForEach-Object { $_.Id })
    $unknown = @($named | Where-Object { $_ -notin $known })
    if ($unknown.Count -gt 0) {
        throw "Unknown scenario id(s) named as required: $($unknown -join ', ')"
    }

    $required = @()
    foreach ($entry in $Catalog) {
        $optIn = $entry.PSObject.Properties.Name -contains 'OptIn' -and $entry.OptIn
        if (-not $optIn -or $entry.Id -in $named) { $required += $entry.Id }
    }
    return [string[]]$required
}

function Get-ReleaseCapabilityMap {
    <#
    .SYNOPSIS
        What the machine could offer, as the record states it.
    .DESCRIPTION
        A summary over the environment facts, not a second source of them: the record
        carries the full fact table as well. This exists so a reader can answer "could
        this campaign have run the HDR gate at all?" without parsing per-alias keys.
    #>
    param($Environment, $Artifact)

    $capabilities = [ordered]@{}
    $capabilities['os.version'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'osVersion')"
    $capabilities['os.architecture'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'architecture')"
    $capabilities['cpu.count'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'processorCount')"
    $capabilities['display.count'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'monitorCount')"
    $capabilities['display.topology'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'monitorTopology')"
    $capabilities['envctl.available'] =
        ("$(Get-ReleaseQualificationField -Object $Environment -Name 'envctl')" -eq 'available')
    $capabilities['ffprobe.available'] =
        -not [string]::IsNullOrWhiteSpace("$(Get-ReleaseQualificationField -Object $Environment -Name 'ffprobeVersion')")
    $capabilities['aliases.bound'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'aliasProfile')"
    $capabilities['runner.elevated'] = "$(Get-ReleaseQualificationField -Object $Environment -Name 'elevated')"
    $capabilities['artifact.installTree'] =
        [bool](Get-ReleaseQualificationField -Object $Artifact -Name 'installTree')
    return $capabilities
}

function Get-ReleaseEvidenceDigest {
    <#
    .SYNOPSIS
        The evidence a check cites, hashed.
    .DESCRIPTION
        A verdict that cites a file nobody can hash is not evidence, so a missing file
        is recorded as such rather than dropped -- and it blocks the promotion. Paths
        are run-relative, exactly as the checks record them, so the record stays
        readable after the run directory moves.
    #>
    param(
        [Parameter(Mandatory)] [string] $RunDirectory,
        $Evidence
    )

    $rows = @()
    foreach ($relative in @($Evidence)) {
        if ([string]::IsNullOrWhiteSpace("$relative")) { continue }
        $full = Join-Path $RunDirectory "$relative"
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $item = Get-Item -LiteralPath $full
            $rows += [ordered]@{
                path    = "$relative"
                bytes   = $item.Length
                sha256  = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
                missing = $false
            }
        }
        else {
            $rows += [ordered]@{ path = "$relative"; bytes = $null; sha256 = $null; missing = $true }
        }
    }
    return $rows
}

function Get-ReleaseHarnessIdentity {
    <#
    .SYNOPSIS
        Which harness produced the record: version, the commit it was checked out at,
        and whether that tree was modified.
    .DESCRIPTION
        A dirty tree does not block a promotion on its own -- it is stated instead, so
        a reader can see that the gates were not exactly the ones in the commit. A
        commit that cannot be determined at all does block: an unattributable harness
        is not a lock.
    #>
    param([string] $RepositoryRoot)

    $identity = [ordered]@{
        version = Get-ReleaseHarnessVersion
        runner  = 'scripts/release-verify.ps1'
        commit  = $null
        dirty   = $null
    }
    if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) { return $identity }
    try {
        $commit = (& git -C $RepositoryRoot rev-parse HEAD 2>$null)
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace("$commit")) {
            $identity['commit'] = "$commit".Trim()
            $status = @(& git -C $RepositoryRoot status --porcelain 2>$null)
            $identity['dirty'] = ($LASTEXITCODE -eq 0 -and $status.Count -gt 0)
        }
    }
    catch {
        # No git, no commit. The blocker below says so; guessing would be worse.
    }
    return $identity
}

function Get-ReleaseCatalogIdentity {
    <#
    .SYNOPSIS
        Which set of gates the verdicts describe.
    .DESCRIPTION
        The digest covers every scenario id and its opt-in flag, so a catalog that
        gained, lost or reclassified a gate cannot be mistaken for the one a record
        was produced against, even when the declared version was not bumped.
    #>
    param(
        [Parameter(Mandatory)] [object[]] $Catalog,
        [Parameter(Mandatory)] [string] $Version
    )

    $lines = @($Catalog | ForEach-Object {
            $optIn = $_.PSObject.Properties.Name -contains 'OptIn' -and $_.OptIn
            "$($_.Id)=$([bool]$optIn)"
        } | Sort-Object)
    $stream = [System.IO.MemoryStream]::new([System.Text.Encoding]::UTF8.GetBytes($lines -join ';'))
    try {
        $digest = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    finally { $stream.Dispose() }
    return [ordered]@{ version = $Version; digest = $digest; scenarioCount = $Catalog.Count }
}

function Get-ReleaseQualificationBlockers {
    <#
    .SYNOPSIS
        Every reason this record must not be promoted. An empty list is the only
        thing that qualifies a release.
    .DESCRIPTION
        Content rules only: what the record itself says. The comparisons against the
        tag being published -- commit, RC tag, published asset hashes -- belong to
        Test-ReleaseQualification, which is what the workflow calls.
    #>
    param($Record)

    $reasons = @()
    if ($null -eq $Record) { return @('no qualification record was provided') }

    $schema = "$(Get-ReleaseQualificationField -Object $Record -Name 'schema')"
    if ($schema -ne (Get-ReleaseQualificationSchema)) {
        $reasons += "record schema is '$schema', not '$(Get-ReleaseQualificationSchema)'"
    }

    foreach ($field in @('runId', 'rcTag', 'sourceCommit', 'generatedUtc', 'machineFingerprint')) {
        $value = Get-ReleaseQualificationField -Object $Record -Name $field
        if ([string]::IsNullOrWhiteSpace("$value")) { $reasons += "record field '$field' is empty" }
    }

    $harness = Get-ReleaseQualificationField -Object $Record -Name 'harness'
    foreach ($field in @('version', 'commit')) {
        $value = Get-ReleaseQualificationField -Object $harness -Name $field
        if ([string]::IsNullOrWhiteSpace("$value")) { $reasons += "record field 'harness.$field' is empty" }
    }

    $catalog = Get-ReleaseQualificationField -Object $Record -Name 'catalog'
    foreach ($field in @('version', 'digest')) {
        $value = Get-ReleaseQualificationField -Object $catalog -Name $field
        if ([string]::IsNullOrWhiteSpace("$value")) { $reasons += "record field 'catalog.$field' is empty" }
    }

    # The promotion contract is what the publish step compares the final build
    # against. A record that declares none permits everything, which is the state
    # this whole section exists to end.
    $promotion = Get-ReleaseQualificationField -Object $Record -Name 'promotion'
    $contract = "$(Get-ReleaseQualificationField -Object $promotion -Name 'contract')"
    if ($contract -ne (Get-ReleasePromotionContract).Id) {
        $reasons += "record promotion contract is '$(if ($contract) { $contract } else { 'absent' })', " +
        "not '$((Get-ReleasePromotionContract).Id)'"
    }
    if ([string]::IsNullOrWhiteSpace("$(Get-ReleaseQualificationField -Object $promotion -Name 'qualifiedVersion')")) {
        $reasons += "record field 'promotion.qualifiedVersion' is empty"
    }
    if (@(Get-ReleaseQualificationField -Object $promotion -Name 'mutableEntries').Count -eq 0) {
        $reasons += "record field 'promotion.mutableEntries' is empty, so nothing constrains what a rebuild may change"
    }

    $packages = @(Get-ReleaseQualificationField -Object $Record -Name 'packages')
    $hashed = @($packages | Where-Object {
            -not [string]::IsNullOrWhiteSpace("$(Get-ReleaseQualificationField -Object $_ -Name 'sha256')")
        })
    if ($hashed.Count -eq 0) {
        $reasons += 'the record carries no release package SHA-256; the campaign was not bound to the published RC assets'
    }

    $checks = @(Get-ReleaseQualificationField -Object $Record -Name 'checks')
    if ($checks.Count -eq 0) {
        $reasons += 'the record contains no scenario verdicts'
        return $reasons
    }

    $required = Get-ReleaseQualificationField -Object $Record -Name 'required'
    $requiredIds = @(Get-ReleaseQualificationField -Object $required -Name 'ids')
    if ($requiredIds.Count -eq 0) { $reasons += 'the record carries no required scenario IDs' }
    $checkIds = @($checks | ForEach-Object { "$(Get-ReleaseQualificationField -Object $_ -Name 'id')" })
    foreach ($duplicate in @($checkIds | Group-Object | Where-Object Count -gt 1)) {
        $reasons += "scenario verdict id is duplicated: $($duplicate.Name)"
    }
    foreach ($duplicate in @($requiredIds | Group-Object | Where-Object Count -gt 1)) {
        $reasons += "required gate id is duplicated: $($duplicate.Name)"
    }
    foreach ($id in $requiredIds) {
        $matches = @($checks | Where-Object { "$(Get-ReleaseQualificationField -Object $_ -Name 'id')" -eq "$id" })
        if ($matches.Count -ne 1) {
            $reasons += "required gate $id has $($matches.Count) verdict rows, expected exactly one"
        }
        elseif ("$(Get-ReleaseQualificationField -Object $matches[0] -Name 'state')" -ne 'PASS') {
            $reasons += "required gate $id is not PASS"
        }
    }

    $failed = @($checks | Where-Object { "$(Get-ReleaseQualificationField -Object $_ -Name 'state')" -eq 'FAIL' })
    if ($failed.Count -gt 0) {
        $reasons += "product defect (FAIL): $(($failed | ForEach-Object { $_.id }) -join ', ')"
    }

    $infra = @($checks | Where-Object { "$(Get-ReleaseQualificationField -Object $_ -Name 'state')" -eq 'INFRA_ERROR' })
    if ($infra.Count -gt 0) {
        $reasons += "harness or environment failure (INFRA_ERROR), so nothing was measured: " +
        "$(($infra | ForEach-Object { $_.id }) -join ', ')"
    }

    foreach ($check in $checks) {
        if (-not [bool](Get-ReleaseQualificationField -Object $check -Name 'required')) { continue }
        $state = "$(Get-ReleaseQualificationField -Object $check -Name 'state')"
        if ($state -in @('PASS', 'FAIL', 'INFRA_ERROR')) { continue }
        $id = "$(Get-ReleaseQualificationField -Object $check -Name 'id')"
        $reasons += "required gate $id is $state, so it was never answered"
    }

    foreach ($check in $checks) {
        $restore = "$(Get-ReleaseQualificationField -Object $check -Name 'restoreResult')"
        if ([string]::IsNullOrWhiteSpace($restore) -or $restore -in @('NOT_APPLICABLE', 'RESTORED')) { continue }
        $id = "$(Get-ReleaseQualificationField -Object $check -Name 'id')"
        $reasons += "$id left the machine misconfigured: environment restore $restore"
    }

    foreach ($check in $checks) {
        $missing = @(@(Get-ReleaseQualificationField -Object $check -Name 'evidence') |
                Where-Object { [bool](Get-ReleaseQualificationField -Object $_ -Name 'missing') })
        if ($missing.Count -eq 0) { continue }
        $id = "$(Get-ReleaseQualificationField -Object $check -Name 'id')"
        $reasons += "$id cites evidence that is not there: $(($missing | ForEach-Object { $_.path }) -join ', ')"
    }

    return $reasons
}

function Test-ReleaseQualification {
    <#
    .SYNOPSIS
        Whether this record may promote this commit. The release workflow's gate.
    .DESCRIPTION
        Everything Get-ReleaseQualificationBlockers finds, plus the three questions
        only the publisher can ask: is this record about the commit being tagged, is
        it about the RC whose assets were verified, and do the hashes it claims match
        the bytes that RC actually published.

        The record's own `qualification.overall` is checked as well as re-derived. A
        record that says QUALIFIED while carrying a FAIL is rejected by the second
        rule, and a record whose verdicts are clean but which never claimed
        qualification is rejected by the first: promotion is an explicit act.
    .PARAMETER ExpectedPackageSha256
        fileName -> lowercase SHA-256, read from the RC release's `.sha256` sidecars.
        Every entry must be matched by a package in the record. Omit to skip the
        asset comparison (local inspection only; the workflow always supplies it).
    .OUTPUTS
        @{ Qualified = [bool]; Reasons = [string[]] }
    #>
    param(
        $Record,
        [string] $ExpectedCommit,
        [string] $ExpectedRcTag,
        [hashtable] $ExpectedPackageSha256
    )

    if ($null -eq $Record) {
        return @{ Qualified = $false; Reasons = @('no qualification record was found for this commit') }
    }

    $reasons = @(Get-ReleaseQualificationBlockers -Record $Record)

    $overall = "$(Get-ReleaseQualificationField -Object (
            Get-ReleaseQualificationField -Object $Record -Name 'qualification') -Name 'overall')"
    if ($overall -ne 'QUALIFIED') {
        $reasons += "the record's own verdict is '$(if ($overall) { $overall } else { 'absent' })', not QUALIFIED"
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedCommit)) {
        $recorded = "$(Get-ReleaseQualificationField -Object $Record -Name 'sourceCommit')"
        if ($recorded -ne $ExpectedCommit) {
            $reasons += "the record qualifies commit $recorded, but the tag points at $ExpectedCommit"
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedRcTag)) {
        $recorded = "$(Get-ReleaseQualificationField -Object $Record -Name 'rcTag')"
        if ($recorded -ne $ExpectedRcTag) {
            $reasons += "the record qualifies RC '$recorded', but it was published under '$ExpectedRcTag'"
        }
    }

    if ($null -ne $ExpectedPackageSha256 -and $ExpectedPackageSha256.Count -gt 0) {
        $packages = @{}
        foreach ($package in @(Get-ReleaseQualificationField -Object $Record -Name 'packages')) {
            $name = "$(Get-ReleaseQualificationField -Object $package -Name 'fileName')"
            if ([string]::IsNullOrWhiteSpace($name)) { continue }
            $packages[$name] = "$(Get-ReleaseQualificationField -Object $package -Name 'sha256')".ToLowerInvariant()
        }
        foreach ($name in ($ExpectedPackageSha256.Keys | Sort-Object)) {
            $expected = "$($ExpectedPackageSha256[$name])".ToLowerInvariant()
            if (-not $packages.ContainsKey($name)) {
                $reasons += "the RC published $name, but the record does not describe it: " +
                'the campaign did not run against the published assets'
                continue
            }
            if ($packages[$name] -ne $expected) {
                $reasons += "$name : the record qualifies SHA-256 $($packages[$name]), " +
                "but the RC published $expected"
            }
        }
    }

    return @{ Qualified = ($reasons.Count -eq 0); Reasons = [string[]]$reasons }
}

function Get-ReleaseInstallTreeEntries {
    <#
    .SYNOPSIS
        relative path -> lowercase SHA-256, from a build's `artifact-manifest.json`.
    .DESCRIPTION
        The manifest states each path under the package root, which carries the release
        version (`ExoSnap-0.9.1-rc1-windows-x64-portable/...`). The root is stripped, so
        two builds of different versions are comparable at all, and it is verified
        rather than assumed: a manifest whose root does not name the version it claims
        is not describing the package it says it is.
    .OUTPUTS
        @{ Entries = @{ path -> sha256 }; Errors = [string[]] }
    #>
    param($Manifest, [Parameter(Mandatory)] [string] $Version, [Parameter(Mandatory)] [string] $Label)

    $result = @{ Entries = @{}; Errors = @() }
    if ($null -eq $Manifest) {
        $result.Errors += "$Label build manifest is missing or empty"
        return $result
    }

    $declared = "$(Get-ReleaseQualificationField -Object $Manifest -Name 'version')"
    if ($declared -ne $Version) {
        $result.Errors += "$Label build manifest declares version '$declared', expected '$Version'"
    }

    $expectedRoot = "ExoSnap-$Version-windows-x64-portable"
    $files = @(Get-ReleaseQualificationField -Object $Manifest -Name 'files')
    if ($files.Count -eq 0) {
        $result.Errors += "$Label build manifest lists no files"
        return $result
    }

    foreach ($file in $files) {
        $path = "$(Get-ReleaseQualificationField -Object $file -Name 'path')"
        $sha = "$(Get-ReleaseQualificationField -Object $file -Name 'sha256')".ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        $separator = $path.IndexOf('/')
        if ($separator -lt 0 -or $path.Substring(0, $separator) -ne $expectedRoot) {
            $result.Errors += "$Label build manifest entry '$path' is not under '$expectedRoot/'"
            continue
        }
        $result.Entries[$path.Substring($separator + 1)] = $sha
    }
    return $result
}

function Compare-ReleaseInstallTree {
    <#
    .SYNOPSIS
        Whether a final build differs from the qualified candidate only where the
        promotion contract permits.
    .DESCRIPTION
        The comparison the publish step runs before it un-drafts a final release. It
        answers the question no gate used to ask: are the bytes about to ship the bytes
        the campaign measured, apart from the difference the release identity forces?

        A file present on one side only, a file whose content changed and is not on the
        contract's list, a source commit that moved, and a manifest that does not
        describe the version it claims are all refusals.
    .PARAMETER MutableEntries
        Install-tree relative paths permitted to differ, as the qualification record
        declares them.
    .OUTPUTS
        @{ Differences = [string[]]; Notes = [string[]] }
    #>
    param(
        $QualifiedManifest,
        $CandidateManifest,
        [Parameter(Mandatory)] [string] $QualifiedVersion,
        [Parameter(Mandatory)] [string] $CandidateVersion,
        [string[]] $MutableEntries = @()
    )

    $differences = @()
    $notes = @()

    $qualified = Get-ReleaseInstallTreeEntries -Manifest $QualifiedManifest -Version $QualifiedVersion -Label 'qualified'
    $candidate = Get-ReleaseInstallTreeEntries -Manifest $CandidateManifest -Version $CandidateVersion -Label 'candidate'
    $differences += $qualified.Errors
    $differences += $candidate.Errors
    if ($differences.Count -gt 0) { return @{ Differences = [string[]]$differences; Notes = [string[]]$notes } }

    $qualifiedCommit = "$(Get-ReleaseQualificationField -Object $QualifiedManifest -Name 'sourceCommit')"
    $candidateCommit = "$(Get-ReleaseQualificationField -Object $CandidateManifest -Name 'sourceCommit')"
    if ($qualifiedCommit -ne $candidateCommit) {
        $differences += "source commit moved: the candidate was built from $qualifiedCommit, this release from $candidateCommit"
    }

    $mutable = @{}
    foreach ($entry in @($MutableEntries)) {
        if (-not [string]::IsNullOrWhiteSpace($entry)) { $mutable[$entry.ToLowerInvariant()] = $true }
    }

    foreach ($path in ($qualified.Entries.Keys | Sort-Object)) {
        if (-not $candidate.Entries.ContainsKey($path)) {
            $differences += "$path was in the qualified candidate and is not in this release"
            continue
        }
        if ($qualified.Entries[$path] -eq $candidate.Entries[$path]) { continue }
        if ($mutable.ContainsKey($path.ToLowerInvariant())) {
            $notes += "$path differs, as the promotion contract permits"
            continue
        }
        $differences += "$path changed: the campaign qualified $($qualified.Entries[$path]), this release ships $($candidate.Entries[$path])"
    }
    foreach ($path in ($candidate.Entries.Keys | Sort-Object)) {
        if (-not $qualified.Entries.ContainsKey($path)) {
            $differences += "$path is in this release and was not in the qualified candidate"
        }
    }

    # A permitted entry that did not change is not an error, but it is worth saying:
    # the version string is compiled into these, so an identical hash means the build
    # did not do what the contract assumes it does.
    foreach ($entry in @($MutableEntries)) {
        $key = "$entry"
        if ($qualified.Entries.ContainsKey($key) -and $candidate.Entries.ContainsKey($key) -and
            $qualified.Entries[$key] -eq $candidate.Entries[$key]) {
            $notes += "$key is byte-identical although the release version differs"
        }
    }

    return @{ Differences = [string[]]$differences; Notes = [string[]]$notes }
}

function Compare-ReleaseToolchain {
    <#
    .SYNOPSIS
        Whether the final release was built by the toolchain that built the qualified
        candidate.
    .DESCRIPTION
        The install-tree comparison exempts the binaries this repository compiles,
        because their bytes cannot match. This is what keeps that exemption narrow: the
        compiler, CMake, Qt, WiX and the vendored FFmpeg that produced them have to be
        the same ones, so "only the version string differs" stays a statement about the
        source and not about the machine.

        The runner image version is reported rather than refused. GitHub patches an
        image between two runs of the same tag without changing anything the compared
        fields cover, and blocking on it would make a re-run of a stalled publish
        impossible for a reason unrelated to the product.
    .OUTPUTS
        @{ Differences = [string[]]; Notes = [string[]] }
    #>
    param($QualifiedToolchain, $CandidateToolchain)

    $differences = @()
    $notes = @()
    if ($null -eq $QualifiedToolchain) {
        return @{ Differences = [string[]]@('the qualified candidate published no toolchain manifest, so the release cannot be shown to have been built the same way'); Notes = [string[]]@() }
    }
    if ($null -eq $CandidateToolchain) {
        return @{ Differences = [string[]]@('this release produced no toolchain manifest'); Notes = [string[]]@() }
    }

    $fields = [ordered]@{
        'msvc.clVersion' = @('msvc', 'clVersion')
        'cmake.version'  = @('cmake', 'version')
        'qt.version'     = @('qt', 'version')
        'wix.version'    = @('wix', 'version')
        'ffmpeg.url'     = @('ffmpeg', 'url')
        'ffmpeg.sha256'  = @('ffmpeg', 'sha256')
        'runner.imageLabel' = @('runner', 'imageLabel')
    }
    foreach ($name in $fields.Keys) {
        $path = $fields[$name]
        $left = "$(Get-ReleaseQualificationField -Object (
                Get-ReleaseQualificationField -Object $QualifiedToolchain -Name $path[0]) -Name $path[1])"
        $right = "$(Get-ReleaseQualificationField -Object (
                Get-ReleaseQualificationField -Object $CandidateToolchain -Name $path[0]) -Name $path[1])"
        if ($left -ne $right) {
            $differences += "$name changed: the candidate was built with '$left', this release with '$right'"
        }
    }

    $leftImage = "$(Get-ReleaseQualificationField -Object (
            Get-ReleaseQualificationField -Object $QualifiedToolchain -Name 'runner') -Name 'imageVersion')"
    $rightImage = "$(Get-ReleaseQualificationField -Object (
            Get-ReleaseQualificationField -Object $CandidateToolchain -Name 'runner') -Name 'imageVersion')"
    if ($leftImage -ne $rightImage) {
        $notes += "runner image version moved from '$leftImage' to '$rightImage'"
    }

    return @{ Differences = [string[]]$differences; Notes = [string[]]$notes }
}

function New-ReleaseQualificationRecord {
    <#
    .SYNOPSIS
        The machine-readable release verdict: `release-verification.json`.
    .DESCRIPTION
        Everything a publisher has to be able to check without trusting the person
        who ran the campaign -- which RC, which commit, which bytes, which machine,
        which harness, which catalog, every verdict, every environment restore, and a
        hash for every piece of evidence cited.

        The overall qualification is derived here from the same rules the workflow
        applies, so a record cannot claim more than its own contents support.
    .PARAMETER RequiredOptIn
        Opt-in scenario ids that this release must also have answered.
    #>
    param(
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] [object[]] $Catalog,
        [Parameter(Mandatory)] [string] $CatalogVersion,
        [string[]] $RequiredOptIn = @(),
        [string] $RepositoryRoot,
        $Packages
    )

    $requiredIds = Get-ReleaseRequiredScenarioIds -Catalog $Catalog -NamedOptIn $RequiredOptIn
    $optInIds = @($Catalog |
            Where-Object { $_.PSObject.Properties.Name -contains 'OptIn' -and $_.OptIn } |
            ForEach-Object { $_.Id })

    $rows = @()
    $summary = [ordered]@{}
    $restoreSummary = [ordered]@{}
    foreach ($property in $Run.State.checks.PSObject.Properties) {
        $check = $property.Value
        $state = "$(Get-ReleaseQualificationField -Object $check -Name 'state')"
        $restore = "$(Get-ReleaseQualificationField -Object $check -Name 'restoreResult')"
        if ([string]::IsNullOrWhiteSpace($restore)) { $restore = 'NOT_APPLICABLE' }
        $summary[$state] = [int]$summary[$state] + 1
        $restoreSummary[$restore] = [int]$restoreSummary[$restore] + 1
        $rows += [ordered]@{
            id                  = "$(Get-ReleaseQualificationField -Object $check -Name 'id')"
            title               = "$(Get-ReleaseQualificationField -Object $check -Name 'title')"
            layer               = "$(Get-ReleaseQualificationField -Object $check -Name 'layer')"
            state               = $state
            required            = ("$(Get-ReleaseQualificationField -Object $check -Name 'id')" -in $requiredIds)
            optIn               = ("$(Get-ReleaseQualificationField -Object $check -Name 'id')" -in $optInIds)
            attempts            = Get-ReleaseQualificationField -Object $check -Name 'attempts'
            startedUtc          = Get-ReleaseQualificationField -Object $check -Name 'startedUtc'
            finishedUtc         = Get-ReleaseQualificationField -Object $check -Name 'finishedUtc'
            message             = Get-ReleaseQualificationField -Object $check -Name 'message'
            skipReason          = Get-ReleaseQualificationField -Object $check -Name 'skipReason'
            interrupted         = [bool](Get-ReleaseQualificationField -Object $check -Name 'interrupted')
            restoreResult       = $restore
            environmentEvidence = Get-ReleaseQualificationField -Object $check -Name 'environmentEvidence'
            evidence            = @(Get-ReleaseEvidenceDigest -RunDirectory $Run.Directory `
                    -Evidence (Get-ReleaseQualificationField -Object $check -Name 'evidence'))
        }
    }

    $environmentTable = @{}
    if ($null -ne $Run.Environment) {
        foreach ($property in $Run.Environment.PSObject.Properties) {
            $environmentTable[$property.Name] = "$($property.Value)"
        }
    }

    $record = [ordered]@{
        schema             = Get-ReleaseQualificationSchema
        generatedUtc       = [DateTime]::UtcNow.ToString('o')
        runId              = "$(Get-ReleaseQualificationField -Object $Run.Run -Name 'runId')"
        rcTag              = "$(Get-ReleaseQualificationField -Object $Run.Artifact -Name 'tag')"
        sourceCommit       = "$(Get-ReleaseQualificationField -Object $Run.Artifact -Name 'sourceCommit')"
        artifact           = $Run.Artifact
        packages           = @($Packages)
        machineFingerprint = Get-LiveVerifyFingerprint -Properties $environmentTable
        harness            = Get-ReleaseHarnessIdentity -RepositoryRoot $RepositoryRoot
        catalog            = Get-ReleaseCatalogIdentity -Catalog $Catalog -Version $CatalogVersion
        promotion          = Get-ReleasePromotionDeclaration -RcTag "$(
            Get-ReleaseQualificationField -Object $Run.Artifact -Name 'tag')"
        capabilities       = Get-ReleaseCapabilityMap -Environment $Run.Environment -Artifact $Run.Artifact
        environment        = $Run.Environment
        required           = [ordered]@{
            policy     = 'every scenario that is not opt-in, plus the opt-in scenarios named for this release'
            ids        = $requiredIds
            namedOptIn = [string[]]@(@($RequiredOptIn) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        }
        summary            = $summary
        restoreSummary     = $restoreSummary
        checks             = $rows
    }

    $blockers = @(Get-ReleaseQualificationBlockers -Record $record)
    $record['qualification'] = [ordered]@{
        overall      = if ($blockers.Count -eq 0) { 'QUALIFIED' } else { 'NOT_QUALIFIED' }
        reasons      = [string[]]$blockers
        evaluatedUtc = [DateTime]::UtcNow.ToString('o')
    }
    return $record
}
