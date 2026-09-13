using ExoSnap.Verify.Analysis;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The timebase a cursor analysis is entitled to, and what it must refuse.
/// </summary>
/// <remarks>
/// The analyzer this replaces derived its offset by assuming the first frame a cursor
/// sprite appeared in was the first scheduled pointer move -- which is the behaviour
/// under test. A cursor that appeared late simply moved the whole timeline, and the
/// verdict came out green. These cases exist so that cannot come back: every estimate
/// here comes from something the cursor cannot influence, and a late or absent cursor
/// must leave the timebase exactly where it was.
/// </remarks>
public sealed class CursorTimelineTests
{
    private const long Frequency = 10_000_000;
    private const double FrameInterval = 1.0 / 30.0;

    /// <summary>A run whose recording opened 0.5 s after the stimulus called t = 0.</summary>
    private static CursorTimelineInput Healthy(string engineLog, double offsetSeconds = 0.5) => new(
        Markers:
        [
            new MarkerObservation(4.0, 4.0 - offsetSeconds),
            new MarkerObservation(31.0, 31.0 - offsetSeconds),
        ],
        FrameIntervalSeconds: FrameInterval,
        EngineLogPath: engineLog,
        StimulusEpochTicks: 0,
        QpcFrequencyHz: Frequency);

    /// <summary>An engine log whose video epoch sits a given offset after the stimulus epoch.</summary>
    private static string WriteEngineLog(double offsetSeconds, string source = "frame_timestamp")
    {
        var path = Path.Combine(Path.GetTempPath(), "exosnap-engine-" + Guid.NewGuid().ToString("N") + ".jsonl");
        var epoch = (long)Math.Round(offsetSeconds * VideoEpochRecord.TicksPerSecond);

        // The engine's own shape: a fields object, and every field written as a string
        // including the numeric ones.
        var record = "{\"message\":\"video epoch established\",\"fields\":{"
            + "\"video_epoch_qpc_100ns\":\"" + epoch.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\","
            + "\"qpc_frequency_hz\":\"" + Frequency.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\","
            + "\"epoch_source\":\"" + source + "\"}}";
        File.WriteAllText(path, record + Environment.NewLine);
        return path;
    }

