using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-PRESENT-001: unelevated present diagnostics explain themselves and open no
/// session.
/// </summary>
/// <remarks>
/// The opt-in is turned on through the product's own switch rather than by editing a
/// file, so what the gate observes is the path a user takes. What must then be true is
/// that the switch reached the snapshot, that no data is claimed without elevation,
/// and that the availability says exactly why.
/// </remarks>
public sealed class UnelevatedPresentGate : IScenarioBody
{
    /// <summary>The availability an unelevated process must report.</summary>
    public const string ExpectedAvailability = "requiresElevation";

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var set = await session.InvokeAsync(
                "diagnostics.setInDepth",
                new Dictionary<string, object?>(StringComparer.Ordinal) { ["enabled"] = true },
                cancellationToken)
            .ConfigureAwait(false);
        if (!set.Ok)
        {
            return ScenarioResult.Fail($"diagnostics.setInDepth refused: {set.Refusal}");
        }

        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var present = Snapshots.Value(environment.Result, "present");
        var evidence = GateEvidence.SaveJson(context, "present.json", present);

        if (Snapshots.IsTrue(present, "elevated"))
        {
            return ScenarioResult.Unavailable(
                "This process is elevated, so the unelevated posture cannot be observed here");
        }

        var problems = new List<string>();
        if (!Snapshots.IsTrue(present, "optIn"))
        {
            problems.Add("the opt-in did not reach environment.snapshot");
        }

        if (Snapshots.IsTrue(present, "available"))
        {
            problems.Add("present data is reported available without elevation");
        }

        var availability = Snapshots.Text(present, "availability");
        if (!string.Equals(availability, ExpectedAvailability, StringComparison.Ordinal))
        {
            problems.Add($"availability is '{availability}', expected '{ExpectedAvailability}'");
        }

        return problems.Count > 0
            ? ScenarioResult.Fail(string.Join("; ", problems), evidence)
            : ScenarioResult.Pass(
                "opt-in on, not elevated -> requiresElevation, no session, no prompt", evidence);
    }
}

/// <summary>
/// REL-PRESENT-XCHECK-001: an independent observer confirms the presentation path
/// ExoSnap reports.
/// </summary>
/// <remarks>
/// Required only when something that could change the answer has moved. ExoSnap's own
/// diagnostics agreeing with ExoSnap's own automation endpoint is the same truth read
/// twice, so this gate is the only place a second observer is consulted - and it is
/// expensive, needing an elevated ETW session on a machine that is presenting
/// something worth measuring.
///
/// The rule: run it when the present-diagnostics sources or the Windows build differ
/// from the last confirmed record. Otherwise report Skipped with a pointer to that
/// record, so a reader can see which run the confirmation came from rather than
/// assuming one happened.
/// </remarks>
public sealed class PresentCrossCheckGate : IScenarioBody
{
    /// <summary>The sources whose digest decides whether the cross-check must run again.</summary>
    public static readonly string[] PresentSourceGlobs = ["app/diagnostics/Present*"];

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var codeHash = PresentConfirmation.HashSources(services.Artifact.RepositoryRoot, PresentSourceGlobs);
        var osBuild = context.Capabilities[CapabilityKeys.OsVersion];

        var required = PresentConfirmation.Required(services.LastPresentConfirmation, codeHash, osBuild);
        if (!required.Required)
        {
            return ScenarioResult.Skipped(required.Reason);
        }

        if (!services.PresentMon.Available)
        {
            return ScenarioResult.Unavailable(services.PresentMon.UnavailableReason);
        }

        var capture = services.PresentCapturePath;
        if (capture is null || !File.Exists(capture))
        {
            return ScenarioResult.Unavailable(
                "no PresentMon capture was taken for this run; the cross-check runs in the disposable guest " +
                "that carries an elevated ETW session");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);
        var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var productMode = Snapshots.Text(pipeline.Result, "sourcePresentation.presentMode");

        var identity = await session.AppSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var processId = (int)(Snapshots.Number(identity.Result, "processId") ?? 0);
        if (processId == 0)
        {
            return ScenarioResult.InfrastructureError(
                "the application reported no process id, so no present could be attributed to it");
        }

