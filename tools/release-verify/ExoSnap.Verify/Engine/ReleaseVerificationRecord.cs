using System.Collections.ObjectModel;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Engine;

/// <summary>One published downloadable the campaign was bound to.</summary>
/// <param name="FileName">The name the release page serves it under.</param>
/// <param name="Sha256">Lowercase hex digest of the bytes.</param>
/// <param name="Bytes">Size, so a truncated download is visible.</param>
public sealed record ReleasePackage(string FileName, string Sha256, long Bytes);

/// <summary>What the campaign measured, and against which bytes.</summary>
/// <param name="RunId">The campaign directory's own name.</param>
/// <param name="RcTag">The release candidate the artifacts were published under.</param>
/// <param name="SourceCommit">The commit those artifacts were built from.</param>
/// <param name="ExecutablePath">The exosnap.exe the gates drove.</param>
/// <param name="ProductVersion">The version those bytes report about themselves.</param>
/// <param name="ExecutableSha256">Digest of the executable, or an empty string.</param>
/// <param name="InstallTree">Whether the artifact came with an install tree.</param>
public sealed record CampaignBinding(
    string RunId,
    string RcTag,
    string SourceCommit,
    string ExecutablePath,
    string ProductVersion,
    string ExecutableSha256,
    bool InstallTree);

/// <summary>What one scenario is recorded as, in the record's own vocabulary.</summary>
/// <param name="Id">Scenario id.</param>
/// <param name="Title">The one-line declaration.</param>
/// <param name="Layer">The weakest mechanism it relied on.</param>
/// <param name="State">PASS, FAIL, INFRA_ERROR, UNAVAILABLE, DEFERRED, SKIPPED, BLOCKED or STALE.</param>
/// <param name="Required">Whether promotion depends on it.</param>
/// <param name="OptIn">Whether it is excluded from a default sweep.</param>
/// <param name="Message">Why it concluded what it did.</param>
/// <param name="DurationMs">How long the body ran.</param>
/// <param name="RestoreResult">Whether the machine came back.</param>
/// <param name="Evidence">The files the verdict rests on, with their digests.</param>
/// <param name="Attempted">
/// Whether the scenario body actually ran. False for a verdict the plan settled before
/// anything started, which is the difference between a gate that answered and a gate
/// nobody asked.
/// </param>
public sealed record RecordedCheck(
    string Id,
    string Title,
    string Layer,
    string State,
    bool Required,
    bool OptIn,
    string Message,
    long DurationMs,
    string RestoreResult,
    IReadOnlyList<Evidence> Evidence,
    bool Attempted);

/// <summary>
/// The document a release promotion is allowed to rest on, in the shape the publish
/// lock reads.
/// </summary>
/// <remarks>
/// The shape is not the harness's own preference: <c>scripts/check-release-qualification.ps1</c>
/// runs on a clean checkout in the release workflow and refuses anything it cannot
/// read, so the producer and that reader share one contract. Two implementations of a
/// release lock that disagree about a field name is a lock that opens.
///
/// The overall verdict is derived here from the same rules the workflow applies, so a
/// record cannot claim more than its own contents support.
/// </remarks>
public static class ReleaseVerificationRecord
{
    /// <summary>The identifier every record carries, so a reader can refuse a shape it does not understand.</summary>
    public const string Schema = "exosnap.release-verification/1";

    /// <summary>The file name the record is written under.</summary>
    public const string FileName = "release-verification.json";

    /// <summary>The runner a record names as its producer.</summary>
    public const string Runner = "tools/release-verify/ExoSnap.Verify";

    /// <summary>The verdict that permits promotion.</summary>
    public const string Qualified = "QUALIFIED";

    /// <summary>The verdict that refuses it.</summary>
    public const string NotQualified = "NOT_QUALIFIED";

    /// <summary>The states that mean a required gate was actually answered.</summary>
    private static readonly string[] AnsweredStates = ["PASS", "FAIL", "INFRA_ERROR"];

    /// <summary>The restore verdicts that leave nothing owed.</summary>
    private static readonly string[] SettledRestores = ["NOT_APPLICABLE", "RESTORED"];

