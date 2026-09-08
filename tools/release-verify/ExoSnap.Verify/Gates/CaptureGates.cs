using System.Diagnostics;
using System.Globalization;
using System.Text.RegularExpressions;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-CAP-001: a recording is produced, and an independent tool confirms it is one.
/// </summary>
/// <remarks>
/// The independent half is the point. ExoSnap's own report confirming ExoSnap's own
/// recording proves only that it is self-consistent, so the verdict is not reached
/// until ffprobe has read the bytes.
/// </remarks>
public sealed class RecordingProducedGate : IScenarioBody
{
    /// <summary>How long the recording runs. A product duration, not a synchronisation sleep.</summary>
    public static readonly TimeSpan RecordFor = TimeSpan.FromSeconds(6);

    /// <summary>The shortest container a six-second recording may produce and still be one.</summary>
    public const double MinimumContainerSeconds = 3.0;

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Ffprobe.Available)
        {
            return ScenarioResult.Unavailable(
                "ffprobe is not resolvable; the output cannot be validated independently");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var selected = await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        if (!selected.Ok)
        {
            return ScenarioResult.Fail($"record.selectTarget refused: {selected.Refusal}");
        }

        var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        if (!started.Ok)
        {
            return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
        }

        var state = await session
            .WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);
        if (!string.Equals(state, RecordingStates.Recording, StringComparison.Ordinal))
        {
            return ScenarioResult.Fail($"the session never reached Recording (last: {state})");
        }

        await Task.Delay(RecordFor, cancellationToken).ConfigureAwait(false);

        var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
        await session.AddMarkerAsync(cancellationToken).ConfigureAwait(false);
        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
            .ConfigureAwait(false);

        var result = await session.RecordResultAsync(cancellationToken).ConfigureAwait(false);
        var evidence = new List<Evidence>
        {
            GateEvidence.SaveJson(context, "pipeline.json", pipeline.Result),
            GateEvidence.SaveJson(context, "result.json", result.Result),
        };

        if (!Snapshots.IsTrue(result.Result, "succeeded"))
        {
            return ScenarioResult.Fail("the product reports the recording did not succeed", [.. evidence]);
        }

        var file = Snapshots.Text(result.Result, "outputPath");
        if (string.IsNullOrWhiteSpace(file) || !File.Exists(file))
        {
            return ScenarioResult.Fail(
                $"the product named an output file that does not exist: {file}", [.. evidence]);
        }

        var probe = await services.Ffprobe.InspectAsync(file, cancellationToken).ConfigureAwait(false);
        evidence.Add(GateEvidence.SaveText(context, "ffprobe.json", probe.RawJson));

        var duration = probe.Format.DurationSeconds ?? 0.0;
        var problems = new List<string>();
        if (probe.VideoStreams.Count != 1)
        {
            problems.Add(
                $"expected exactly one video stream, found {probe.VideoStreams.Count.ToString(CultureInfo.InvariantCulture)}");
        }

        if (duration < MinimumContainerSeconds)
        {
            problems.Add(
                $"ffprobe reports only {Format(duration)}s for a {Format(RecordFor.TotalSeconds)} s recording");
        }

        if (problems.Count > 0)
        {
            return ScenarioResult.Fail(string.Join("; ", problems), [.. evidence]);
        }

        return ScenarioResult.Pass(
            $"{Path.GetFileName(file)}: {probe.VideoStreams[0].CodecName} {Format(duration)}s, " +
            $"{probe.Streams.Count.ToString(CultureInfo.InvariantCulture)} stream(s)",
            [.. evidence]);
    }

    private static string Format(double value) => value.ToString("0.###", CultureInfo.InvariantCulture);
}

/// <summary>
/// REL-CAP-QUIET-001: a minimized window stalls silently, and the recording carries on.
/// </summary>
/// <remarks>
/// Silence only means something once the capture has actually stopped producing
/// frames, so two samples four seconds apart establish that first: without them the
/// gate could pass by measuring a perfectly healthy recording.
///
/// The notification check is by title only, and only over entries raised since the
/// gate started. Matching title and body once caught the previous gate's "Recording
/// saved" toast, whose body is the output filename - which contains the probe's window
/// title. The gate then reported that a minimized window had raised it, which was true
/// about the string and false about the product.
/// </remarks>
public sealed partial class QuietStallGate : IScenarioBody
{
    /// <summary>The probe process name, so a leftover instance can be found and ended.</summary>
    public const string ProbeProcessName = "probe_stall_window";

