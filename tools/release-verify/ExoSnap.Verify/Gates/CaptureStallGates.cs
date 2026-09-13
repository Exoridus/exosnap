using System.Diagnostics;
using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-CAP-STALL-001: a stalled window capture is reported honestly and stays controllable.
/// </summary>
/// <remarks>
/// The sibling of <see cref="QuietStallGate"/>, and the other half of the product's
/// documented rule. A minimized window that stops producing frames is supposed to be
/// silent; a window that is fullscreen-shaped, alive and visible is supposed to say
/// so. The probe produces the second shape by covering the monitor and then freezing,
/// which is a documented call on a window this campaign created -- no synthesised
/// input, no foreign window, and nothing asked of a person.
///
/// What makes the gate worth running is the third assertion. An explanation that
/// blames exclusive fullscreen is only accepted when a present mode was actually
/// measured as one: a product guessing it from window shape would be right often
/// enough to look correct while telling the user something nobody checked.
/// </remarks>
public sealed class CaptureStallGate : IScenarioBody
{
    private static readonly TimeSpan SelectionWindow = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan NoticeWindow = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(500);
    private static readonly TimeSpan FreezeAfter = TimeSpan.FromSeconds(8);
    private static readonly TimeSpan ProbeLifetime = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);

    private readonly Func<string?>? resolveProbe;

    /// <summary>Creates the gate with the default probe resolution.</summary>
    public CaptureStallGate()
        : this(null)
    {
    }

    /// <summary>Creates the gate with an injected probe resolution.</summary>
    public CaptureStallGate(Func<string?>? resolveProbe) => this.resolveProbe = resolveProbe;

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var probePath = this.resolveProbe is null
            ? QuietStallGate.DefaultProbePath(services.Artifact.RepositoryRoot)
            : this.resolveProbe();
        if (probePath is null)
        {
            return ScenarioResult.Unavailable(
                $"{QuietStallGate.ProbeProcessName} is not built (-DEXOSNAP_BUILD_PROBES=ON)");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        // freeze, not minimise: the probe becomes a borderless window covering the
        // monitor and then stops repainting, which is the one shape the product
        // reports. QuietStallGate covers the silent case.
        using var probe = Process.Start(ProbeStart.For(
            probePath,
            "--mode", "freeze",
            "--stall-after", ((int)FreezeAfter.TotalSeconds).ToString(CultureInfo.InvariantCulture),
            "--seconds", ((int)ProbeLifetime.TotalSeconds).ToString(CultureInfo.InvariantCulture)));

        if (probe is null)
        {
            return ScenarioResult.InfrastructureError($"{QuietStallGate.ProbeProcessName} did not start");
        }

        // The probe puts its own process id in the window title, so the filter can
        // only ever match this probe: two gates run it back to back and both bind by
        // title, and a shared one let the second select the first one's window.
        var title = $"ExoSnap stall probe {probe.Id}";
        try
        {
            var refusal = await SelectProbeWindowAsync(session, title, cancellationToken).ConfigureAwait(false);
            if (refusal is not null)
            {
                return ScenarioResult.InfrastructureError(refusal);
            }

            var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
            if (!started.Ok)
            {
                return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
            }

            await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
                .ConfigureAwait(false);

            return await ObserveStallAsync(context, session, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            try
            {
                await session.StopRecordingAsync(CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception exception) when (exception is not OperationCanceledException)
            {
                // The recording is the probe's and the probe is about to end. A stop
                // that refuses here must not replace the verdict above it.
            }

            if (!probe.HasExited)
            {
                probe.Kill();
            }
        }
    }

    /// <summary>
    /// The title and body of a standing capture-stall notice, or an empty string.
    /// </summary>
    /// <remarks>
    /// The hub's entries carry no id and no detail; the text is what the honesty rule
    /// reads, and both halves of it are searched because the product puts the subject
    /// in one and the explanation in the other.
    /// </remarks>
    internal static string StallNoticeText(JsonElement notifications)
    {
        foreach (var entry in Snapshots.NotificationEntries(notifications))
        {
            var text = $"{Snapshots.Text(entry, "title")} {Snapshots.Text(entry, "body")}".Trim();
            if (text.Contains("stall", StringComparison.OrdinalIgnoreCase)
                || text.Contains("no frame", StringComparison.OrdinalIgnoreCase))
            {
                return text;
            }
        }

        return string.Empty;
    }

    private static async Task<string?> SelectProbeWindowAsync(
        ILiveVerifySession session, string title, CancellationToken cancellationToken)
    {
        // The window has to exist before it can be selected, and the application
        // enumerates windows when asked rather than watching for new ones. The
        // selection confirms itself: sourceName is what the application resolved the
        // filter to.
        var deadline = DateTime.UtcNow + SelectionWindow;
        var resolved = string.Empty;
        while (DateTime.UtcNow < deadline)
        {
            await session.SelectTargetAsync("window", title, cancellationToken).ConfigureAwait(false);
            var snapshot = await session.RecordSnapshotAsync(cancellationToken).ConfigureAwait(false);
            resolved = Snapshots.Text(snapshot.Result, "sourceName");
            if (resolved.Contains(title, StringComparison.Ordinal))
            {
                return null;
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }

        return $"the stall probe window was never selectable (the source stayed '{resolved}')";
    }

    private static async Task<ScenarioResult> ObserveStallAsync(
        ScenarioContext context, ILiveVerifySession session, CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow + NoticeWindow;
        var noticeText = string.Empty;
        JsonElement notifications = default;
        while (DateTime.UtcNow < deadline)
        {
            var snapshot = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
            notifications = snapshot.Result;
            noticeText = StallNoticeText(notifications);
            if (noticeText.Length > 0)
            {
                break;
            }

            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
        }

        var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "notifications.json", notifications),
            GateEvidence.SaveJson(context, "pipeline.json", pipeline.Result),
        };

        return CaptureStallHonesty.Verdict(
            new StallObservation(
                noticeText.Length > 0,
                noticeText,
                Snapshots.Text(pipeline.Result, "lifecycle"),
                Snapshots.Text(pipeline.Result, "sourcePresentation.presentMode"),
                Snapshots.Text(pipeline.Result, "sourcePresentation.modeAvailability")),
            evidence);
    }
}

