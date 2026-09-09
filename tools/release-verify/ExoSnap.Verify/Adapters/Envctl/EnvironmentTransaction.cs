using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Adapters.Envctl;

/// <summary>Whether a mutated environment property came back.</summary>
public enum RestoreResult
{
    /// <summary>Nothing was ever applied, so nothing is owed.</summary>
    NotApplicable,

    /// <summary>The machine is exactly as it was found.</summary>
    Restored,

    /// <summary>The restore did not complete and the journal still holds a debt.</summary>
    RestorePending,

    /// <summary>The restore could not run because the device is no longer attached.</summary>
    RestorePendingDeviceUnavailable,

    /// <summary>The restore ran and its read-back disagreed, or nothing could be concluded.</summary>
    RestoreFailed,
}

/// <summary>The record vocabulary for a restore verdict.</summary>
public static class RestoreResultNames
{
    /// <summary>The token the qualification record carries for a verdict.</summary>
    public static string For(RestoreResult result) => result switch
    {
        RestoreResult.NotApplicable => "NOT_APPLICABLE",
        RestoreResult.Restored => "RESTORED",
        RestoreResult.RestorePending => "RESTORE_PENDING",
        RestoreResult.RestorePendingDeviceUnavailable => "RESTORE_PENDING_DEVICE_UNAVAILABLE",
        _ => "RESTORE_FAILED",
    };
}

/// <summary>What one environment transaction produced.</summary>
/// <param name="Product">Whatever the scenario body returned, or null when it never ran.</param>
/// <param name="Restore">Whether the machine came back.</param>
/// <param name="TransactionId">The id envctl assigned, or an empty string.</param>
/// <param name="AppliedJson">The <c>begin</c> document, kept so a scenario can cite what changed.</param>
/// <param name="EvidenceJson">The restore evidence document, or an empty string.</param>
/// <param name="SetupErrorCode">
/// Set when the environment could not be brought to the desired state at all, so the
/// body never ran. Typed rather than a message because two very different things have
/// to be told apart: a desk that cannot offer the mode, and a mechanism that claimed
/// success and read back wrong.
/// </param>
/// <param name="SetupError">The refusal in words.</param>
/// <param name="BodyError">A message the body threw, recorded rather than swallowed.</param>
public sealed record EnvironmentTransactionOutcome(
    ScenarioResult? Product,
    RestoreResult Restore,
    string TransactionId,
    string AppliedJson,
    string EvidenceJson,
    string SetupErrorCode,
    string SetupError,
    string BodyError)
{
    /// <summary>
    /// The setup error codes that describe the desk rather than a broken mechanism.
    /// </summary>
    private static readonly string[] AbsentHardwareCodes =
        ["apply_rejected", "device_not_present", "unknown_property", "not_mutable"];

    /// <summary>
    /// The verdict a setup failure produces: Unavailable when this machine cannot
    /// offer what the scenario declared it needs, InfrastructureError when a
    /// mechanism misbehaved. Never Fail: nothing measured the product.
    /// </summary>
    public ScenarioResult SetupOutcome() =>
        AbsentHardwareCodes.Contains(this.SetupErrorCode, StringComparer.OrdinalIgnoreCase)
            ? ScenarioResult.Unavailable($"{this.SetupErrorCode}: {this.SetupError}")
            : ScenarioResult.InfrastructureError($"{this.SetupErrorCode}: {this.SetupError}");
}

/// <summary>
/// Runs scenario bodies with the machine mutated, and puts it back afterwards.
/// </summary>
/// <remarks>
/// The restore is in a <c>finally</c>, and that placement is the contract. It has to
/// survive an assertion failure, a product failure, a harness bug, a timeout, an
/// exception from anywhere in the body, and cancellation. A human gate sits inside a
/// transaction, so an operator who walks away must not leave the machine
/// reconfigured.
///
/// The product verdict and the restore verdict are separate on purpose: a scenario
/// can prove the product correct and still leave a display in the wrong mode, and one
/// field cannot say both.
/// </remarks>
public sealed class EnvironmentOrchestrator
{
    private readonly IEnvctl envctl;
    private readonly string runId;
    private readonly string journalDirectory;
    private readonly string journalPath;
    private readonly Dictionary<string, RestoreResult> restores = new(StringComparer.OrdinalIgnoreCase);