    /// <summary>The environment variable that pins an explicit probe build.</summary>
    public const string ProbePathVariable = "EXOSNAP_STALL_PROBE";

    /// <summary>
    /// How long the probe stays visible before minimising. Longer than the selection
    /// loop below is allowed to take: a minimised window drops out of the
    /// application's window list, so a probe that minimises while the gate is still
    /// resolving its title can never be selected.
    /// </summary>
    public static readonly TimeSpan StallAfter = TimeSpan.FromSeconds(25);

    private static readonly TimeSpan SelectionWindow = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan SilenceWindow = TimeSpan.FromSeconds(4);
    private static readonly TimeSpan SettleWindow = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(500);
    private static readonly string[] RunningLifecycles = ["recording", "paused"];

    private readonly Func<string?>? resolveProbe;

    /// <summary>Creates the gate with the default probe resolution.</summary>
    public QuietStallGate()
        : this(null)
    {
    }

    /// <summary>Creates the gate with an injected probe resolution.</summary>
    /// <param name="resolveProbe">
    /// Where to look for the probe, or null to search the pinning environment variable
    /// and then the build trees under the repository the campaign is bound to.
    /// </param>
    public QuietStallGate(Func<string?>? resolveProbe) => this.resolveProbe = resolveProbe;

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
                $"{ProbeProcessName} is not built (-DEXOSNAP_BUILD_PROBES=ON)");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        // A probe left over from an earlier attempt would hold a window and a capture
        // lease for nothing.
        // The probe puts its own process id in the window title, so the filter can only
        // ever match this probe. Two gates run it back to back and both bind by title;
        // a shared title let the second select the first one's window while the
        // application's target list still held it.
        using var probe = Process.Start(new ProcessStartInfo
        {
            FileName = probePath,
            UseShellExecute = false,
            ArgumentList =
            {
                "--mode", "minimise",
                "--stall-after", ((int)StallAfter.TotalSeconds).ToString(CultureInfo.InvariantCulture),
                "--seconds", "90",
            },
        }) ?? throw new InvalidOperationException($"'{probePath}' could not be started.");

        var title = $"ExoSnap stall probe {probe.Id.ToString(CultureInfo.InvariantCulture)}";

        try
        {
            var sourceName = string.Empty;
            var selected = false;
            var deadline = DateTime.UtcNow + SelectionWindow;
            while (DateTime.UtcNow < deadline)
            {
                await session.SelectTargetAsync("window", title, cancellationToken).ConfigureAwait(false);
                var snapshot = await session.RecordSnapshotAsync(cancellationToken).ConfigureAwait(false);
                sourceName = Snapshots.Text(snapshot.Result, "sourceName");
                if (sourceName.Contains(title, StringComparison.Ordinal))
                {
                    selected = true;
                    break;
                }

                await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
            }

            if (!selected)
            {
                return ScenarioResult.InfrastructureError(
                    $"the probe window was never selectable (source stayed '{sourceName}')");
            }

            // Everything the hub already holds. The hub is a permanent record, so "is
            // there a stall notice" can only be asked about entries that did not exist
            // before this recording.
            var before = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
            var baseline = Snapshots.NotificationEntries(before.Result)
                .Select(entry => Snapshots.Text(entry, "sequence"))
                .ToHashSet(StringComparer.Ordinal);

            await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
            await Task.Delay(StallAfter + TimeSpan.FromSeconds(13), cancellationToken).ConfigureAwait(false);

            var first = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
            var framesFirst = Snapshots.Number(first.Result, "capture.framesCaptured");
            await Task.Delay(SilenceWindow, cancellationToken).ConfigureAwait(false);
            var second = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
            var framesSecond = Snapshots.Number(second.Result, "capture.framesCaptured");

            var notifications = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
            var evidence = new[]
            {
                GateEvidence.SaveJson(context, "pipeline.json", second.Result),
                GateEvidence.SaveJson(context, "notifications.json", notifications.Result),
            };

            if (framesFirst is null || framesSecond is null)
            {
                return ScenarioResult.InfrastructureError(
                    "the pipeline snapshots carry no capture.framesCaptured, so silence was never measured");
            }

            if (framesSecond.Value != framesFirst.Value)
            {
                return ScenarioResult.InfrastructureError(
                    $"the minimized window kept producing frames ({Format(framesFirst.Value)} -> {Format(framesSecond.Value)}); " +
                    "nothing was silent about",
                    evidence);
            }

            var stall = Snapshots.NotificationEntries(notifications.Result)
                .Where(entry => !baseline.Contains(Snapshots.Text(entry, "sequence")))
                .Select(entry => Snapshots.Text(entry, "title"))
                .FirstOrDefault(entryTitle => StallTitle().IsMatch(entryTitle));

            if (stall is not null)
            {
                return ScenarioResult.Fail(
                    $"a minimized window raised '{stall}'; it is documented to stay silent", evidence);
            }

            var lifecycle = Snapshots.Text(second.Result, "lifecycle");
            if (!RunningLifecycles.Contains(lifecycle, StringComparer.Ordinal))
            {
                return ScenarioResult.Fail(
                    $"the recording left the running lifecycle ({lifecycle}) while the window was minimized",
                    evidence);
            }

            return ScenarioResult.Pass(
                $"frames held at {Format(framesSecond.Value)} for {Format(SilenceWindow.TotalSeconds)} s with no notification, lifecycle {lifecycle}",
                evidence);
        }
        finally
        {
            // Stopping is not being stopped: the finalize runs on after the command
            // returns, and the campaign keeps one application session for every gate.
            // Handing the next one a recording still in flight gets it refused at
            // setup, and that failure belongs to this teardown rather than to it.
            await FinishAsync(session, probe).ConfigureAwait(false);
        }
    }

    /// <summary>
    /// Where the probe is, or null. Null rather than a refusal: the probe is a
    /// developer build artifact, and its absence is a statement about the build tree,
    /// never about the product.
    /// </summary>
    public static string? DefaultProbePath() => DefaultProbePath(Environment.CurrentDirectory);

    /// <summary>Where the probe is under a given repository root, or null.</summary>
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
                     "build/windows-x64-release/tools/probes/probe_stall_window/Release/probe_stall_window.exe",
                     "build/windows-x64-debug/tools/probes/probe_stall_window/Debug/probe_stall_window.exe",
                     "build/windows-x64-ninja-release/tools/probes/probe_stall_window/probe_stall_window.exe",
                     "build/windows-x64-ninja-debug/tools/probes/probe_stall_window/probe_stall_window.exe",
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

    [GeneratedRegex("stall|no frame", RegexOptions.IgnoreCase)]
    private static partial Regex StallTitle();

    private static async Task SettleAsync(ILiveVerifySession session)
    {
        await session.StopRecordingAsync(CancellationToken.None).ConfigureAwait(false);

        var deadline = DateTime.UtcNow + SettleWindow;
        while (DateTime.UtcNow < deadline)
        {
            var state = await session.RecordSnapshotAsync(CancellationToken.None).ConfigureAwait(false);
            if (!Snapshots.IsTrue(state.Result, "recording") && !Snapshots.IsTrue(state.Result, "finalizing"))
            {
                return;
            }

            await Task.Delay(PollInterval, CancellationToken.None).ConfigureAwait(false);
        }

        throw new TimeoutException("the recording did not settle before probe teardown");
    }

    internal static async Task FinishAsync(ILiveVerifySession session, Process probe)
    {
        try
        {
            await SettleAsync(session).ConfigureAwait(false);
        }
        finally
        {
            EndProbe(probe);
        }
    }

    private static void EndProbe(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
        }
        catch (System.ComponentModel.Win32Exception)
        {
        }
    }

    private static string Format(double value) => value.ToString("0.###", CultureInfo.InvariantCulture);
}
