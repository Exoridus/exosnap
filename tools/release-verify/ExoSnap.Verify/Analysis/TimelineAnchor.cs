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

/// <summary>One estimate of how far the recording's timebase sits from the stimulus's.</summary>
/// <param name="Source">Where the estimate came from.</param>
/// <param name="OffsetSeconds">Add this to a presentation timestamp to get a stimulus time.</param>
/// <param name="UncertaintySeconds">
/// Half-width of the interval the true offset lies in. Never zero: an estimate presented
/// as exact cannot be reconciled with another, because any disagreement at all then reads
/// as a contradiction.
/// </param>
/// <param name="Evidence">What was measured, phrased for a verdict line.</param>
public sealed record TimelineAnchorEstimate(
    AnchorSource Source,
    double OffsetSeconds,
    double UncertaintySeconds,
    string Evidence);

/// <summary>Why a reconciliation of several anchors did not produce a usable timebase.</summary>
public enum AnchorRejection
{
    /// <summary>Fewer than two independent estimates were supplied.</summary>
    NotCorroborated,

    /// <summary>Two estimates disagree by more than their uncertainties allow.</summary>
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
    /// Reconciles independent offset estimates into one timebase.
    /// </summary>
    /// <param name="estimates">
    /// At least two estimates from different sources. Two readings of the same source
    /// corroborate nothing: they share whatever is wrong with that source.
    /// </param>
    public static TimelineAnchor Reconcile(IEnumerable<TimelineAnchorEstimate> estimates)
    {
        ArgumentNullException.ThrowIfNull(estimates);
        var supplied = new ReadOnlyCollection<TimelineAnchorEstimate>([.. estimates]);

        var distinctSources = supplied.Select(estimate => estimate.Source).Distinct().Count();
        if (distinctSources < 2)
        {
            var names = supplied.Count == 0
                ? "none"
                : string.Join(", ", supplied.Select(estimate => estimate.Source));
            return new TimelineAnchor(
                0,
                0,
                supplied,
                AnchorRejection.NotCorroborated,
                $"the timebase needs two independent anchors; got {names}");
        }

        foreach (var left in supplied)
        {
            foreach (var right in supplied)
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

        // Inverse-variance weighting, with the uncertainty floored: an estimate claiming
        // zero uncertainty would take the whole weight and silence every other anchor,
        // which is the opposite of corroboration.
        double weightSum = 0;
        double weighted = 0;
        foreach (var estimate in supplied)
        {
            var sigma = Math.Max(estimate.UncertaintySeconds, 1e-6);
            var weight = 1.0 / (sigma * sigma);
            weightSum += weight;
            weighted += weight * estimate.OffsetSeconds;
        }

        var offset = weighted / weightSum;
        var uncertainty = Math.Sqrt(1.0 / weightSum);
        var best = supplied.OrderBy(estimate => estimate.UncertaintySeconds).First();

        return new TimelineAnchor(
            offset,
            uncertainty,
            supplied,
            null,
            $"{supplied.Count.ToString(CultureInfo.InvariantCulture)} anchors agree on {Seconds(offset)} s +/- {Seconds(uncertainty)} " +
            $"(tightest: {best.Source}, {best.Evidence})");
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
    /// The offset implied by a marker the stimulus drew at a known stimulus time and the
    /// frame it was found in.
    /// </summary>
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
            $"marker declared at stimulus t={Seconds(markerStimulusTimeSeconds)} s found at {Seconds(markerFramePts)} s");
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