    private EnvironmentOrchestrator(
        IEnvctl envctl,
        string runId,
        string journalDirectory,
        string journalPath)
    {
        this.envctl = envctl;
        this.runId = runId;
        this.journalDirectory = journalDirectory;
        this.journalPath = journalPath;
    }

    /// <summary>Whether a previous run left the machine mutated and unrestored.</summary>
    public bool Dirty { get; private set; }

    /// <summary>Why the machine is considered dirty, or an empty string.</summary>
    public string DirtyDetail { get; private set; } = string.Empty;

    /// <summary>The properties the startup recovery pass put back.</summary>
    public IReadOnlyList<string> Recovered { get; private set; } = [];

    /// <summary>Whether the tool is available at all.</summary>
    public bool Available => this.envctl.Available;

    /// <summary>
    /// What every scenario that mutated something left behind, by scenario id.
    /// </summary>
    /// <remarks>
    /// Kept separate from the scenario verdicts because they answer different
    /// questions. A gate can prove the product correct and still leave a display in
    /// the wrong mode, and a campaign that merged the two would hide the second.
    /// </remarks>
    public IReadOnlyDictionary<string, string> Restores =>
        this.restores.ToDictionary(pair => pair.Key, pair => RestoreResultNames.For(pair.Value), StringComparer.Ordinal);

    /// <summary>
    /// Opens the orchestrator for one campaign, performing the mandatory startup
    /// recovery before it returns.
    /// </summary>
    /// <remarks>
    /// One journal file per machine, not per campaign: the dirty-startup gate is
    /// "does that file exist and is it unfinished", which only answers anything
    /// across campaigns if every campaign asks about the same file.
    ///
    /// A recovery pass that itself failed is the strongest possible reason not to
    /// mutate anything else, and it is recorded as dirty rather than thrown: the
    /// campaign can still run its read-only scenarios and report exactly why the
    /// mutating ones did not.
    /// </remarks>
    public static async Task<EnvironmentOrchestrator> OpenAsync(
        IEnvctl envctl,
        string runId,
        string journalDirectory,
        string journalPath,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(envctl);
        ArgumentException.ThrowIfNullOrWhiteSpace(runId);
        ArgumentException.ThrowIfNullOrWhiteSpace(journalDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(journalPath);

        var orchestrator = new EnvironmentOrchestrator(
            envctl, runId, Path.GetFullPath(journalDirectory), Path.GetFullPath(journalPath));

        if (!envctl.Available)
        {
            return orchestrator;
        }

        Directory.CreateDirectory(orchestrator.journalDirectory);

        try
        {
            var recovered = await envctl.RecoverAsync(orchestrator.journalPath, cancellationToken)
                .ConfigureAwait(false);

            orchestrator.Recovered =
            [
                .. recovered.Field("evidence")
                    .EnumerateOptionalArray("properties")
                    .Select(property => property.OptionalString("property"))
                    .Where(property => property.Length > 0),
            ];

            // `mutationAllowed` is the gate envctl computes, and it is the only field
            // worth trusting here: a journal that was present and restored leaves it
            // true. Deriving "dirty" from `journalPresent` would call a successfully
            // recovered run dirty.
            var body = recovered.Body;
            if (body.ValueKind == JsonValueKind.Object &&
                body.TryGetProperty("mutationAllowed", out var allowed) &&
                allowed.ValueKind == JsonValueKind.False)
            {
                orchestrator.Dirty = true;
                orchestrator.DirtyDetail = $"recovery left the environment in state '{recovered.State}'";
                if (recovered.Error.Length > 0)
                {
                    orchestrator.DirtyDetail += $": {recovered.Error}";
                }
            }
        }
#pragma warning disable CA1031 // Any recovery failure is a reason not to mutate, never a reason to abandon the campaign.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            orchestrator.Dirty = true;
            orchestrator.DirtyDetail = exception.Message;
        }

        return orchestrator;
    }

