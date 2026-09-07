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

function Get-ReleaseQualificationSchema {
    <#
    .SYNOPSIS
        The identifier every qualification record carries, so a reader can refuse a
        shape it does not understand instead of misreading it.
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
    return '1.0.0'
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
