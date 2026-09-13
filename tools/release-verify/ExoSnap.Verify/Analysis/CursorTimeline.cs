using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ExoSnap.Verify.Analysis;

/// <summary>A marker the stimulus drew, and the frame it was found in.</summary>
/// <param name="StimulusTimeSeconds">Stimulus time the marker was declared at.</param>
/// <param name="FramePts">Presentation timestamp of the frame it first appears in.</param>
public sealed record MarkerObservation(
    [property: JsonPropertyName("stimulusTimeSeconds")] double StimulusTimeSeconds,
    [property: JsonPropertyName("framePts")] double FramePts);

/// <summary>What a caller measured outside the recording, to place it on the recording's timeline.</summary>
/// <param name="Markers">
/// Every in-band marker that was located. Two markers separated in time also say whether
/// the two timebases ran at the same rate, which one marker cannot.
/// </param>
/// <param name="FrameIntervalSeconds">Nominal frame interval; half of it is a marker's uncertainty.</param>
/// <param name="EngineLogPath">The recorder's JSONL log, for its video-epoch record.</param>
/// <param name="StimulusEpochTicks">The counter value the stimulus calls its own t = 0.</param>
/// <param name="QpcFrequencyHz">Counter frequency, from the stimulus.</param>
public sealed record CursorTimelineInput(
    [property: JsonPropertyName("markers")] IReadOnlyList<MarkerObservation> Markers,
    [property: JsonPropertyName("frameIntervalSeconds")] double FrameIntervalSeconds,
    [property: JsonPropertyName("engineLogPath")] string? EngineLogPath,
    [property: JsonPropertyName("stimulusEpochTicks")] long? StimulusEpochTicks,
    [property: JsonPropertyName("qpcFrequencyHz")] long? QpcFrequencyHz,
    [property: JsonPropertyName("acquireIntervalSeconds")] double AcquireIntervalSeconds = 1.0 / 60.0);

/// <summary>How far two markers disagree about the rate the timebases ran at.</summary>
/// <param name="Measured">Seconds between the markers in the recording.</param>
/// <param name="Declared">Seconds between them in the stimulus schedule.</param>
/// <param name="ToleranceSeconds">What the frame quantisation alone allows.</param>
public sealed record MarkerRateCheck(double Measured, double Declared, double ToleranceSeconds)
{
    /// <summary>Whether the separation agrees within what the quantisation allows.</summary>
    public bool Agrees => Math.Abs(this.Measured - this.Declared) <= this.ToleranceSeconds;

    /// <summary>The check, phrased for a verdict line.</summary>
    public string Evidence =>
        string.Create(
            CultureInfo.InvariantCulture,
            $"markers are {this.Measured:F3} s apart in the recording and {this.Declared:F3} s apart in the "
            + $"schedule, a difference of {Math.Abs(this.Measured - this.Declared):F3} s against a tolerance of "
            + $"{this.ToleranceSeconds:F3} s");
}

/// <summary>
/// The stimulus-to-recording timebase for a cursor run, established without reference to
/// the cursor.
/// </summary>
/// <remarks>
/// The anchor a cursor analysis is entitled to. Every estimate here comes from something
/// the cursor cannot influence: a marker is a flash filling the whole window, and the
/// epoch is the recorder's own reading of when its timeline opened. An analysis that
/// derived its offset from the first frame a cursor sprite appears in would be deriving
/// it from the behaviour under test, and could not report that behaviour as wrong.
/// </remarks>
public static class CursorTimeline
{
    /// <summary>Qualifies a timebase from measurements that do not involve the cursor.</summary>
    public static CursorTimelineResult Qualify(CursorTimelineInput input)
    {
        ArgumentNullException.ThrowIfNull(input);

        var notes = new List<string>();
        var estimates = new List<TimelineAnchorEstimate>();
        var markers = (input.Markers ?? []).OrderBy(marker => marker.StimulusTimeSeconds).ToList();

        // The first marker carries the offset. Later ones are readings of the same
        // source, so they corroborate nothing on their own -- what they add is the
        // rate check below.
        if (markers.Count > 0 && input.FrameIntervalSeconds > 0)
        {
            estimates.Add(TimelineAnchor.FromInBandMarker(
                markers[0].StimulusTimeSeconds, markers[0].FramePts, input.FrameIntervalSeconds));
        }
        else if (markers.Count == 0)
        {
            notes.Add("no in-band marker was located in the recording");
        }

        MarkerRateCheck? rate = null;
        if (markers.Count >= 2 && input.FrameIntervalSeconds > 0)
        {
            var first = markers[0];
            var last = markers[^1];
            rate = new MarkerRateCheck(
                last.FramePts - first.FramePts,
                last.StimulusTimeSeconds - first.StimulusTimeSeconds,
                input.FrameIntervalSeconds);
            notes.Add(rate.Evidence);
        }
        else if (markers.Count == 1)
        {
            notes.Add("only one marker was located, so the two timebases' rates were not compared");
        }

        if (!string.IsNullOrWhiteSpace(input.EngineLogPath))
        {
            AddEpochEstimate(input, estimates, notes);
        }
        else
        {
            notes.Add("no engine log was supplied, so the recorder's own epoch could not corroborate the markers");
        }

        var anchor = TimelineAnchor.Reconcile(estimates);

        // A rate disagreement disqualifies the timebase even when the offsets agree:
        // the two clocks ran at different speeds, so one offset cannot describe the
        // whole run. Carried beside the anchor rather than folded into it, because
        // TimelineAnchor owns what its own rejections mean.
        return new CursorTimelineResult(anchor, rate, new ReadOnlyCollection<string>(notes));
    }