/// <summary>
/// REL-CAP-FSE-001: true exclusive fullscreen is detected and explained.
/// </summary>
/// <remarks>
/// The separation this gate exists for: a borderless window covering a monitor looks
/// the same to a person and is a completely different capture path. Window shape
/// decides nothing here -- only a present measurement does, and a run without one is
/// unavailable rather than passing on a guess.
///
/// The probe takes a display into real exclusive fullscreen through
/// <c>SetFullscreenState</c>, releases it itself and exits on its own deadline. Its
/// window is selected before the measurement is read because present statistics
/// follow the selected capture target: without that the attribution counts every
/// process on the desktop and the verdict describes whatever presented last.
///
/// The independent PresentMon comparison is not repeated here. It needs a live
/// elevated ETW session, and REL-PRESENT-XCHECK-001 already owns that reading against
/// a capture the disposable guest takes.
/// </remarks>
public sealed class ExclusiveFullscreenGate : IScenarioBody
{
    /// <summary>The environment variable that pins an explicit probe build.</summary>
    public const string ProbePathVariable = "EXOSNAP_FSE_PROBE";

    /// <summary>The probe's window title, which the capture target is bound to.</summary>
    public const string ProbeWindowTitle = "ExoSnap FSE probe";

    private static readonly TimeSpan WindowAppears = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan ModeSettles = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ProbeLifetime = TimeSpan.FromSeconds(45);

    private readonly Func<string?>? resolveProbe;

    /// <summary>Creates the gate with the default probe resolution.</summary>
    public ExclusiveFullscreenGate()
        : this(null)
    {
    }

    /// <summary>Creates the gate with an injected probe resolution.</summary>
    public ExclusiveFullscreenGate(Func<string?>? resolveProbe) => this.resolveProbe = resolveProbe;

