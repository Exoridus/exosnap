namespace ExoSnap.Verify.Models;

/// <summary>Whether a release candidate may be promoted.</summary>
public enum QualificationOutcome
{
    /// <summary>Every required scenario passed against these exact bytes.</summary>
    Qualified,

    /// <summary>Something is missing, failed, or was never measured.</summary>
    NotQualified,
}

/// <summary>One artifact the verdicts describe.</summary>
/// <param name="Name">File name as published.</param>
/// <param name="Sha256">Lowercase hex SHA-256 of the artifact content.</param>
public sealed record ArtifactDigest(string Name, string Sha256);

/// <summary>Which machine produced the verdicts.</summary>
/// <param name="Fingerprint">Stable, non-identifying digest of the machine's hardware and OS facts.</param>
/// <param name="OsVersion">Operating system version string.</param>
/// <param name="Gpus">Adapter descriptions, in enumeration order.</param>
public sealed record MachineFingerprint(
    string Fingerprint,
    string OsVersion,
    IReadOnlyList<string> Gpus);

/// <summary>Which build of the harness produced the verdicts.</summary>
/// <param name="Version">Informational version of the harness assembly.</param>
/// <param name="Commit">Source commit the harness was built from, or "unknown".</param>
public sealed record HarnessIdentity(string Version, string Commit);

/// <summary>What one scenario concluded during the run.</summary>
/// <param name="Id">Scenario id.</param>
/// <param name="Outcome">The verdict.</param>
/// <param name="Message">Why.</param>
/// <param name="DurationMs">Wall-clock duration of the scenario body.</param>
/// <param name="Evidence">Files backing the verdict.</param>
public sealed record ScenarioVerdict(
    string Id,
    ScenarioOutcome Outcome,
    string Message,
    long DurationMs,
    IReadOnlyList<Evidence> Evidence)
{
    /// <summary>
    /// Why this scenario's evidence did not reach the host, or an empty string.
    /// </summary>
    /// <remarks>
    /// Carried alongside the outcome rather than folded into it. A scenario whose
    /// product assertions all held and whose evidence was destroyed is not a product
    /// failure and is not a complete pass either, and only the promotion contract is
    /// in a position to say which of those matters.
    /// </remarks>
    public string EvidenceGap { get; init; } = string.Empty;
}

/// <summary>
/// Whether an environment property a scenario changed came back to what it was.
/// </summary>
/// <param name="Property">The property, as the environment orchestrator names it.</param>
/// <param name="Expected">The value recorded before the mutation.</param>
/// <param name="Observed">The value read back after the restore.</param>
/// <param name="Restored">Whether the two agree.</param>
/// <remarks>
/// Separate from the scenario verdicts on purpose. A scenario can pass and still
/// leave the machine changed, and a campaign that left a display in HDR is not a
/// campaign anyone may build the next verdict on.
/// </remarks>
public sealed record EnvironmentRestoreVerdict(string Property, string Expected, string Observed, bool Restored);

/// <summary>
/// The document a release promotion is allowed to rest on.
/// </summary>
/// <param name="SchemaVersion">Version of this document's shape.</param>
/// <param name="RcTag">The release-candidate tag the artifacts were published under.</param>
/// <param name="SourceCommitSha">The commit the artifacts were built from.</param>
/// <param name="Artifacts">The exact bytes the verdicts describe.</param>
/// <param name="Machine">Which machine produced the verdicts.</param>
/// <param name="Harness">Which harness build produced them.</param>
/// <param name="CatalogVersion">Digest of the scenario catalog that was run.</param>
/// <param name="Capabilities">The machine capabilities as measured at the start of the run.</param>
/// <param name="Verdicts">Every scenario verdict, including the ones that did not run.</param>
/// <param name="EnvironmentRestores">Whether every mutated property came back.</param>
/// <param name="EvidenceHashes">Every evidence file the run produced.</param>
/// <param name="TimestampUtc">When the run finished.</param>
/// <param name="Overall">Whether promotion is permitted.</param>
/// <remarks>
/// The publishing workflow refuses a tag whose commit does not match
/// <paramref name="SourceCommitSha"/>, and refuses this document entirely when
/// <paramref name="Overall"/> is not <see cref="QualificationOutcome.Qualified"/>.
/// That is what makes the checklist executable rather than advisory.
/// </remarks>
public sealed record QualificationRecord(
    string SchemaVersion,
    string RcTag,
    string SourceCommitSha,
    IReadOnlyList<ArtifactDigest> Artifacts,
    MachineFingerprint Machine,
    HarnessIdentity Harness,
    string CatalogVersion,
    IReadOnlyDictionary<string, string> Capabilities,
    IReadOnlyList<ScenarioVerdict> Verdicts,
    IReadOnlyList<EnvironmentRestoreVerdict> EnvironmentRestores,
    IReadOnlyList<Evidence> EvidenceHashes,
    DateTimeOffset TimestampUtc,
    QualificationOutcome Overall)
{
    /// <summary>The schema version this build of the harness writes.</summary>
    public const string CurrentSchemaVersion = "1";
}
