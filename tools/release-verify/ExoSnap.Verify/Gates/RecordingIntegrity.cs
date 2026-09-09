using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// What a session report and its container say about one finished recording.
/// </summary>
/// <remarks>
/// The criteria a recording has to meet to be complete and continuous do not depend
/// on how long it ran, so they live here and not in the soak gate that first needed
/// them: the audio has to cover the container, no source may have degraded to
/// silence, the resampler drain may not have dropped a tail, and every segment has to
/// be finalized. The soak verdict adds what is specific to a long run - the duration
/// skew against a known expected length, the outage budget, and the zero-tolerance
/// counters.
/// </remarks>
public static class RecordingIntegrity
{
    /// <summary>Longest single audio outage a listener would not notice, in milliseconds.</summary>
    private const double LongestOutageMs = 120.0;

    /// <summary>Total audio outage budget, as a fraction of the recording.</summary>
    private const double OutageBudgetFraction = 0.001;

    /// <summary>Container duration skew a soak may carry, as a fraction of the expected length.</summary>
    private const double DurationSkewFraction = 0.02;

    /// <summary>Counters that must be exactly zero for a recording to be clean.</summary>
    private static readonly string[] ZeroToleranceCounters =
    [
        "mux_failures",
        "encoder_keyframe_prediction_mismatches",
        "frames_dropped.processing_failure",
        "frames_dropped.backpressure",
    ];

    /// <summary>
    /// Everything wrong with one recording, as findings. An empty list means nothing
    /// is.
    /// </summary>
    /// <param name="report">The session report, already unwrapped from its envelope.</param>
    /// <param name="containerSeconds">The container duration an independent tool measured.</param>
    /// <param name="audioSpanSeconds">First-to-last packet span of each audio stream.</param>
    public static ReadOnlyCollection<string> Problems(
        JsonElement report,
        double containerSeconds,
        IReadOnlyList<double> audioSpanSeconds)
    {
        ArgumentNullException.ThrowIfNull(audioSpanSeconds);
        var problems = new List<string>();

        foreach (var span in audioSpanSeconds)
        {
            if (span < containerSeconds * 0.99)
            {
                problems.Add($"an audio track spans {Seconds(span)}s of a {Seconds(containerSeconds)}s container");
            }
        }

        if (Snapshots.IsTrue(report, "audio.degraded_occurred"))
        {
            problems.Add("audio.degraded_occurred is true");
        }

        foreach (var drain in Snapshots.Items(report, "audio.resampler_drain"))
        {
            var undrained = Snapshots.Number(drain, "undrained_frames");
            if (undrained is not null && undrained.Value != 0)
            {
                problems.Add(
                    $"track {Snapshots.Text(drain, "track")} left {Format(undrained.Value)} undrained frame(s)");
            }
        }

        foreach (var segment in Snapshots.Items(report, "segments"))
        {
            if (!Snapshots.IsTrue(segment, "finalized"))
            {
                problems.Add($"segment {Snapshots.Text(segment, "index")} is not finalized");
            }
        }

        return new ReadOnlyCollection<string>(problems);
    }