    /// <summary>Where the fullscreen probe is under a given repository root, or null.</summary>
    public static string? DefaultProbePath(string repositoryRoot)
    {
        var pinned = Environment.GetEnvironmentVariable(ProbePathVariable);
        if (!string.IsNullOrWhiteSpace(pinned) && File.Exists(pinned))
        {
            return Path.GetFullPath(pinned);
        }

        if (string.IsNullOrWhiteSpace(repositoryRoot))
        {
            return null;
        }

        foreach (var candidate in new[]
                 {
                     "build/windows-x64-release/tools/probes/probe_fullscreen_present/Release/probe_fullscreen_present.exe",
                     "build/windows-x64-debug/tools/probes/probe_fullscreen_present/Debug/probe_fullscreen_present.exe",
                     "build/windows-x64-ninja-release/tools/probes/probe_fullscreen_present/probe_fullscreen_present.exe",
                     "build/windows-x64-ninja-debug/tools/probes/probe_fullscreen_present/probe_fullscreen_present.exe",
                 })
        {
            var path = Path.Combine(repositoryRoot, candidate.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(path))
            {
                return Path.GetFullPath(path);
            }
        }

        return null;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var probePath = this.resolveProbe is null
            ? DefaultProbePath(services.Artifact.RepositoryRoot)
            : this.resolveProbe();
        if (probePath is null)
        {
            return ScenarioResult.Unavailable(
                "probe_fullscreen_present is not built (-DEXOSNAP_BUILD_PROBES=ON); no API makes another process "
                + "take exclusive fullscreen, so there is nothing to measure without it");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        // Checked before the probe runs at all: with the opt-in off or the session
        // unelevated, no probe can make this measurable, and an unmet precondition is
        // unavailable rather than a failure earned by a scenario nobody could pass.
        var before = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        if (!Snapshots.IsTrue(before.Result, "present.available"))
        {
            var availability = Snapshots.Text(before.Result, "present.availability");
            return ScenarioResult.Unavailable(
                $"present diagnostics are unavailable ({availability}); run the elevated present scenario first");
        }

        using var probe = Process.Start(ProbeStart.For(
            probePath,
            "--display", "0",
            "--seconds", ((int)ProbeLifetime.TotalSeconds).ToString(CultureInfo.InvariantCulture)));

        if (probe is null)
        {
            return ScenarioResult.InfrastructureError("probe_fullscreen_present did not start");
        }

        try
        {
            await Task.Delay(WindowAppears, cancellationToken).ConfigureAwait(false);

            var selected = await session.SelectTargetAsync("window", ProbeWindowTitle, cancellationToken)
                .ConfigureAwait(false);
            if (!selected.Ok)
            {
                return ScenarioResult.InfrastructureError(
                    "the probe window could not be selected, so the present statistics would describe the desktop "
                    + $"rather than it: {selected.Refusal}");
            }

            await Task.Delay(ModeSettles, cancellationToken).ConfigureAwait(false);

            var measured = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
            var evidence = new[] { GateEvidence.SaveJson(context, "present.json", measured.Result) };

            return ExclusiveFullscreenDetection.Verdict(
                new PresentMeasurement(
                    Snapshots.IsTrue(measured.Result, "present.available"),
                    Snapshots.Text(measured.Result, "present.availability"),
                    Snapshots.Text(measured.Result, "present.mode"),
                    Snapshots.Number(measured.Result, "present.presentCount") ?? 0),
                string.Empty,
                evidence);
        }
        finally
        {
            if (!probe.HasExited)
            {
                probe.Kill();
            }
        }
    }
}

/// <summary>Starting a probe with an argument list rather than a command line.</summary>
/// <remarks>
/// Windows re-parses a command line in the child, so a single string turns a value
/// containing a space into a different invocation than the one intended.
/// </remarks>
internal static class ProbeStart
{
    internal static ProcessStartInfo For(string fileName, params string[] arguments)
    {
        var info = new ProcessStartInfo { FileName = fileName, UseShellExecute = false };
        foreach (var argument in arguments)
        {
            info.ArgumentList.Add(argument);
        }

        return info;
    }
}
