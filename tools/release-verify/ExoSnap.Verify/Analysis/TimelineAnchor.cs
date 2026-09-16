using System.Collections.ObjectModel;
using System.Globalization;

namespace ExoSnap.Verify.Analysis;

/// <summary>Where one estimate of the stimulus-to-recording offset came from.</summary>
public enum AnchorSource
{
    /// <summary>
    /// Wall-clock timestamps written independently by the stimulus and the recorder.
    /// Cheap and always available, but only as precise as the recorder's start-up latency
    /// is known.
    /// </summary>
    WallClock,

    /// <summary>
    /// A marker the stimulus drew into the captured surface at a declared stimulus time,
    /// located in the decoded frames. Precise to one frame interval, and independent of
    /// every clock.
    /// </summary>
    InBandMarker,

    /// <summary>
    /// Matching QueryPerformanceCounter readings logged by the stimulus and the capture
    /// path. The most precise anchor, and the only one that survives a machine whose
    /// wall clock is slewed mid-run.
    /// </summary>
    PerformanceCounter,
}

/// <summary>What a piece of timing evidence actually says about the offset.</summary>
/// <remarks>
/// Not every time a run records is a reading of the offset. A counter logged on both
/// sides of the same instant is one. A transition rendered by the stimulus and then seen
/// in the recording is not: between the paint and the frame that carries it sits a
/// capture latency that is unknown, non-negative, and not the same on every machine --
/// about a third of a frame on bare metal and two frames in a GPU-partitioned guest.
/// Treating such a marker as a point estimate pulls the anchor towards the paint time by
/// exactly that latency, and averaging it with a counter reading makes the result worse
/// than the counter alone while calling it corroboration.
/// </remarks>
public enum TimelineEvidenceKind
{
    /// <summary>A reading of the offset, usable on its own.</summary>
    PointEstimate,

    /// <summary>
    /// The offset cannot be smaller than this. What a rendered marker gives: the frame
    /// carrying it cannot precede the paint that produced it.
    /// </summary>
    LowerBound,

    /// <summary>The offset cannot be larger than this.</summary>
    UpperBound,
}

/// <summary>One piece of evidence about how far the recording's timebase sits from the stimulus's.</summary>
/// <param name="Source">Where the evidence came from.</param>
/// <param name="OffsetSeconds">Add this to a presentation timestamp to get a stimulus time.</param>
/// <param name="UncertaintySeconds">
/// Half-width of the interval this evidence allows. Never zero: evidence presented as
/// exact cannot be reconciled with anything else, because any disagreement at all then
/// reads as a contradiction.
/// </param>
/// <param name="Evidence">What was measured, phrased for a verdict line.</param>
/// <param name="Kind">
/// Whether this reads the offset or only bounds it. Defaulted to a point estimate so a
/// caller that supplies a genuine reading says nothing extra; evidence that merely
/// constrains has to say so.
/// </param>
public sealed record TimelineAnchorEstimate(
    AnchorSource Source,
    double OffsetSeconds,
    double UncertaintySeconds,
    string Evidence,
    TimelineEvidenceKind Kind = TimelineEvidenceKind.PointEstimate);

/// <summary>Why a reconciliation of several anchors did not produce a usable timebase.</summary>
public enum AnchorRejection
{
    /// <summary>
    /// No usable reading of the offset, or nothing independent to check it against.
    /// Bounds alone establish nothing: they say where the offset is not.
    /// </summary>
    NotCorroborated,

    /// <summary>
    /// Two readings disagree by more than their uncertainties allow, or a reading falls
    /// outside what a bound permits.
    /// </summary>
    Contradicted,
}

/// <summary>
/// The stimulus-to-recording offset, established from independent estimates that agree.
/// </summary>
/// <remarks>
/// <para>
/// The offset cannot be derived from the thing under test. An earlier analysis found the
/// first frame in which a cursor sprite appeared and declared that frame to be the first
/// logged pointer move; every later check then measured the sprite against a timeline the
/// sprite itself had defined. A recording in which the cursor never followed the pointer
/// pins the offset onto whatever else changed first -- a window border, a blinking caret --
/// and the run reports a large offset and a plausible-looking pass. That is how a -13.5 s
/// offset was once read as a measurement rather than as a broken anchor.
/// </para>
/// <para>
/// So the offset comes from at least two sources that do not depend on the cursor, and
/// they have to agree inside their stated uncertainties. Two agreeing anchors give a
/// timebase; one gives nothing, and two that disagree mean the run cannot be judged --
/// which is an infrastructure result, not a product failure.
/// </para>
/// </remarks>
public sealed class TimelineAnchor
{
    private static string Seconds(double value) => value.ToString("F3", CultureInfo.InvariantCulture);

    private TimelineAnchor(
        double offsetSeconds,
        double uncertaintySeconds,
        ReadOnlyCollection<TimelineAnchorEstimate> estimates,
        AnchorRejection? rejection,
        string explanation)
    {
        this.OffsetSeconds = offsetSeconds;
        this.UncertaintySeconds = uncertaintySeconds;
        this.Estimates = estimates;
        this.Rejection = rejection;
        this.Explanation = explanation;
    }

