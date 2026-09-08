using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-AUD-CLOCK-001: a long mixed-clock recording stays in sync.
/// </summary>
/// <remarks>
/// Thirty minutes, because the defect class this catches - resampler drift and drain
/// residue across two clock domains - does not appear in a six-second clip. That is
/// why the checklist calls for a soak rather than a check.
///
/// The drift samples are collected and reported, never asserted: they carry the audio
/// device's own clock residual, which the engine already judges and logs. What decides
/// the verdict is the session report, applied through the shared soak post-checks.
/// </remarks>
public sealed class AudioClockSoakGate : IScenarioBody
{
    /// <summary>How long the soak records.</summary>
    public static readonly TimeSpan SoakFor = TimeSpan.FromMinutes(30);

    /// <summary>How often the drift counters are sampled during the soak.</summary>
    public static readonly TimeSpan SampleEvery = TimeSpan.FromSeconds(30);

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromMinutes(2);
    private static readonly string[] RunningLifecycles = ["recording", "paused"];

    private readonly TimeSpan soakFor;
    private readonly TimeSpan sampleEvery;

    /// <summary>Creates the gate with the release soak duration.</summary>
    public AudioClockSoakGate()
        : this(SoakFor, SampleEvery)
    {
    }

    /// <summary>
    /// Creates the gate with an explicit duration, so its logic can be exercised
    /// without a thirty-minute wait. The expected length the verdict is judged against
    /// is always the duration this gate actually recorded for.
    /// </summary>
    public AudioClockSoakGate(TimeSpan soakFor, TimeSpan sampleEvery)
    {
        this.soakFor = soakFor;
        this.sampleEvery = sampleEvery;
    }

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Ffprobe.Available)
        {
            return ScenarioResult.Unavailable("ffprobe is not resolvable");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        var started = await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        if (!started.Ok)
        {
            return ScenarioResult.Fail($"record.start refused: {started.Refusal}");
        }

        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);

        var samples = new List<DriftSample>();
        var deadline = DateTime.UtcNow + this.soakFor;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(this.sampleEvery, cancellationToken).ConfigureAwait(false);
            var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);

            // The three A/V facts live under `avTiming`, never at the snapshot root,
            // and the group is absent entirely while `valid` is false.
            var lifecycle = Snapshots.Text(pipeline.Result, "lifecycle");
            samples.Add(new DriftSample(
                DateTimeOffset.UtcNow,
                lifecycle,
                Snapshots.Number(pipeline.Result, "avTiming.avDriftMs"),
                Snapshots.Text(pipeline.Result, "avTiming.avDriftAvailability")));

            if (!RunningLifecycles.Contains(lifecycle, StringComparer.Ordinal))
            {
                break;
            }
        }

        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
            .ConfigureAwait(false);

        var result = await session.RecordResultAsync(cancellationToken).ConfigureAwait(false);
        var report = await session.SessionLatestAsync(cancellationToken).ConfigureAwait(false);

        var evidence = new List<Evidence>
        {
            GateEvidence.SaveText(
                context,
                "drift-samples.json",
                JsonSerializer.Serialize<IReadOnlyList<DriftSample>>(samples, GateJson.Default.IReadOnlyListDriftSample)),
            GateEvidence.SaveJson(context, "session.json", report.Result),
        };

        if (!Snapshots.IsTrue(result.Result, "succeeded"))
        {
            return ScenarioResult.Fail("the long recording did not succeed", [.. evidence]);
        }

        var outputPath = Snapshots.Text(result.Result, "outputPath");
        var probe = await services.Ffprobe.InspectAsync(outputPath, cancellationToken).ConfigureAwait(false);
        evidence.Add(GateEvidence.SaveText(context, "ffprobe.json", probe.RawJson));

        var audioIndexes = probe.AudioStreams.Select(stream => stream.Index).ToList();
        if (audioIndexes.Count == 0)
        {
            return ScenarioResult.Fail("the soak recording carries no audio track", [.. evidence]);
        }

        var spans = await services.Ffprobe
            .PacketSpanSecondsAsync(outputPath, audioIndexes, cancellationToken)
            .ConfigureAwait(false);

        var verdict = RecordingIntegrity.SoakVerdict(
            report.Result,
            probe.Format.DurationSeconds ?? 0.0,
            this.soakFor.TotalSeconds,
            spans);

        return verdict with
        {
            Message =
                $"{this.soakFor.TotalMinutes.ToString("0.##", CultureInfo.InvariantCulture)} min recorded, " +
                $"{samples.Count.ToString(CultureInfo.InvariantCulture)} drift samples; {verdict.Message}",
            Evidence = [.. evidence],
        };
    }
}