        var records = await services.PresentMon.ReadCaptureAsync(capture, cancellationToken).ConfigureAwait(false);
        var agreement = PresentModeCrossCheck.Compare(records, processId, productMode);

        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "pipeline.json", pipeline.Result),
            GateEvidence.SaveText(
                context,
                "cross-check.json",
                JsonSerializer.Serialize(
                    new PresentCrossCheckSummary(
                        processId,
                        PresentMonCsv.ProductName(agreement.Product),
                        PresentMonCsv.ProductName(agreement.Observed),
                        agreement.PresentCount,
                        agreement.Agreed,
                        codeHash,
                        osBuild),
                    GateJson.Default.PresentCrossCheckSummary)),
        };

        if (agreement.Agreed)
        {
            return ScenarioResult.Pass(agreement.Message, evidence);
        }

        // Nothing was compared when the capture attributed no present to the process,
        // so that case is an infrastructure error rather than a claim about the
        // product. A real disagreement is a defect in one of the two readings, and the
        // gate says which two readings disagreed rather than which one is wrong.
        return agreement.PresentCount == 0
            ? ScenarioResult.InfrastructureError(agreement.Message, evidence)
            : ScenarioResult.Fail(agreement.Message, evidence);
    }
}

/// <summary>What a previous present cross-check confirmed, and against what.</summary>
/// <param name="RunId">The campaign that confirmed it.</param>
/// <param name="PresentCodeHash">Digest of the present-diagnostics sources at that time.</param>
/// <param name="OsBuild">The Windows build it was confirmed on.</param>
public sealed record PresentConfirmation(string RunId, string PresentCodeHash, string OsBuild)
{
    /// <summary>
    /// A digest over the present-diagnostics sources, so a change to the code that
    /// decides a present mode makes the cross-check required again.
    /// </summary>
    /// <remarks>
    /// Over content, not over modification times: a checkout reorders those and would
    /// make every fresh clone look like a change. A tree the digest cannot be taken
    /// over at all reports "unknown", which never matches a recorded hash and
    /// therefore requires the cross-check - the safe direction.
    /// </remarks>
    public static string HashSources(string repositoryRoot, IReadOnlyList<string> globs)
    {
        ArgumentNullException.ThrowIfNull(globs);
        if (string.IsNullOrWhiteSpace(repositoryRoot) || !Directory.Exists(repositoryRoot))
        {
            return CapabilityKeys.Unknown;
        }

        var files = new List<string>();
        foreach (var glob in globs)
        {
            var directory = Path.Combine(repositoryRoot, Path.GetDirectoryName(glob) ?? string.Empty);
            var pattern = Path.GetFileName(glob);
            if (!Directory.Exists(directory))
            {
                continue;
            }

            files.AddRange(Directory.EnumerateFiles(directory, pattern, SearchOption.TopDirectoryOnly));
        }

        if (files.Count == 0)
        {
            return CapabilityKeys.Unknown;
        }

        var material = new StringBuilder();
        foreach (var file in files.Order(StringComparer.Ordinal))
        {
            material.Append(Path.GetFileName(file));
            material.Append(':');
            material.Append(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))));
            material.Append('\n');
        }

        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(material.ToString())))[..16]
            .ToLower(CultureInfo.InvariantCulture);
    }

    /// <summary>Whether the cross-check has to run again, and why.</summary>
    public static (bool Required, string Reason) Required(
        PresentConfirmation? lastConfirmed,
        string presentCodeHash,
        string osBuild)
    {
        if (lastConfirmed is null)
        {
            return (true, "no confirmed present cross-check exists yet");
        }

        var codeMoved = !string.Equals(lastConfirmed.PresentCodeHash, presentCodeHash, StringComparison.Ordinal);
        var osMoved = !string.Equals(MajorOf(lastConfirmed.OsBuild), MajorOf(osBuild), StringComparison.Ordinal);

        return (codeMoved, osMoved) switch
        {
            (true, true) => (true, "the present-diagnostics sources and the Windows version both changed"),
            (true, false) => (true, "the present-diagnostics sources changed since the last confirmation"),
            (false, true) => (true, "the Windows major version changed since the last confirmation"),
            _ => (false,
                $"confirmed by run {lastConfirmed.RunId} against present code {lastConfirmed.PresentCodeHash} " +
                $"on {lastConfirmed.OsBuild}; neither has changed"),
        };
    }

    // Only the major version matters. A cumulative update moves the build every month
    // and does not change which ETW events the present provider emits, so comparing
    // the full version would make the cross-check required forever.
    private static string MajorOf(string version) =>
        string.IsNullOrWhiteSpace(version) ? string.Empty : version.Split('.')[0];
}