    /// <summary>Add this to a presentation timestamp to get a stimulus time. Meaningless unless <see cref="IsEstablished"/>.</summary>
    public double OffsetSeconds { get; }

    /// <summary>Half-width of the interval the offset lies in, after reconciliation.</summary>
    public double UncertaintySeconds { get; }

    /// <summary>The estimates this was reconciled from, in the order they were supplied.</summary>
    public ReadOnlyCollection<TimelineAnchorEstimate> Estimates { get; }

    /// <summary>Why no timebase was established, or null when one was.</summary>
    public AnchorRejection? Rejection { get; }

    /// <summary>What was decided and on what evidence, phrased for a verdict line.</summary>
    public string Explanation { get; }

    /// <summary>Whether a usable timebase was established.</summary>
    public bool IsEstablished => this.Rejection is null;

    /// <summary>
    /// Reconciles independent timing evidence into one timebase.
    /// </summary>
    /// <param name="estimates">
    /// At least one point estimate, and at least two distinct sources. Two readings of
    /// the same source corroborate nothing: they share whatever is wrong with that
    /// source. Bounds may accompany the readings and may refute them, but never move
    /// them.
    /// </param>
    public static TimelineAnchor Reconcile(IEnumerable<TimelineAnchorEstimate> estimates)
    {
        ArgumentNullException.ThrowIfNull(estimates);
        var supplied = new ReadOnlyCollection<TimelineAnchorEstimate>([.. estimates]);
        var readings = supplied.Where(e => e.Kind == TimelineEvidenceKind.PointEstimate).ToList();
        var bounds = supplied.Where(e => e.Kind != TimelineEvidenceKind.PointEstimate).ToList();

        var distinctSources = supplied.Select(estimate => estimate.Source).Distinct().Count();
        if (readings.Count == 0 || distinctSources < 2)
        {
            var names = supplied.Count == 0
                ? "none"
                : string.Join(", ", supplied.Select(estimate => $"{estimate.Source} ({estimate.Kind})"));
            var missing = readings.Count == 0
                ? "the timebase needs a reading of the offset; bounds alone say only where it is not"
                : "the timebase needs two independent anchors";
            return new TimelineAnchor(0, 0, supplied, AnchorRejection.NotCorroborated, $"{missing}; got {names}");
        }

        foreach (var left in readings)
        {
            foreach (var right in readings)
            {
                if (left.Source == right.Source)
                {
                    continue;
                }

                var separation = Math.Abs(left.OffsetSeconds - right.OffsetSeconds);
                var allowed = left.UncertaintySeconds + right.UncertaintySeconds;
                if (separation > allowed)
                {
                    return new TimelineAnchor(
                        0,
                        0,
                        supplied,
                        AnchorRejection.Contradicted,
                        $"{left.Source} says {Seconds(left.OffsetSeconds)} s +/- {Seconds(left.UncertaintySeconds)} and " +
                        $"{right.Source} says {Seconds(right.OffsetSeconds)} s +/- {Seconds(right.UncertaintySeconds)}; " +
                        $"they differ by {Seconds(separation)} s, more than the {Seconds(allowed)} s their uncertainties allow");
                }
            }
        }

        // Inverse-variance weighting over the READINGS only, with the uncertainty floored:
        // an estimate claiming zero uncertainty would take the whole weight and silence
        // every other anchor, which is the opposite of corroboration. Bounds are excluded
        // by construction -- a bound that pulled the offset towards itself would import
        // the very latency it exists to acknowledge.
        double weightSum = 0;
        double weighted = 0;
        foreach (var estimate in readings)
        {
            var sigma = Math.Max(estimate.UncertaintySeconds, 1e-6);
            var weight = 1.0 / (sigma * sigma);
            weightSum += weight;
            weighted += weight * estimate.OffsetSeconds;
        }

        var offset = weighted / weightSum;
        var uncertainty = Math.Sqrt(1.0 / weightSum);

        // A bound refutes a reading only when the reading's whole interval sits on the
        // wrong side of it. Anything less is agreement: the gap between them is the
        // latency the bound was never able to measure.
        foreach (var bound in bounds)
        {
            var limit = bound.Kind == TimelineEvidenceKind.LowerBound
                ? bound.OffsetSeconds - bound.UncertaintySeconds
                : bound.OffsetSeconds + bound.UncertaintySeconds;
            var refuted = bound.Kind == TimelineEvidenceKind.LowerBound
                ? offset + uncertainty < limit
                : offset - uncertainty > limit;
            if (refuted)
            {
                var side = bound.Kind == TimelineEvidenceKind.LowerBound ? "below" : "above";
                return new TimelineAnchor(
                    0,
                    0,
                    supplied,
                    AnchorRejection.Contradicted,
                    $"the readings put the offset at {Seconds(offset)} s +/- {Seconds(uncertainty)}, which is entirely {side} " +
                    $"the {Seconds(limit)} s {bound.Source} allows ({bound.Evidence})");
            }
        }

        var best = readings.OrderBy(estimate => estimate.UncertaintySeconds).First();
        var corroboration = bounds.Count == 0
            ? string.Empty
            : $", consistent with {bounds.Count.ToString(CultureInfo.InvariantCulture)} causal bound(s)";

        return new TimelineAnchor(
            offset,
            uncertainty,
            supplied,
            null,
            $"{readings.Count.ToString(CultureInfo.InvariantCulture)} reading(s) give {Seconds(offset)} s +/- {Seconds(uncertainty)} " +
            $"(tightest: {best.Source}, {best.Evidence}){corroboration}");
    }