    [Fact]
    public void AHealthyRunQualifiesFromTwoIndependentSources()
    {
        var log = WriteEngineLog(0.5);
        try
        {
            var result = CursorTimeline.Qualify(Healthy(log));

            Assert.True(result.Qualified, result.Anchor.Explanation);
            Assert.Equal(0.5, result.Anchor.OffsetSeconds, 3);
            Assert.Contains(result.Anchor.Estimates, estimate => estimate.Source == AnchorSource.InBandMarker);
            Assert.Contains(result.Anchor.Estimates, estimate => estimate.Source == AnchorSource.PerformanceCounter);
            Assert.True(result.Rate?.Agrees);
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void ALateCursorDoesNotMoveTheTimebase()
    {
        // Test A, stated against what the circular anchor would have said. A run whose
        // recording opened 0.5 s after the stimulus, and whose cursor then appears
        // 0.5 s later than it should.
        const double TrueOffset = 0.5;
        const double CursorLateness = 0.5;
        const double FirstMoveStimulusTime = 6.0;

        var log = WriteEngineLog(TrueOffset);
        try
        {
            var result = CursorTimeline.Qualify(Healthy(log, TrueOffset));

            // What the replaced analyzer computed: the first frame a sprite appeared in
            // was declared to be the first scheduled move.
            //     offset = firstMove - pts(firstSpriteFrame)
            // With the cursor late, that sprite frame sits at
            //     pts = (firstMove - trueOffset) + lateness
            var circularOffset = FirstMoveStimulusTime
                - (FirstMoveStimulusTime - TrueOffset + CursorLateness);

            Assert.True(result.Qualified, result.Anchor.Explanation);
            Assert.Equal(TrueOffset, result.Anchor.OffsetSeconds, 3);

            // The circular method absorbs the lateness into the timebase and reports an
            // offset that is wrong by exactly the defect it was supposed to reveal.
            Assert.Equal(TrueOffset - CursorLateness, circularOffset, 6);
            Assert.True(
                Math.Abs(result.Anchor.OffsetSeconds - circularOffset) > result.Anchor.UncertaintySeconds,
                "the qualified timebase has to disagree with the circular one, or the defect stays invisible");

            // On the qualified timebase the late sprite lands 0.5 s after the move it
            // belongs to, which is what a cursor verdict is then entitled to report.
            var spritePts = FirstMoveStimulusTime - TrueOffset + CursorLateness;
            var spriteStimulusTime = spritePts + result.Anchor.OffsetSeconds;
            Assert.Equal(FirstMoveStimulusTime + CursorLateness, spriteStimulusTime, 3);
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void AMissingCursorStillQualifiesTheTimebase()
    {
        // Test B. The cleanest independence check: no cursor at all, and the timebase
        // is still established. A cursor verdict computed on it then has something to
        // be wrong against.
        var log = WriteEngineLog(0.5);
        try
        {
            var result = CursorTimeline.Qualify(Healthy(log));

            Assert.True(result.Qualified);
            Assert.Equal(0.5, result.Anchor.OffsetSeconds, 3);
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void MarkersThatDisagreeAboutTheRateDisqualifyTheTimebase()
    {
        // Test C. The offsets can agree while the clocks run at different speeds: the
        // first marker fixes the offset and the second lands a second early. One
        // offset cannot describe a run whose timebases drift apart.
        var log = WriteEngineLog(0.5);
        try
        {
            var drifted = Healthy(log) with
            {
                Markers =
                [
                    new MarkerObservation(4.0, 3.5),
                    new MarkerObservation(31.0, 29.5),
                ],
            };

            var result = CursorTimeline.Qualify(drifted);

            Assert.False(result.Qualified);
            Assert.False(result.Rate!.Agrees);
            Assert.Contains("apart in the recording", result.Rate.Evidence, StringComparison.Ordinal);
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void OneSourceAloneIsNotCorroboration()
    {
        // Markers without an engine log: one source, however many readings of it.
        var result = CursorTimeline.Qualify(Healthy(engineLog: null!) with { EngineLogPath = null });

        Assert.False(result.Qualified);
        Assert.Equal(AnchorRejection.NotCorroborated, result.Anchor.Rejection);
        Assert.Contains(result.Notes, note => note.Contains("no engine log", StringComparison.Ordinal));
    }

    [Fact]
    public void AFlooredEpochIsABoundAndCorroboratesNothing()
    {
        // SessionStartFloor says the timeline cannot have opened before some instant,
        // not that it opened then. Treating it as a reading would manufacture exactly
        // the second source this reconciliation exists to require.
        var log = WriteEngineLog(0.5, "session_start_floor");
        try
        {
            var result = CursorTimeline.Qualify(Healthy(log));

            Assert.False(result.Qualified);
            Assert.Equal(AnchorRejection.NotCorroborated, result.Anchor.Rejection);
            Assert.Contains(
                result.Notes,
                note => note.Contains("bound rather than a reading", StringComparison.Ordinal));
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void SourcesThatContradictEachOtherAreRefused()
    {
        // The markers say the recording opened 0.5 s in; the recorder says 2 s. One of
        // them is wrong and nothing here can say which, so no timebase is published.
        var log = WriteEngineLog(2.0);
        try
        {
            var result = CursorTimeline.Qualify(Healthy(log));

            Assert.False(result.Qualified);
            Assert.Equal(AnchorRejection.Contradicted, result.Anchor.Rejection);
        }
        finally
        {
            File.Delete(log);
        }
    }

    [Fact]
    public void TheOldCircularAnchorCannotBeExpressedHere()
    {
        // Test D, as far as it can be written: the circular anchor derived its offset
        // from a cursor observation, and this input has nowhere to put one. The
        // measurement is structurally unable to consult the behaviour under test.
        var properties = typeof(CursorTimelineInput).GetProperties().Select(property => property.Name).ToList();

        Assert.DoesNotContain(properties, name => name.Contains("Cursor", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(properties, name => name.Contains("Sprite", StringComparison.OrdinalIgnoreCase));
        Assert.Contains(properties, name => name == "Markers");
        Assert.Contains(properties, name => name == "EngineLogPath");
    }
}