    /// <summary>
    /// Runs a body with the machine mutated to <paramref name="desired"/>, and
    /// restores it exactly afterwards whatever the body did.
    /// </summary>
    /// <param name="scenario">The scenario the transaction belongs to.</param>
    /// <param name="desired">Property to value, as <c>alias:property</c> keys. Empty means "mutate nothing".</param>
    /// <param name="body">The work. Receives the <c>begin</c> document, or the default element.</param>
    /// <param name="cancellationToken">Cancels the body; the restore still runs.</param>
    public async Task<EnvironmentTransactionOutcome> RunAsync(
        string scenario,
        IReadOnlyDictionary<string, string> desired,
        Func<JsonElement, CancellationToken, Task<ScenarioResult>> body,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scenario);
        ArgumentNullException.ThrowIfNull(desired);
        ArgumentNullException.ThrowIfNull(body);

        // A desired state with nothing in it mutates nothing, journals nothing and
        // needs no restore. Modelled explicitly rather than falling out of an empty
        // loop, so a scenario that only reads the environment is visibly a different
        // thing from one that mutated and happened to restore.
        if (desired.Count == 0)
        {
            var product = await body(default, cancellationToken).ConfigureAwait(false);
            return new EnvironmentTransactionOutcome(
                product, RestoreResult.NotApplicable, string.Empty, string.Empty, string.Empty,
                string.Empty, string.Empty, string.Empty);
        }

        if (this.Dirty)
        {
            return new EnvironmentTransactionOutcome(
                null, RestoreResult.NotApplicable, string.Empty, string.Empty, string.Empty,
                "environment_dirty",
                $"the environment is dirty from an earlier run and was not restored: {this.DirtyDetail}",
                string.Empty);
        }

        if (!this.envctl.Available)
        {
            return new EnvironmentTransactionOutcome(
                null, RestoreResult.NotApplicable, string.Empty, string.Empty, string.Empty,
                "envctl_unavailable", "exosnap-envctl is not built; a mutating scenario cannot run", string.Empty);
        }

        Directory.CreateDirectory(this.journalDirectory);
        var desiredFile = Path.Combine(
            this.journalDirectory,
            $"desired-{Guid.NewGuid().ToString("n", CultureInfo.InvariantCulture)}.json");

        // The document envctl reads is { "desired": { "<alias>:<property>": "<value>" } }
        // and every value is a string. A JSON number here would be a type error at the
        // boundary rather than a refresh rate.
        WriteDesired(desiredFile, desired);

        EnvctlResponse begun;
        try
        {
            begun = await this.envctl
                .BeginAsync(scenario, this.runId, this.journalPath, desiredFile, cancellationToken)
                .ConfigureAwait(false);
        }
#pragma warning disable CA1031 // A failed begin may have partially mutated the machine, so recovery must still run.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            var (beginRestore, beginEvidence) = await this.RestoreAsync(scenario, this.journalPath).ConfigureAwait(false);
            this.restores[scenario] = beginRestore;
            return new EnvironmentTransactionOutcome(
                null,
                beginRestore,
                string.Empty,
                string.Empty,
                beginEvidence,
                "begin_exception",
                $"envctl begin threw {exception.GetType().Name}: {exception.Message}",
                string.Empty);
        }
        finally
        {
            Delete(desiredFile);
        }

        if (!begun.Ok)
        {
            return this.BeginRefused(scenario, begun);
        }

        var transactionId = begun.Field("transactionId").OptionalStringValue();
        var openJournal = begun.Field("journalPath").OptionalStringValue();
        if (openJournal.Length == 0)
        {
            openJournal = this.journalPath;
        }

        ScenarioResult? outcome = null;
        var bodyError = string.Empty;
        try
        {
            outcome = await body(begun.Body, cancellationToken).ConfigureAwait(false);
        }