    private static void AddEpochEstimate(
        CursorTimelineInput input, List<TimelineAnchorEstimate> estimates, List<string> notes)
    {
        var read = EngineLog.ReadVideoEpochs(input.EngineLogPath!);
        foreach (var rejection in read.Rejections)
        {
            notes.Add($"an epoch record could not be read: {rejection}");
        }

        if (read.VideoEpochs.Count == 0)
        {
            notes.Add("the engine log carries no video-epoch record");
            return;
        }

        // The last epoch, because a run that restarted its video timeline opened a new
        // one; the records before it describe a timeline the recording no longer has.
        var epoch = read.VideoEpochs[^1];
        if (input.StimulusEpochTicks is not { } stimulusTicks || input.QpcFrequencyHz is not { } frequency)
        {
            notes.Add("the stimulus did not report its counter epoch, so the recorder's epoch has nothing to sit against");
            return;
        }

        // Both epochs on the same axis: the recorder's is already in 100 ns units, the
        // stimulus reports raw counter ticks at its own frequency.
        var stimulusOn100ns = (long)Math.Round(
            (double)stimulusTicks / frequency * VideoEpochRecord.TicksPerSecond);
        var estimate = epoch.ToAnchorEstimate(stimulusOn100ns, input.AcquireIntervalSeconds);
        if (estimate is null)
        {
            // SessionStartFloor is a bound below which the timeline cannot have opened,
            // not a reading of when it did. Turning it into a point estimate would
            // manufacture the corroboration this whole reconciliation exists to require.
            notes.Add(
                $"the video epoch came from {epoch.Source}, which is a bound rather than a reading, "
                + "so it cannot corroborate the markers");
            return;
        }

        estimates.Add(estimate);
    }
}

/// <summary>A qualified timebase, with what was measured to reach it.</summary>
/// <param name="Anchor">The reconciled anchor, usable or rejected.</param>
/// <param name="Rate">The two-marker rate comparison, when two markers were found.</param>
/// <param name="Notes">What was measured or could not be, phrased for a verdict.</param>
public sealed record CursorTimelineResult(
    TimelineAnchor Anchor,
    MarkerRateCheck? Rate,
    ReadOnlyCollection<string> Notes)
{
    /// <summary>
    /// Whether a cursor analysis may proceed on this timebase.
    /// </summary>
    /// <remarks>
    /// Both conditions, because they fail differently: the anchor says the offsets
    /// corroborate one another, and the rate check says one offset can describe the
    /// whole run rather than only its beginning.
    /// </remarks>
    public bool Qualified => this.Anchor.IsEstablished && this.Rate?.Agrees != false;

    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    /// <summary>The result as the JSON an analyzer reads back.</summary>
    public string ToJson() => JsonSerializer.Serialize(
        new
        {
            qualified = this.Qualified,
            offsetSeconds = this.Anchor.OffsetSeconds,
            uncertaintySeconds = this.Anchor.UncertaintySeconds,
            rejection = this.Anchor.Rejection?.ToString(),
            detail = this.Anchor.Explanation,
            estimates = this.Anchor.Estimates.Select(estimate => new
            {
                source = estimate.Source.ToString(),
                offsetSeconds = estimate.OffsetSeconds,
                uncertaintySeconds = estimate.UncertaintySeconds,
                evidence = estimate.Evidence,
            }),
            rate = this.Rate is null ? null : new
            {
                measuredSeconds = this.Rate.Measured,
                declaredSeconds = this.Rate.Declared,
                toleranceSeconds = this.Rate.ToleranceSeconds,
                agrees = this.Rate.Agrees,
            },
            notes = this.Notes,
        },
        Indented);
}