    /// <summary>The record's own name for one outcome.</summary>
    public static string StateOf(ScenarioOutcome outcome) => outcome switch
    {
        ScenarioOutcome.Pass => "PASS",
        ScenarioOutcome.Fail => "FAIL",
        ScenarioOutcome.InfrastructureError => "INFRA_ERROR",
        ScenarioOutcome.Blocked => "BLOCKED",
        ScenarioOutcome.Unavailable => "UNAVAILABLE",
        ScenarioOutcome.Deferred => "DEFERRED",
        ScenarioOutcome.Skipped => "SKIPPED",
        _ => "STALE",
    };

    /// <summary>
    /// Every reason this record must not promote anything. An empty list is the only
    /// thing that qualifies a release.
    /// </summary>
    /// <remarks>
    /// Content rules only: what the record itself says. The comparisons against the
    /// tag being published - commit, RC tag, published asset digests - belong to the
    /// workflow, which has the release page in front of it and this record does not.
    /// </remarks>
    public static ReadOnlyCollection<string> Blockers(
        CampaignBinding binding,
        string machineFingerprint,
        string harnessCommit,
        string catalogDigest,
        IReadOnlyList<ReleasePackage> packages,
        IReadOnlyList<RecordedCheck> checks)
    {
        ArgumentNullException.ThrowIfNull(binding);
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentNullException.ThrowIfNull(checks);

        var reasons = new List<string>();

        foreach (var (field, value) in new[]
                 {
                     ("runId", binding.RunId),
                     ("rcTag", binding.RcTag),
                     ("sourceCommit", binding.SourceCommit),
                     ("machineFingerprint", machineFingerprint),
                 })
        {
            if (string.IsNullOrWhiteSpace(value))
            {
                reasons.Add($"record field '{field}' is empty");
            }
        }

        if (string.IsNullOrWhiteSpace(harnessCommit))
        {
            reasons.Add("record field 'harness.commit' is empty");
        }

        if (string.IsNullOrWhiteSpace(catalogDigest))
        {
            reasons.Add("record field 'catalog.digest' is empty");
        }

        if (!packages.Any(package => !string.IsNullOrWhiteSpace(package.Sha256)))
        {
            reasons.Add(
                "the record carries no release package SHA-256; the campaign was not bound to the published RC assets");
        }

        if (checks.Count == 0)
        {
            reasons.Add("the record contains no scenario verdicts");
            return new ReadOnlyCollection<string>(reasons);
        }

        var failed = checks.Where(check => check.State == "FAIL").Select(check => check.Id).ToList();
        if (failed.Count > 0)
        {
            reasons.Add($"product defect (FAIL): {string.Join(", ", failed)}");
        }

        var infra = checks.Where(check => check.State == "INFRA_ERROR").Select(check => check.Id).ToList();
        if (infra.Count > 0)
        {
            reasons.Add(
                "harness or environment failure (INFRA_ERROR), so nothing was measured: " +
                string.Join(", ", infra));
        }

        foreach (var check in checks.Where(check =>
                     check.Required && !AnsweredStates.Contains(check.State, StringComparer.Ordinal)))
        {
            reasons.Add($"required gate {check.Id} is {check.State}, so it was never answered");
        }

        foreach (var check in checks.Where(check =>
                     !string.IsNullOrWhiteSpace(check.RestoreResult) &&
                     !SettledRestores.Contains(check.RestoreResult, StringComparer.Ordinal)))
        {
            reasons.Add($"{check.Id} left the machine misconfigured: environment restore {check.RestoreResult}");
        }

        foreach (var check in checks)
        {
            var missing = check.Evidence.Where(evidence => !File.Exists(evidence.Path)).ToList();
            if (missing.Count > 0)
            {
                reasons.Add(
                    $"{check.Id} cites evidence that is not there: " +
                    string.Join(", ", missing.Select(evidence => evidence.Name)));
            }
        }

        return new ReadOnlyCollection<string>(reasons);
    }