#pragma warning disable CA1031 // Recorded, not swallowed: the caller decides what a body failure means. The restore runs either way.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            bodyError = $"{exception.GetType().Name}: {exception.Message}";
        }

        var (restore, evidenceJson) = await this.RestoreAsync(scenario, openJournal).ConfigureAwait(false);
        this.restores[scenario] = restore;

        return new EnvironmentTransactionOutcome(
            outcome, restore, transactionId, begun.RawJson, evidenceJson, string.Empty, string.Empty, bodyError);
    }

    /// <summary>
    /// Maps envctl's terminal transaction state onto the report's restore taxonomy.
    /// </summary>
    /// <remarks>
    /// Total on purpose: an unrecognised state becomes <see cref="RestoreResult.RestoreFailed"/>,
    /// never <see cref="RestoreResult.Restored"/>, because "I do not know what
    /// happened" and "everything is fine" must not be the same answer.
    ///
    /// <paramref name="ok"/> is not redundant with the state. A journal that exists
    /// but cannot be parsed never reaches a transaction, so envctl reports the default
    /// state <c>Clean</c> with <c>ok=false</c>, and a state-only mapping would turn
    /// "the machine may be mutated and I cannot say how" into Restored.
    /// </remarks>
    public static RestoreResult MapRestoreState(string state, bool? ok)
    {
        var mapped = state switch
        {
            "Restored" => RestoreResult.Restored,
            "RestorePending" => RestoreResult.RestorePending,
            "RestorePendingDeviceUnavailable" => RestoreResult.RestorePendingDeviceUnavailable,
            "RestoreFailed" => RestoreResult.RestoreFailed,
            "Clean" => RestoreResult.Restored,
            _ => RestoreResult.RestoreFailed,
        };

        return mapped == RestoreResult.Restored && ok is false ? RestoreResult.RestoreFailed : mapped;
    }

    /// <summary>
    /// A refresh rate the display offers that a read-back can actually confirm, or
    /// null when it offers none.
    /// </summary>
    /// <remarks>
    /// Windows enumerates the nominal and the actual rate of the same physical mode as
    /// two entries - 59 and 60, 119 and 120 - and collapses them on apply:
    /// <c>ChangeDisplaySettingsExW</c> accepts 60 and <c>EnumDisplaySettingsExW</c>
    /// then reports 59. Asking for one half of such a pair produces a verify mismatch
    /// no matter how correct the mechanism is, so a rate is only usable when no
    /// neighbour within 1 Hz is also enumerated. That is a rule about Windows, not
    /// about one panel.
    /// </remarks>
    public static int? SelectUntwinnedRefreshRate(IReadOnlyCollection<int> offered, int current)
    {
        ArgumentNullException.ThrowIfNull(offered);
        var rates = offered.Distinct().ToHashSet();
        foreach (var rate in rates.OrderDescending())
        {
            if (rate == current || rates.Contains(rate - 1) || rates.Contains(rate + 1))
            {
                continue;
            }

            return rate;
        }

        return null;
    }

    /// <summary>
    /// A stable digest of a property bag, order-independent.
    /// </summary>
    /// <remarks>
    /// Used for the machine fingerprint a qualification record carries and for the
    /// per-scenario environment dependency, so "did anything this verdict rests on
    /// change" is one string comparison rather than a field-by-field diff that quietly
    /// forgets a field. The key <c>fingerprint</c> is excluded: it is the digest of
    /// the rest and must never be part of it.
    /// </remarks>
    public static string Fingerprint(IReadOnlyDictionary<string, string> properties)
    {
        ArgumentNullException.ThrowIfNull(properties);
        var material = string.Join(
            ';',
            properties
                .Where(pair => !string.Equals(pair.Key, "fingerprint", StringComparison.Ordinal))
                .OrderBy(pair => pair.Key, StringComparer.Ordinal)
                .Select(pair => $"{pair.Key}={pair.Value}"));
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(material)))
            .ToLower(CultureInfo.InvariantCulture);
    }

    private EnvironmentTransactionOutcome BeginRefused(string scenario, EnvctlResponse begun)
    {
        var code = begun.ErrorCode.Length > 0 ? begun.ErrorCode : "begin_failed";
        var message = begun.Error.Length > 0 ? begun.Error : "envctl begin produced no usable answer";

        // `begin` rolls back whatever it had already applied before it gave up, and
        // that rollback can itself fail - which is exactly when saying nothing is
        // worst. envctl reports the outcome of its own rollback in `state`, so that
        // decides here rather than the premise that a failed begin always cleans up.
        // A begin with no state at all is the unknown case, and unknown is dirty.
        if (begun.State is "Clean" or "Restored")
        {
            return new EnvironmentTransactionOutcome(
                null, RestoreResult.NotApplicable, string.Empty, begun.RawJson, string.Empty,
                code, message, string.Empty);
        }

        var restore = MapRestoreState(begun.State, false);
        this.restores[scenario] = restore;
        this.Dirty = true;
        this.DirtyDetail =
            $"scenario '{scenario}' failed to begin and its rollback ended in {RestoreResultNames.For(restore)} ({code})";

        return new EnvironmentTransactionOutcome(
            null, restore, string.Empty, begun.RawJson, begun.Field("evidence").RawText(),
            code, message, string.Empty);
    }

    private async Task<(RestoreResult Restore, string EvidenceJson)> RestoreAsync(string scenario, string journal)
    {
        try
        {
            // `restore` exits non-zero when the environment is still owed something,
            // which is a verdict to report rather than throw on: the record needs
            // RESTORE_FAILED next to the product result, not a stack trace instead of
            // both. Cancellation is deliberately not honoured here - the whole point
            // of the finally is that it runs.
            var restored = await this.envctl.RestoreAsync(journal, CancellationToken.None).ConfigureAwait(false);
            var result = MapRestoreState(restored.State, restored.Ok);
            if (result != RestoreResult.Restored)
            {
                this.Dirty = true;
                this.DirtyDetail = $"scenario '{scenario}' ended in {RestoreResultNames.For(result)}";
            }

            return (result, restored.Field("evidence").RawText());
        }
#pragma warning disable CA1031 // A restore that threw leaves the machine in an unknown state, which is the dirtiest answer there is.
        catch (Exception exception)
#pragma warning restore CA1031
        {
            this.Dirty = true;
            this.DirtyDetail = exception.Message;
            return (RestoreResult.RestoreFailed, string.Empty);
        }
    }

    private static void WriteDesired(string path, IReadOnlyDictionary<string, string> desired)
    {
        using var stream = File.Create(path);
        using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });
        writer.WriteStartObject();
        writer.WritePropertyName("desired");
        writer.WriteStartObject();
        foreach (var (key, value) in desired.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            writer.WriteString(key, value);
        }

        writer.WriteEndObject();
        writer.WriteEndObject();
    }

    private static void Delete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
            // A handoff document nobody can remove is not worth a verdict.
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}

/// <summary>Reads optional fields out of a document without throwing on an absent one.</summary>
internal static class JsonElementExtensions
{
    internal static IEnumerable<JsonElement> EnumerateOptionalArray(this JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Array
            ? value.EnumerateArray()
            : [];

    internal static string OptionalString(this JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    internal static string OptionalStringValue(this JsonElement element) =>
        element.ValueKind == JsonValueKind.String ? element.GetString() ?? string.Empty : string.Empty;

    internal static string RawText(this JsonElement element) =>
        element.ValueKind is JsonValueKind.Undefined or JsonValueKind.Null
            ? string.Empty
            : element.GetRawText();
}
