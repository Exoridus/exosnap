using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-JOURNEY-001: the full product journey, launch to export to restart.
/// </summary>
/// <remarks>
/// The journey script already asserts the whole chain in one process. It is invoked
/// rather than reimplemented so the two cannot drift, and its own evidence file is
/// carried into this campaign's evidence.
///
/// A journey launches its own isolated instance, so the campaign's shared session must
/// be out of the way first: the application enforces a single-instance guard, and a
/// second launch would hand focus to the first one and exit.
/// </remarks>
public sealed class ProductJourneyGate : IScenarioBody
{
    /// <summary>The script this gate delegates to, relative to the repository root.</summary>
    public const string ScriptPath = "scripts/live-verify-product-journey.ps1";

    /// <summary>How long the journey records inside the script.</summary>
    public const int RecordSeconds = 8;

    private static readonly TimeSpan JourneyTimeout = TimeSpan.FromMinutes(10);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var script = Path.Combine(services.Artifact.RepositoryRoot, ScriptPath);
        if (!File.Exists(script))
        {
            return ScenarioResult.Unavailable($"{ScriptPath} is missing");
        }

        await services.Sessions.EndAsync().ConfigureAwait(false);

        var evidencePath = Path.Combine(context.EvidenceDirectory, "journey.json");
        Directory.CreateDirectory(context.EvidenceDirectory);

        var run = await services.Processes.RunAsync(
            new ProcessRunRequest(
                "pwsh",
                "-NoProfile", "-File", script,
                "-AppPath", services.Artifact.ExecutablePath,
                "-RecordSeconds", RecordSeconds.ToString(CultureInfo.InvariantCulture),
                "-EvidencePath", evidencePath)
            {
                WorkingDirectory = services.Artifact.RepositoryRoot,
                Timeout = JourneyTimeout,
            },
            cancellationToken).ConfigureAwait(false);

        var evidence = new List<Evidence>
        {
            GateEvidence.SaveText(context, "journey.log", run.StandardOutput + run.StandardError),
        };

        if (File.Exists(evidencePath))
        {
            evidence.Add(Evidence.ForFile("journey", evidencePath));
        }

        if (run.TimedOut)
        {
            return ScenarioResult.InfrastructureError(
                "the product journey did not finish within its deadline", [.. evidence]);
        }

        if (run.ExitCode != 0)
        {
            return ScenarioResult.InfrastructureError(
                $"the product journey exited {run.ExitCode.ToString(CultureInfo.InvariantCulture)} without a typed product-defect result", [.. evidence]);
        }

        return ScenarioResult.Pass(
            "launch, record, marker, pause/resume, stop, edit, export, restart", [.. evidence]);
    }
}

/// <summary>
/// REL-SHUTDOWN-001: unread control-channel events never hold the application open.
/// </summary>
/// <remarks>
/// The gate deliberately generates events and then never reads them, which is what a
/// runner waiting for the process to exit looks like from the server's side. The
/// defect it pins let one unread event block the process forever, because the server
/// flushed the pipe on the stop path and the flush waits for a client that is by
/// definition not reading.
///
/// Its own instance, its own run id, so the shared session goes first for the same
/// single-instance reason the journey gate has.
/// </remarks>
public sealed class ShutdownGate : IScenarioBody
{
    /// <summary>How long the process is given to exit with events left unread.</summary>
    public static readonly TimeSpan ExitDeadline = TimeSpan.FromSeconds(20);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        await services.Sessions.EndAsync().ConfigureAwait(false);

        var session = await services.SessionFactory
            .LaunchAsync(services.Artifact.ExecutablePath, cancellationToken)
            .ConfigureAwait(false);

        await using (session.ConfigureAwait(false))
        {
            await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
            await session.InvokeAsync(
                    "ui.navigate",
                    new Dictionary<string, object?>(StringComparer.Ordinal) { ["page"] = "diagnostics" },
                    cancellationToken)
                .ConfigureAwait(false);
            await session.InvokeAsync("diagnostics.run", null, cancellationToken).ConfigureAwait(false);

            var buffered = session.BufferedEventCount;

            // Close the channel with events still buffered, then ask the window to
            // close. No event drain here. On purpose.
            var shutdown = await session.ShutdownAsync(ExitDeadline, cancellationToken).ConfigureAwait(false);

            var evidence = GateEvidence.SaveText(
                context,
                "shutdown.json",
                JsonSerializer.Serialize(
                    new ShutdownObservation(shutdown.ToString(), ExitDeadline.TotalSeconds, buffered),
                    GateJson.Default.ShutdownObservation));

            var seconds = ExitDeadline.TotalSeconds.ToString("0", CultureInfo.InvariantCulture);
            return shutdown switch
            {
                SessionShutdown.Exited =>
                    ScenarioResult.Pass("exited promptly with unread events buffered", evidence),
                SessionShutdown.StillRunning =>
                    ScenarioResult.Fail(
                        $"the application did not exit within {seconds} s with events left unread", evidence),

                // Nothing was asked, so nothing was learned. This gate needs a real
                // desktop, and running it against a windowless instance measures the
                // harness rather than the product.
                _ => ScenarioResult.InfrastructureError(
                    "the application owns no window a close request could reach, so the exit was never requested",
                    evidence),
            };
        }
    }
}

/// <summary>
/// REL-UPD-PORTABLE-001: the portable update handoff installs the version it pinned.
/// </summary>
/// <remarks>
/// The current handoff script follows a live feed and cannot bind its target to the
/// campaign artifact. Until that boundary carries verified candidate evidence, the
/// gate reports unavailable and does not launch an update.
/// </remarks>
public sealed class PortableUpdateGate : IScenarioBody
{
    /// <summary>The script this gate delegates to, relative to the repository root.</summary>
    public const string ScriptPath = "scripts/live-verify-update-handoff.ps1";

    /// <summary>The variable naming the older build the update starts from.</summary>
    public const string UpdateFromVariable = "EXOSNAP_UPDATE_FROM";

    private readonly Func<string?> readUpdateFrom;

    /// <summary>Creates the gate reading the real process environment.</summary>
    public PortableUpdateGate()
        : this(() => Environment.GetEnvironmentVariable(UpdateFromVariable))
    {
    }

    /// <summary>Creates the gate with an injected environment reader.</summary>
    public PortableUpdateGate(Func<string?> readUpdateFrom)
    {
        ArgumentNullException.ThrowIfNull(readUpdateFrom);
        this.readUpdateFrom = readUpdateFrom;
    }

    /// <inheritdoc/>
    public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return Task.FromResult(this.CheckAvailability(context));
    }

    private ScenarioResult CheckAvailability(ScenarioContext context)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var script = Path.Combine(services.Artifact.RepositoryRoot, ScriptPath);
        if (!File.Exists(script))
        {
            return ScenarioResult.Unavailable($"{ScriptPath} is missing");
        }

        var from = this.readUpdateFrom();
        if (string.IsNullOrWhiteSpace(from) || !File.Exists(from))
        {
            return ScenarioResult.Unavailable(
                $"set {UpdateFromVariable} to an older official exosnap.exe; the bound artifact " +
                "is the newest release and can only ever report up-to-date");
        }

        return ScenarioResult.Unavailable(
            "the handoff script follows a live feed and cannot verify that its installed bytes match the bound candidate");
    }
}
