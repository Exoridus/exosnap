using ExoSnap.Verify.Analysis;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Where the stimulus-to-recording offset is allowed to come from.
/// </summary>
/// <remarks>
/// The private cursor analyzer derived it from the first frame in which a cursor
/// sprite appeared, and then measured the sprite against that timeline. A recording
/// in which the cursor never followed the pointer pins the offset onto whatever else
/// changed first, and the run reports a large offset and a plausible pass -- which is
/// how a -13.5 s offset was once read as a measurement. Every case here is about the
/// offset resting on something other than the thing under test.
/// </remarks>
public sealed class TimelineAnchorTests
{
    private static TimelineAnchorEstimate Estimate(AnchorSource source, double offset, double uncertainty) =>
        new(source, offset, uncertainty, "fixture");

    [Fact]
    public void OneAnchorEstablishesNothing()
    {
        var anchor = TimelineAnchor.Reconcile([Estimate(AnchorSource.WallClock, 6.0, 0.5)]);

        Assert.False(anchor.IsEstablished);
        Assert.Equal(AnchorRejection.NotCorroborated, anchor.Rejection);
        Assert.Contains("WallClock", anchor.Explanation, StringComparison.Ordinal);
    }

    [Fact]
    public void TwoReadingsOfTheSameSourceCorroborateNothing()
    {
        // They share whatever is wrong with that source, so agreeing says only that
        // the source is consistent -- not that it is right.
        var anchor = TimelineAnchor.Reconcile(
        [
            Estimate(AnchorSource.WallClock, 6.0, 0.5),
            Estimate(AnchorSource.WallClock, 6.01, 0.5),
        ]);

        Assert.False(anchor.IsEstablished);
        Assert.Equal(AnchorRejection.NotCorroborated, anchor.Rejection);
    }

    [Fact]
    public void TwoAgreeingSourcesEstablishATighterOffsetThanEither()
    {
        var anchor = TimelineAnchor.Reconcile(
        [
            Estimate(AnchorSource.WallClock, 6.00, 0.500),
            Estimate(AnchorSource.InBandMarker, 6.02, 0.008),
        ]);

        Assert.True(anchor.IsEstablished);
        // The tight anchor dominates, as inverse-variance weighting requires.
        Assert.Equal(6.02, anchor.OffsetSeconds, 3);
        Assert.True(anchor.UncertaintySeconds < 0.008);
    }

    [Fact]
    public void SourcesThatDisagreeBeyondTheirUncertaintiesEstablishNothing()
    {
        var anchor = TimelineAnchor.Reconcile(
        [
            Estimate(AnchorSource.WallClock, 6.0, 0.05),
            Estimate(AnchorSource.InBandMarker, 19.5, 0.008),
        ]);

        Assert.False(anchor.IsEstablished);
        Assert.Equal(AnchorRejection.Contradicted, anchor.Rejection);
        Assert.Contains("13.500", anchor.Explanation, StringComparison.Ordinal);
    }

    [Fact]
    public void AnEstimateClaimingNoUncertaintyDoesNotSilenceTheOthers()
    {
        // Weighting an exact estimate by 1/0 would take the whole weight, which is
        // the opposite of corroboration -- and the "exact" one is the one most
        // likely to be wrong about being exact.
        var anchor = TimelineAnchor.Reconcile(
        [
            Estimate(AnchorSource.PerformanceCounter, 6.000, 0.0),
            Estimate(AnchorSource.InBandMarker, 6.004, 0.008),
        ]);

        Assert.True(anchor.IsEstablished);
        Assert.True(anchor.UncertaintySeconds > 0);
    }

    [Fact]
    public void TheWallClockAnchorCarriesTheStartupLatencyAsItsUncertainty()
    {
        // The wall clock cannot see the first frame; it sees the request to start.
        var stimulus = DateTimeOffset.Parse("2026-09-13T10:00:00Z", System.Globalization.CultureInfo.InvariantCulture);
        var recorder = stimulus.AddSeconds(6.0);

        var estimate = TimelineAnchor.FromWallClock(stimulus, recorder, startupLatencySeconds: 0.5);

        Assert.Equal(AnchorSource.WallClock, estimate.Source);
        Assert.Equal(6.25, estimate.OffsetSeconds, 6);
        Assert.Equal(0.25, estimate.UncertaintySeconds, 6);
    }

    [Fact]
    public void TheMarkerAnchorIsGoodToHalfAFrame()
    {
        var estimate = TimelineAnchor.FromInBandMarker(
            markerStimulusTimeSeconds: 6.0, markerFramePts: 0.5, frameIntervalSeconds: 1.0 / 60);

        Assert.Equal(5.5, estimate.OffsetSeconds, 6);
        Assert.Equal(1.0 / 120, estimate.UncertaintySeconds, 6);
    }

    [Fact]
    public void ThePerformanceCounterAnchorRefusesAZeroReadingUncertainty()
    {
        // There is no such thing as a timestamp that labels its event exactly, and
        // an anchor that claims one cannot be reconciled with anything.
        Assert.Throws<ArgumentOutOfRangeException>(
            () => TimelineAnchor.FromPerformanceCounter(0, 10_000_000, 10_000_000, 0.0));
    }

    [Fact]
    public void ThePerformanceCounterAnchorConvertsTicksAtTheStatedFrequency()
    {
        var estimate = TimelineAnchor.FromPerformanceCounter(
            stimulusEpochTicks: 1_000_000,
            firstFrameTicks: 64_000_000,
            ticksPerSecond: 10_000_000,
            readingUncertaintySeconds: 0.001);

        Assert.Equal(6.3, estimate.OffsetSeconds, 6);
    }

    [Fact]
    public void NoEstimatesAtAllIsNotAnExceptionButARefusal()
    {
        var anchor = TimelineAnchor.Reconcile([]);

        Assert.False(anchor.IsEstablished);
        Assert.Contains("none", anchor.Explanation, StringComparison.Ordinal);
    }
}