    /// <summary>
    /// The soak post-checks applied to one session report.
    /// </summary>
    /// <remarks>
    /// The container being as long as the recording is the weakest of the checks, and
    /// on its own it passes a run whose audio drain dropped its tail, whose muxer
    /// failed, or whose segments were never finalized. Every counter read here is
    /// already in the report the gate fetches, so reading them invents no threshold -
    /// it stops discarding the evidence.
    ///
    /// Audio outages are judged by the time a listener lost, not by how often the
    /// operating system fell behind: a machine under real load misses buffers, and the
    /// engine answers each miss with exactly as much silence, keeping the track
    /// aligned with video. Demanding zero would fail the product for behaving
    /// correctly. What this cannot know is whether anything was playing across the
    /// gap, so the finding says what was measured and stops short of claiming it was
    /// heard.
    ///
    /// The A/V drift counters are reported, never asserted. They carry the audio
    /// device's own clock residual, which the engine already judges and logs; a device
    /// whose clock leaves the correction envelope is a statement about that device,
    /// and the file it produced can still be correct.
    /// </remarks>
    public static ScenarioResult SoakVerdict(
        JsonElement report,
        double containerSeconds,
        double expectedSeconds,
        IReadOnlyList<double> audioSpanSeconds)
    {
        ArgumentNullException.ThrowIfNull(audioSpanSeconds);
        report = Unwrap(report);

        var counters = Snapshots.Value(report, "counters");
        if (counters.ValueKind != JsonValueKind.Object)
        {
            return ScenarioResult.InfrastructureError(
                "no session report, so the soak post-checks were not performed");
        }

        var requiredCounters = ZeroToleranceCounters.Concat(
            ["audio_discontinuity_ms_total", "audio_discontinuity_ms_longest", "audio_discontinuities"]);
        var missing = requiredCounters.Where(path => Snapshots.Number(counters, path) is null).ToList();
        if (missing.Count > 0)
        {
            return ScenarioResult.InfrastructureError(
                "missing or malformed report counters: " + string.Join(", ", missing.Select(path => $"counters.{path}")));
        }

        var problems = new List<string>(Problems(report, containerSeconds, audioSpanSeconds));

        var skew = Math.Abs(containerSeconds - expectedSeconds);
        if (skew > expectedSeconds * DurationSkewFraction)
        {
            problems.Add(
                $"container {Seconds(containerSeconds)}s vs {Seconds(expectedSeconds)}s recorded (skew {Seconds(skew)}s)");
        }

        var totalMs = Snapshots.Number(counters, "audio_discontinuity_ms_total");
        var longestMs = Snapshots.Number(counters, "audio_discontinuity_ms_longest");
        var outages = Snapshots.Number(counters, "audio_discontinuities");

        if (totalMs is null || longestMs is null)
        {
            problems.Add("the audio discontinuity duration counters are absent from the report");
        }
        else
        {
            var budgetMs = expectedSeconds * 1000.0 * OutageBudgetFraction;
            if (totalMs.Value > budgetMs)
            {
                problems.Add(
                    $"audio lost {Format(totalMs.Value)} ms across {Format(outages)} outage(s), over the " +
                    $"{Format(budgetMs)} ms budget for a {Seconds(expectedSeconds)}s recording");
            }

            if (longestMs.Value > LongestOutageMs)
            {
                problems.Add(
                    $"the longest single audio outage was {Format(longestMs.Value)} ms; long enough to be heard if " +
                    "anything was playing at the time, which this report cannot say");
            }
        }

        foreach (var path in ZeroToleranceCounters)
        {
            var value = Snapshots.Number(counters, path);
            if (value is null)
            {
                problems.Add($"counters.{path} is absent from the report");
                continue;
            }

            if (value.Value != 0)
            {
                problems.Add($"counters.{path} = {Format(value.Value)}");
            }
        }

        var reported =
            $"av_drift_ms {Format(Snapshots.Number(counters, "av_drift_ms"))}, " +
            $"peak {Format(Snapshots.Number(counters, "peak_av_drift_ms"))} " +
            "(device clock residual, reported not asserted); " +
            $"audio outages {Format(outages)} totalling {Format(totalMs)} ms, longest {Format(longestMs)} ms";

        return problems.Count > 0
            ? ScenarioResult.Fail(string.Join("; ", problems) + $"; {reported}")
            : ScenarioResult.Pass(
                $"container {Seconds(containerSeconds)}s, audio spans {Spans(audioSpanSeconds)}, " +
                $"post-checks clean; {reported}");
    }

    /// <summary>
    /// Whether one recording is complete and continuous, when the gate's subject is
    /// the device it came from rather than its length.
    /// </summary>
    /// <remarks>
    /// It cannot see the endpoint format, and nothing downstream can: every capture
    /// path opens the endpoint with automatic PCM conversion, so Windows has already
    /// resampled to 48 kHz before a frame reaches the engine. What it proves is the
    /// other half - that recording from that endpoint produced a whole, gapless file.
    /// </remarks>
    public static ScenarioResult Verdict(
        JsonElement report,
        double containerSeconds,
        IReadOnlyList<double> audioSpanSeconds)
    {
        ArgumentNullException.ThrowIfNull(audioSpanSeconds);
        report = Unwrap(report);

        if (Snapshots.Value(report, "counters").ValueKind != JsonValueKind.Object)
        {
            return ScenarioResult.InfrastructureError("no session report, so the recording was not checked");
        }

        if (audioSpanSeconds.Count == 0)
        {
            return ScenarioResult.InfrastructureError(
                "the output carries no audio track to measure, so nothing was checked about the endpoint");
        }

        var problems = Problems(report, containerSeconds, audioSpanSeconds);
        return problems.Count > 0
            ? ScenarioResult.Fail(string.Join("; ", problems))
            : ScenarioResult.Pass(
                $"audio spans {Spans(audioSpanSeconds)} of a {Seconds(containerSeconds)}s container, " +
                "no degradation, nothing undrained, every segment finalized");
    }

    /// <summary>
    /// The report itself, whether the caller holds the <c>session.latest</c> envelope
    /// or the report inside it.
    /// </summary>
    /// <remarks>
    /// Both are accepted so a gate cannot be wired to the wrong one and report "no
    /// session report" after a thirty-minute recording.
    /// </remarks>
    public static JsonElement Unwrap(JsonElement reportOrEnvelope)
    {
        var inner = Snapshots.Value(reportOrEnvelope, "report");
        return inner.ValueKind == JsonValueKind.Object ? inner : reportOrEnvelope;
    }

    private static string Spans(IReadOnlyList<double> spans) =>
        spans.Count == 0 ? "no audio track" : string.Join("s, ", spans.Select(Seconds)) + "s";

    private static string Seconds(double value) => value.ToString("0.###", CultureInfo.InvariantCulture);

    private static string Format(double? value) =>
        value is null ? "(absent)" : value.Value.ToString("0.###", CultureInfo.InvariantCulture);
}