    /// <summary>
    /// A digest over every scenario id and its opt-in flag.
    /// </summary>
    /// <remarks>
    /// Deliberately the same material and the same encoding the PowerShell producer
    /// uses, so a record written by either side is comparable with one written by the
    /// other. It covers reclassification as well as addition and removal: a catalog
    /// that turned a required gate opt-in cannot be mistaken for the one a record was
    /// produced against, even when nobody bumped the declared version.
    /// </remarks>
    public static string CatalogDigest(IEnumerable<ScenarioDescriptor> descriptors)
    {
        ArgumentNullException.ThrowIfNull(descriptors);
        var material = string.Join(
            ';',
            descriptors
                .Select(descriptor => $"{descriptor.Id}={(descriptor.OptIn ? "True" : "False")}")
                .Order(StringComparer.Ordinal));
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(material)))
            .ToLower(CultureInfo.InvariantCulture);
    }

    /// <summary>Writes the record, and reports whether it permits promotion.</summary>
    /// <param name="path">Where the document goes.</param>
    /// <param name="binding">What the campaign measured, and against which bytes.</param>
    /// <param name="machineFingerprint">The digest of the environment facts the verdicts rest on.</param>
    /// <param name="harnessVersion">The harness build that produced them.</param>
    /// <param name="harnessCommit">The commit that harness was checked out at, or an empty string.</param>
    /// <param name="harnessDirty">Whether that tree had uncommitted changes.</param>
    /// <param name="catalogVersion">The catalog's own version.</param>
    /// <param name="catalogDigest">The digest over its declarations.</param>
    /// <param name="catalogSize">How many scenarios it declares.</param>
    /// <param name="capabilities">What the machine reported before anything ran.</param>
    /// <param name="environment">The environment facts, as the orchestrator names them.</param>
    /// <param name="packages">The published downloadables the campaign was bound to.</param>
    /// <param name="requiredIds">The gates promotion depends on.</param>
    /// <param name="namedOptIn">The opt-in gates this release also required.</param>
    /// <param name="checks">Every verdict.</param>
    /// <param name="runDirectory">The run directory evidence paths are written relative to.</param>
    public static ReadOnlyCollection<string> Write(
        string path,
        CampaignBinding binding,
        string machineFingerprint,
        string harnessVersion,
        string harnessCommit,
        bool harnessDirty,
        string catalogVersion,
        string catalogDigest,
        int catalogSize,
        IReadOnlyDictionary<string, string> capabilities,
        IReadOnlyDictionary<string, string> environment,
        IReadOnlyList<ReleasePackage> packages,
        IReadOnlyList<string> requiredIds,
        IReadOnlyList<string> namedOptIn,
        IReadOnlyList<RecordedCheck> checks,
        string runDirectory)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        ArgumentNullException.ThrowIfNull(binding);
        ArgumentNullException.ThrowIfNull(capabilities);
        ArgumentNullException.ThrowIfNull(environment);
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentNullException.ThrowIfNull(requiredIds);
        ArgumentNullException.ThrowIfNull(namedOptIn);
        ArgumentNullException.ThrowIfNull(checks);

        var blockers = Blockers(binding, machineFingerprint, harnessCommit, catalogDigest, packages, checks);
        var now = DateTimeOffset.UtcNow.ToString("o", CultureInfo.InvariantCulture);

        var directory = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        using var stream = File.Create(path);
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });

        writer.WriteStartObject();
        writer.WriteString("schema", Schema);
        writer.WriteString("generatedUtc", now);
        writer.WriteString("runId", binding.RunId);
        writer.WriteString("rcTag", binding.RcTag);
        writer.WriteString("sourceCommit", binding.SourceCommit);

        writer.WriteStartObject("artifact");
        writer.WriteString("exePath", binding.ExecutablePath);
        writer.WriteString("productVersion", binding.ProductVersion);
        writer.WriteString("sha256", binding.ExecutableSha256);
        writer.WriteString("tag", binding.RcTag);
        writer.WriteString("sourceCommit", binding.SourceCommit);
        writer.WriteBoolean("installTree", binding.InstallTree);
        writer.WriteEndObject();

        writer.WriteStartArray("packages");
        foreach (var package in packages)
        {
            writer.WriteStartObject();
            writer.WriteString("fileName", package.FileName);
            writer.WriteString("sha256", package.Sha256);
            writer.WriteNumber("bytes", package.Bytes);
            writer.WriteEndObject();
        }

        writer.WriteEndArray();

        writer.WriteString("machineFingerprint", machineFingerprint);

        writer.WriteStartObject("harness");
        writer.WriteString("version", harnessVersion);
        writer.WriteString("runner", Runner);
        writer.WriteString("commit", harnessCommit);
        writer.WriteBoolean("dirty", harnessDirty);
        writer.WriteEndObject();

        writer.WriteStartObject("catalog");
        writer.WriteString("version", catalogVersion);
        writer.WriteString("digest", catalogDigest);
        writer.WriteNumber("scenarioCount", catalogSize);
        writer.WriteEndObject();

        WriteMap(writer, "capabilities", capabilities);
        WriteMap(writer, "environment", environment);

        writer.WriteStartObject("required");
        writer.WriteString(
            "policy",
            "every scenario that is not opt-in, plus the opt-in scenarios named for this release");
        writer.WriteStartArray("ids");
        foreach (var id in requiredIds)
        {
            writer.WriteStringValue(id);
        }

        writer.WriteEndArray();
        writer.WriteStartArray("namedOptIn");
        foreach (var id in namedOptIn)
        {
            writer.WriteStringValue(id);
        }

        writer.WriteEndArray();
        writer.WriteEndObject();

        WriteCounts(writer, "summary", checks.Select(check => check.State));
        WriteCounts(writer, "restoreSummary", checks.Select(check => check.RestoreResult));

        writer.WriteStartArray("checks");
        foreach (var check in checks)
        {
            writer.WriteStartObject();
            writer.WriteString("id", check.Id);
            writer.WriteString("title", check.Title);
            writer.WriteString("layer", check.Layer);
            writer.WriteString("state", check.State);
            writer.WriteBoolean("required", check.Required);
            writer.WriteBoolean("optIn", check.OptIn);
            writer.WriteString("message", check.Message);
            writer.WriteNumber("durationMs", check.DurationMs);

            // The engine runs a scenario body once. Retrying is an explicit act by
            // whoever reads the report, not something a run does on its own, so this
            // counts what happened rather than describing a policy.
            writer.WriteNumber("attempts", check.Attempted ? 1 : 0);

            // Written even when empty, because a reader that has to tell "no reason
            // was recorded" from "the field is not in this shape" is reading two
            // producers, and only one of them can be wrong at a time.
            writer.WriteString("skipReason", check.Attempted ? string.Empty : check.Message);
            writer.WriteBoolean("interrupted", false);
            writer.WriteString("restoreResult", check.RestoreResult);
            writer.WriteStartArray("evidence");
            foreach (var evidence in check.Evidence)
            {
                var exists = File.Exists(evidence.Path);
                writer.WriteStartObject();
                writer.WriteString("path", Relative(runDirectory, evidence.Path));
                if (exists)
                {
                    writer.WriteNumber("bytes", new FileInfo(evidence.Path).Length);
                    writer.WriteString("sha256", evidence.Sha256);
                }
                else
                {
                    writer.WriteNull("bytes");
                    writer.WriteNull("sha256");
                }

                writer.WriteBoolean("missing", !exists);
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        writer.WriteEndArray();

        writer.WriteStartObject("qualification");
        writer.WriteString("overall", blockers.Count == 0 ? Qualified : NotQualified);
        writer.WriteStartArray("reasons");
        foreach (var reason in blockers)
        {
            writer.WriteStringValue(reason);
        }

        writer.WriteEndArray();
        writer.WriteString("evaluatedUtc", now);
        writer.WriteEndObject();

        writer.WriteEndObject();
        return blockers;
    }

    /// <summary>The environment fingerprint the record carries.</summary>
    public static string FingerprintOf(IReadOnlyDictionary<string, string> environment) =>
        EnvironmentOrchestrator.Fingerprint(environment);

    private static void WriteMap(Utf8JsonWriter writer, string name, IReadOnlyDictionary<string, string> values)
    {
        writer.WriteStartObject(name);
        foreach (var (key, value) in values.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            writer.WriteString(key, value);
        }

        writer.WriteEndObject();
    }

    private static void WriteCounts(Utf8JsonWriter writer, string name, IEnumerable<string> values)
    {
        writer.WriteStartObject(name);
        foreach (var group in values
                     .Where(value => !string.IsNullOrWhiteSpace(value))
                     .GroupBy(value => value, StringComparer.Ordinal)
                     .OrderBy(group => group.Key, StringComparer.Ordinal))
        {
            writer.WriteNumber(group.Key, group.Count());
        }

        writer.WriteEndObject();
    }

    // Run-relative, exactly as the checks record them, so the record stays readable
    // after the run directory moves.
    private static string Relative(string runDirectory, string path)
    {
        if (string.IsNullOrWhiteSpace(runDirectory))
        {
            return path;
        }

        var relative = Path.GetRelativePath(Path.GetFullPath(runDirectory), Path.GetFullPath(path));
        return relative.Replace('\\', '/');
    }
}