    /// <summary>
    /// The offset implied by two wall-clock readings, with the recorder's start-up latency
    /// as its uncertainty.
    /// </summary>
    /// <param name="stimulusEpochUtc">Wall-clock instant the stimulus calls its own t = 0.</param>
    /// <param name="recorderStartUtc">Wall-clock instant the recorder was asked to start.</param>
    /// <param name="startupLatencySeconds">
    /// Best estimate of the delay between that request and the first captured frame. This
    /// IS the uncertainty of the anchor -- the wall clock cannot see the first frame.
    /// </param>
    public static TimelineAnchorEstimate FromWallClock(
        DateTimeOffset stimulusEpochUtc,
        DateTimeOffset recorderStartUtc,
        double startupLatencySeconds)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(startupLatencySeconds);

        // The first frame carries pts 0, so stimulus_time(pts) = pts + (first frame's
        // wall clock - stimulus epoch), and the first frame is the recorder's start plus
        // a latency nobody measured directly.
        var offset = (recorderStartUtc - stimulusEpochUtc).TotalSeconds + (startupLatencySeconds / 2);
        return new TimelineAnchorEstimate(
            AnchorSource.WallClock,
            offset,
            Math.Max(startupLatencySeconds / 2, 1e-3),
            $"recorder started {Seconds((recorderStartUtc - stimulusEpochUtc).TotalSeconds)} s after the stimulus epoch, " +
            $"first frame within {Seconds(startupLatencySeconds)} s of that");
    }

    /// <summary>
    /// The lower bound a marker places on the offset: the stimulus drew it at a known
    /// time, and the frame carrying it cannot precede that paint.
    /// </summary>
    /// <remarks>
    /// A bound and not a reading. With the offset defined as stimulus time minus
    /// presentation timestamp, a marker painted at S and first visible in the frame at P
    /// gives `offset = S - P + L`, where L is the paint-to-capture latency: non-negative,
    /// unmeasured, and unbounded above. So `S - P` is the smallest the offset can be, and
    /// a marker that appeared EARLIER than a counter reading allows still refutes it --
    /// which is the fabrication this evidence exists to catch. What it must not do is
    /// drag the offset down by L, which is exactly what averaging it with a counter
    /// reading did.
    /// </remarks>
    /// <param name="markerStimulusTimeSeconds">Stimulus time the marker was drawn at.</param>
    /// <param name="markerFramePts">Presentation timestamp of the frame it was found in.</param>
    /// <param name="frameIntervalSeconds">
    /// Nominal frame interval. Half of it is the uncertainty: the marker was drawn at some
    /// point inside the frame it first appears in.
    /// </param>
    public static TimelineAnchorEstimate FromInBandMarker(
        double markerStimulusTimeSeconds,
        double markerFramePts,
        double frameIntervalSeconds)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(frameIntervalSeconds);

        return new TimelineAnchorEstimate(
            AnchorSource.InBandMarker,
            markerStimulusTimeSeconds - markerFramePts,
            frameIntervalSeconds / 2,
            $"marker declared at stimulus t={Seconds(markerStimulusTimeSeconds)} s found at {Seconds(markerFramePts)} s",
            TimelineEvidenceKind.LowerBound);
    }

    /// <summary>
    /// The offset implied by performance-counter readings taken on both sides of the run.
    /// </summary>
    /// <param name="stimulusEpochTicks">Counter value the stimulus calls its own t = 0.</param>
    /// <param name="firstFrameTicks">Counter value recorded when the first frame was captured.</param>
    /// <param name="ticksPerSecond">Counter frequency, from QueryPerformanceFrequency.</param>
    /// <param name="readingUncertaintySeconds">
    /// How far either reading may sit from the event it labels. Pass the capture path's
    /// own timestamping granularity; zero is never correct and is refused.
    /// </param>
    public static TimelineAnchorEstimate FromPerformanceCounter(
        long stimulusEpochTicks,
        long firstFrameTicks,
        long ticksPerSecond,
        double readingUncertaintySeconds)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(ticksPerSecond);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(readingUncertaintySeconds);

        var offset = (double)(firstFrameTicks - stimulusEpochTicks) / ticksPerSecond;
        return new TimelineAnchorEstimate(
            AnchorSource.PerformanceCounter,
            offset,
            readingUncertaintySeconds,
            $"first frame counted {(firstFrameTicks - stimulusEpochTicks).ToString(CultureInfo.InvariantCulture)} ticks after the stimulus epoch " +
            $"at {ticksPerSecond.ToString(CultureInfo.InvariantCulture)} Hz");
    }
}
