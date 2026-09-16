using ExoSnap.Verify.Analysis;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Reading the recorder's own timeline out of its log.
/// </summary>
/// <remarks>
/// The epoch record exists so that an offset never has to be inferred from the picture.
/// It only helps if what the reader takes from it carries the precision the recorder
/// actually had: the three epoch sources are not interchangeable, and the file holds
/// more than one session.
/// </remarks>
public sealed class EngineLogTests
{
    private static string EpochLine(string epoch, string frequency, string source, string? session = null)
    {
        var fields = "\"video_epoch_qpc_100ns\":\"" + epoch + "\"," +
                     "\"qpc_frequency_hz\":\"" + frequency + "\"," +
                     "\"epoch_source\":\"" + source + "\"";
        if (session is not null)
        {
            fields += ",\"session\":\"" + session + "\"";
        }

        return "{\"timestamp_unix_ms\":1,\"level\":\"info\",\"component\":\"video_thread\"," +
               "\"message\":\"video epoch established\",\"fields\":{" + fields + "}}";
    }

    private static string WriteLog(TemporaryDirectory directory, params string[] lines)
    {
        var path = Path.Combine(directory.Path, "engine.jsonl");
        File.WriteAllLines(path, lines);
        return path;
    }

    [Fact]
    public void TheEpochItsUnitAndItsSourceAreRead()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-engine-log");
        var path = WriteLog(
            directory,
            "{\"message\":\"capture session starting\",\"fields\":{}}",
            EpochLine("133700000000000", "10000000", "frame_timestamp", "launch-1"));

        var read = EngineLog.ReadVideoEpochs(path);

        var epoch = Assert.Single(read.VideoEpochs);
        Assert.Equal(133700000000000L, epoch.EpochQpc100ns);
        Assert.Equal(10000000L, epoch.QpcFrequencyHz);
        Assert.Equal(VideoEpochSource.FrameTimestamp, epoch.Source);
        Assert.Equal("launch-1", epoch.SessionId);
        Assert.Empty(read.Rejections);
    }

    [Fact]
    public void EverySessionInTheFileIsReturned()
    {
        // The engine appends across launches. Handing back only the last record would
        // let a caller measure this run's frames against a previous run's timeline
        // without anything in the result saying so.
        using var directory = FixtureTool.NewTemporaryDirectory("-engine-log");
        var path = WriteLog(
            directory,
            EpochLine("100", "10000000", "capture_observed", "launch-1"),
            EpochLine("900", "10000000", "frame_timestamp", "launch-2"));

        var read = EngineLog.ReadVideoEpochs(path);

        Assert.Equal(2, read.VideoEpochs.Count);
        Assert.Equal(["launch-1", "launch-2"], read.VideoEpochs.Select(epoch => epoch.SessionId));
    }

    [Fact]
    public void APartialTrailingLineIsCountedAndNotAnError()
    {
        // A log read while the recorder is still running ends mid-write.
        using var directory = FixtureTool.NewTemporaryDirectory("-engine-log");
        var path = WriteLog(
            directory,
            EpochLine("500", "10000000", "frame_timestamp"),
            "{\"message\":\"video epoch estab");

        var read = EngineLog.ReadVideoEpochs(path);

        Assert.Single(read.VideoEpochs);
        Assert.Equal(1, read.UnreadableLines);
        Assert.Empty(read.Rejections);
    }

    [Fact]
    public void AnUnknownEpochSourceIsRejectedRatherThanAssumedPrecise()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-engine-log");
        var path = WriteLog(directory, EpochLine("500", "10000000", "presentation_time"));

        var read = EngineLog.ReadVideoEpochs(path);

        Assert.Empty(read.VideoEpochs);
        Assert.Contains("presentation_time", Assert.Single(read.Rejections), StringComparison.Ordinal);
    }

    [Fact]
    public void AMissingFileIsAnEmptyReadNotAThrow()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-engine-log");

        var read = EngineLog.ReadVideoEpochs(Path.Combine(directory.Path, "absent.jsonl"));

        Assert.Empty(read.VideoEpochs);
        Assert.Empty(read.Rejections);
        Assert.Equal(0, read.UnreadableLines);
    }

    [Fact]
    public void AFrameTimestampEpochIsWorthMoreThanAnObservedOne()
    {
        var stamped = new VideoEpochRecord(60_000_000, 10_000_000, VideoEpochSource.FrameTimestamp, null);
        var observed = new VideoEpochRecord(60_000_000, 10_000_000, VideoEpochSource.CaptureObserved, null);

        var stampedEstimate = stamped.ToAnchorEstimate(0, 1.0 / 60.0);
        var observedEstimate = observed.ToAnchorEstimate(0, 1.0 / 60.0);

        Assert.NotNull(stampedEstimate);
        Assert.NotNull(observedEstimate);
        Assert.True(stampedEstimate.UncertaintySeconds < observedEstimate.UncertaintySeconds);
        Assert.Equal(1.0 / 120.0, observedEstimate.UncertaintySeconds, 6);
    }

    [Fact]
    public void AnObservedEpochIsCorrectedForTheLatencyItCarries()
    {
        // The observation is later than the present by up to one acquire interval, so
        // taking it verbatim biases every offset late by half of one. At 60 fps that is
        // 8.3 ms -- the size of an in-band marker's entire uncertainty.
        var observed = new VideoEpochRecord(60_000_000, 10_000_000, VideoEpochSource.CaptureObserved, null);

        var estimate = observed.ToAnchorEstimate(0, 1.0 / 60.0);

        Assert.NotNull(estimate);
        Assert.Equal(6.0 - (1.0 / 120.0), estimate.OffsetSeconds, 6);
    }

    [Fact]
    public void AFlooredEpochCarriesNoEstimateAtAll()
    {
        // The session-start floor is a bound below the first frame, not the instant of
        // one. An offset measured against it is not a reading of anything.
        var floored = new VideoEpochRecord(60_000_000, 10_000_000, VideoEpochSource.SessionStartFloor, null);

        Assert.Null(floored.ToAnchorEstimate(0, 1.0 / 60.0));
    }

    [Fact]
    public void TheEngineEpochCorroboratesAWallClockAnchor()
    {
        // The point of the record: two sources that do not depend on the picture, which
        // is what TimelineAnchor requires before it will hand out a timebase.
        var stimulusEpoch100ns = 50_000_000L;
        var record = new VideoEpochRecord(56_000_000, 10_000_000, VideoEpochSource.FrameTimestamp, null);
        var counter = record.ToAnchorEstimate(stimulusEpoch100ns, 1.0 / 60.0);
        Assert.NotNull(counter);

        var wallClock = TimelineAnchor.FromWallClock(
            DateTimeOffset.UnixEpoch, DateTimeOffset.UnixEpoch.AddSeconds(0.4), 0.4);

        var anchor = TimelineAnchor.Reconcile([wallClock, counter]);

        Assert.True(anchor.IsEstablished);
        Assert.Equal(0.6, anchor.OffsetSeconds, 3);
    }
}
