using ExoSnap.Verify.Analysis;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What a decode reports about itself, and what the analysis is allowed to
/// conclude from it.
/// </summary>
/// <remarks>
/// The private cursor analyzer this replaces threw on a frame-count mismatch. That
/// turns the interesting finding -- the decode dropped frames -- into a stack trace
/// and reports nothing at all about the thing under test. Every case here is a
/// departure that used to abort the run or go unnoticed.
/// </remarks>
public sealed class FrameSequenceTests
{
    private const long FrameBytes = 64;

    private static string[] Log(params double[] times) =>
        [.. times.Select(value => $"[Parsed_showinfo_0 @ 0] n:0 pts_time:{value.ToString("F6", System.Globalization.CultureInfo.InvariantCulture)} fmt:gray")];

    [Fact]
    public void ACleanDecodeReportsNothing()
    {
        var sequence = FrameSequence.Read(FrameBytes * 4, FrameBytes, Log(0.0, 0.016, 0.032, 0.048));

        Assert.Empty(sequence.Findings);
        Assert.Equal(4, sequence.RawFrameCount);
        Assert.Equal(4, sequence.UsableFrameCount);
        Assert.True(sequence.IsTimebaseTrustworthy);
    }

    [Fact]
    public void BackwardsTimestampsAreReportedAndDisqualifyTheTimebase()
    {
        var sequence = FrameSequence.Read(FrameBytes * 3, FrameBytes, Log(0.0, 0.032, 0.016));

        var finding = Assert.Single(sequence.Findings);
        Assert.Equal(FrameSequenceDefect.Backwards, finding.Defect);
        Assert.Equal(2, finding.FrameIndex);
        Assert.False(sequence.IsTimebaseTrustworthy);
    }

    [Fact]
    public void ADuplicateTimestampIsReportedButLeavesTheTimebaseUsable()
    {
        // A duplicate breaks "which frame was showing at time t" for one frame; it
        // does not break a measurement that reads each frame's own timestamp.
        var sequence = FrameSequence.Read(FrameBytes * 3, FrameBytes, Log(0.0, 0.016, 0.016));

        var finding = Assert.Single(sequence.Findings);
        Assert.Equal(FrameSequenceDefect.Duplicate, finding.Defect);
        Assert.True(sequence.IsTimebaseTrustworthy);
    }

    [Fact]
    public void AGapNamesHowManyFramesAreMissing()
    {
        var sequence = FrameSequence.Read(
            FrameBytes * 4, FrameBytes, Log(0.0, 0.016, 0.080, 0.096), nominalFrameIntervalSeconds: 0.016);

        var finding = Assert.Single(sequence.Findings);
        Assert.Equal(FrameSequenceDefect.Gap, finding.Defect);
        Assert.Contains("3 frame(s) missing", finding.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void TheNominalIntervalIsTakenFromTheMedianWhenItIsNotGiven()
    {
        // A variable-frame-rate capture has no declared interval. One huge gap
        // drags a MEAN far enough that a smaller gap beside it falls inside the
        // tolerance and is never reported -- the failure a median estimate exists
        // to prevent, and the reason this fixture carries two gaps of very
        // different size rather than one.
        var sequence = FrameSequence.Read(
            FrameBytes * 9,
            FrameBytes,
            Log(0.0, 0.016, 0.032, 0.048, 0.064, 0.080, 0.130, 3.130, 3.146));

        Assert.Equal(2, sequence.Findings.Count(item => item.Defect == FrameSequenceDefect.Gap));
    }

    [Fact]
    public void AFrameCountMismatchIsAFindingRatherThanAnException()
    {
        // The failure the analyzer this replaces aborted on.
        var sequence = FrameSequence.Read(FrameBytes * 5, FrameBytes, Log(0.0, 0.016, 0.032));

        var finding = Assert.Single(sequence.Findings, item => item.Defect == FrameSequenceDefect.CountMismatch);
        Assert.Contains("5 raw frame(s) against 3", finding.Detail, StringComparison.Ordinal);
        // Three frames are still measurable, and saying so is more useful than
        // refusing to say anything.
        Assert.Equal(3, sequence.UsableFrameCount);
    }

    [Fact]
    public void ATruncatedFinalFrameIsReportedAndDisqualifiesTheTimebase()
    {
        var sequence = FrameSequence.Read((FrameBytes * 3) + 17, FrameBytes, Log(0.0, 0.016, 0.032));

        Assert.Contains(sequence.Findings, item => item.Defect == FrameSequenceDefect.TruncatedFrame);
        Assert.False(sequence.IsTimebaseTrustworthy);
    }

    [Fact]
    public void DecoderErrorLinesAreCountedSeparatelyFromTimestamps()
    {
        string[] log =
        [
            "[Parsed_showinfo_0 @ 0] n:0 pts_time:0.000000",
            "[h264 @ 0] Invalid data found when processing input",
            "[Parsed_showinfo_0 @ 0] n:1 pts_time:0.016000",
            "[h264 @ 0] corrupt decoded frame",
        ];

        var sequence = FrameSequence.Read(FrameBytes * 2, FrameBytes, log);

        var finding = Assert.Single(sequence.Findings, item => item.Defect == FrameSequenceDefect.DecoderError);
        Assert.Contains("2 error line(s)", finding.Detail, StringComparison.Ordinal);
        Assert.Equal(2, sequence.PresentationTimes.Count);
    }

    [Fact]
    public void AnErrorWordInsideAFilterNameIsNotADecoderError()
    {
        // "showinfo" and friends put the rule's own words in ordinary lines; a
        // guard that fires on those reports its own log format as corruption.
        string[] log =
        [
            "[Parsed_showinfo_0 @ 0] n:0 pts_time:0.000000 checksum:ERRORFREE",
            "Stream mapping: error_resilience off",
        ];

        var sequence = FrameSequence.Read(FrameBytes, FrameBytes, log);

        Assert.DoesNotContain(sequence.Findings, item => item.Defect == FrameSequenceDefect.DecoderError);
    }

    [Fact]
    public void ScientificNotationTimestampsParse()
    {
        var sequence = FrameSequence.Read(FrameBytes * 2, FrameBytes, ["pts_time:1.6e-2", "pts_time:3.2e-2"]);

        Assert.Equal(2, sequence.PresentationTimes.Count);
        Assert.Equal(0.016, sequence.PresentationTimes[0], 6);
    }

    [Fact]
    public void AGapToleranceAtOrBelowOneIsRefused()
    {
        // Every interval is a gap at a tolerance of 1, so the parameter would make
        // the finding meaningless rather than stricter.
        Assert.Throws<ArgumentOutOfRangeException>(
            () => FrameSequence.Read(FrameBytes, FrameBytes, Log(0.0), gapTolerance: 1.0));
    }
}
