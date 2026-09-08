using System.Text.Json;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The shared completeness and continuity criteria a finished recording is judged
/// against, ported from the release checklist.
/// </summary>
public sealed class RecordingIntegrityTests
{
    private static JsonElement Parse(string json) => JsonDocument.Parse(json).RootElement;

    private const string CleanCounters =
        """
        {
            "counters": {
                "audio_discontinuity_ms_total": 0,
                "audio_discontinuity_ms_longest": 0,
                "audio_discontinuities": 0,
                "mux_failures": 0,
                "encoder_keyframe_prediction_mismatches": 0,
                "frames_dropped": { "processing_failure": 0, "backpressure": 0 }
            }
        }
        """;

    [Fact]
    public void ProblemsFlagsAnAudioTrackShorterThan99PercentOfTheContainer()
    {
        var report = Parse("""{"counters":{}}""");
        var problems = RecordingIntegrity.Problems(report, containerSeconds: 10.0, audioSpanSeconds: [9.8]);

        Assert.Contains(problems, problem => problem.Contains("spans", StringComparison.Ordinal));
    }

    [Fact]
    public void ProblemsAcceptsAnAudioSpanAtExactly99Percent()
    {
        var report = Parse("""{"counters":{}}""");
        var problems = RecordingIntegrity.Problems(report, containerSeconds: 10.0, audioSpanSeconds: [9.9]);

        Assert.Empty(problems);
    }

    [Fact]
    public void ProblemsFlagsAudioDegradedOccurred()
    {
        var report = Parse("""{"counters":{},"audio":{"degraded_occurred":true}}""");
        var problems = RecordingIntegrity.Problems(report, containerSeconds: 10.0, audioSpanSeconds: [10.0]);

        Assert.Contains(problems, problem => problem.Contains("degraded_occurred", StringComparison.Ordinal));
    }

    [Fact]
    public void ProblemsFlagsUndrainedResamplerFrames()
    {
        var report = Parse(
            """{"counters":{},"audio":{"resampler_drain":[{"track":"1","undrained_frames":5}]}}""");
        var problems = RecordingIntegrity.Problems(report, containerSeconds: 10.0, audioSpanSeconds: [10.0]);

        Assert.Contains(problems, problem => problem.Contains("undrained", StringComparison.Ordinal));
    }

    [Fact]
    public void ProblemsFlagsAnUnfinalizedSegment()
    {
        var report = Parse("""{"counters":{},"segments":[{"index":"0","finalized":false}]}""");
        var problems = RecordingIntegrity.Problems(report, containerSeconds: 10.0, audioSpanSeconds: [10.0]);

        Assert.Contains(problems, problem => problem.Contains("not finalized", StringComparison.Ordinal));
    }

    [Fact]
    public void VerdictIsInfrastructureErrorWhenTheReportCarriesNoCounters()
    {
        var report = Parse("""{}""");
        var verdict = RecordingIntegrity.Verdict(report, containerSeconds: 10.0, audioSpanSeconds: [10.0]);

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdict.Outcome);
    }

    [Fact]
    public void VerdictIsInfrastructureErrorWhenThereIsNoAudioTrackToMeasure()
    {
        var report = Parse("""{"counters":{}}""");
        var verdict = RecordingIntegrity.Verdict(report, containerSeconds: 10.0, audioSpanSeconds: []);

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdict.Outcome);
    }

    [Fact]
    public void VerdictAcceptsABareReportAndASessionLatestEnvelopeIdentically()
    {
        var bareReport = Parse("""{"counters":{}}""");
        var envelope = Parse("""{"available":true,"report":{"counters":{}}}""");

        var fromBare = RecordingIntegrity.Verdict(bareReport, containerSeconds: 10.0, audioSpanSeconds: [10.0]);
        var fromEnvelope = RecordingIntegrity.Verdict(envelope, containerSeconds: 10.0, audioSpanSeconds: [10.0]);

        Assert.Equal(ScenarioOutcome.Pass, fromBare.Outcome);
        Assert.Equal(ScenarioOutcome.Pass, fromEnvelope.Outcome);
    }

    [Fact]
    public void SoakVerdictFailsWhenDurationSkewExceedsTwoPercent()
    {
        var report = Parse(CleanCounters);
        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 96.0, expectedSeconds: 100.0, audioSpanSeconds: [96.0]);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("skew", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SoakVerdictFailsWhenTheOutageBudgetIsExceeded()
    {
        const string counters =
            """
            {
                "counters": {
                    "audio_discontinuity_ms_total": 1500,
                    "audio_discontinuity_ms_longest": 50,
                    "audio_discontinuities": 3,
                    "mux_failures": 0,
                    "encoder_keyframe_prediction_mismatches": 0,
                    "frames_dropped": { "processing_failure": 0, "backpressure": 0 }
                }
            }
            """;
        var report = Parse(counters);

        // Budget is 0.1% of a 1000 s recording, i.e. 1000 ms; 1500 ms is over it.
        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 1000.0, expectedSeconds: 1000.0, audioSpanSeconds: [1000.0]);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("over the", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SoakVerdictFailsWhenTheLongestSingleOutageExceeds120Ms()
    {
        const string counters =
            """
            {
                "counters": {
                    "audio_discontinuity_ms_total": 10,
                    "audio_discontinuity_ms_longest": 200,
                    "audio_discontinuities": 1,
                    "mux_failures": 0,
                    "encoder_keyframe_prediction_mismatches": 0,
                    "frames_dropped": { "processing_failure": 0, "backpressure": 0 }
                }
            }
            """;
        var report = Parse(counters);

        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 1000.0, expectedSeconds: 1000.0, audioSpanSeconds: [1000.0]);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("longest single audio outage", verdict.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("mux_failures")]
    [InlineData("encoder_keyframe_prediction_mismatches")]
    [InlineData("frames_dropped.processing_failure")]
    [InlineData("frames_dropped.backpressure")]
    public void SoakVerdictFailsWhenAZeroToleranceCounterIsAbsent(string path)
    {
        using var document = JsonDocument.Parse(CleanCounters);
        var report = Strip(document.RootElement, "counters." + path);

        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains($"counters.{path} is absent", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SoakVerdictFailsWhenAZeroToleranceCounterIsNonZero()
    {
        const string counters =
            """
            {
                "counters": {
                    "audio_discontinuity_ms_total": 0,
                    "audio_discontinuity_ms_longest": 0,
                    "audio_discontinuities": 0,
                    "mux_failures": 1,
                    "encoder_keyframe_prediction_mismatches": 0,
                    "frames_dropped": { "processing_failure": 0, "backpressure": 0 }
                }
            }
            """;
        var report = Parse(counters);

        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("counters.mux_failures = 1", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SoakVerdictReportsDriftButNeverFailsACleanRunOnItAlone()
    {
        const string counters =
            """
            {
                "counters": {
                    "audio_discontinuity_ms_total": 0,
                    "audio_discontinuity_ms_longest": 0,
                    "audio_discontinuities": 0,
                    "mux_failures": 0,
                    "encoder_keyframe_prediction_mismatches": 0,
                    "frames_dropped": { "processing_failure": 0, "backpressure": 0 },
                    "av_drift_ms": 500,
                    "peak_av_drift_ms": 600
                }
            }
            """;
        var report = Parse(counters);

        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);

        Assert.Equal(ScenarioOutcome.Pass, verdict.Outcome);
        Assert.Contains("av_drift_ms 500", verdict.Message, StringComparison.Ordinal);
        Assert.Contains("peak 600", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SoakVerdictAcceptsABareReportAndASessionLatestEnvelopeIdentically()
    {
        using var bareDocument = JsonDocument.Parse(CleanCounters);
        var envelope = Parse($$"""{"available":true,"report":{{CleanCounters}}}""");

        var fromBare = RecordingIntegrity.SoakVerdict(
            bareDocument.RootElement, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);
        var fromEnvelope = RecordingIntegrity.SoakVerdict(
            envelope, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);

        Assert.Equal(ScenarioOutcome.Pass, fromBare.Outcome);
        Assert.Equal(ScenarioOutcome.Pass, fromEnvelope.Outcome);
    }

    [Fact]
    public void SoakVerdictIsInfrastructureErrorWhenThereIsNoSessionReport()
    {
        var report = Parse("""{}""");
        var verdict = RecordingIntegrity.SoakVerdict(
            report, containerSeconds: 100.0, expectedSeconds: 100.0, audioSpanSeconds: [100.0]);

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdict.Outcome);
    }

    // Removes one dotted counter path from a parsed document, so a zero-tolerance
    // counter can be tested as genuinely absent rather than merely zero.
    private static JsonElement Strip(JsonElement root, string dottedPath)
    {
        var segments = dottedPath.Split('.');
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            WriteWithout(writer, root, segments, 0);
        }

        stream.Position = 0;
        using var document = JsonDocument.Parse(stream.ToArray());
        return document.RootElement.Clone();
    }

    private static void WriteWithout(Utf8JsonWriter writer, JsonElement element, string[] path, int depth)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            element.WriteTo(writer);
            return;
        }

        writer.WriteStartObject();
        foreach (var property in element.EnumerateObject())
        {
            if (depth < path.Length && string.Equals(property.Name, path[depth], StringComparison.Ordinal) &&
                depth == path.Length - 1)
            {
                continue;
            }

            writer.WritePropertyName(property.Name);
            if (depth < path.Length && string.Equals(property.Name, path[depth], StringComparison.Ordinal))
            {
                WriteWithout(writer, property.Value, path, depth + 1);
            }
            else
            {
                property.Value.WriteTo(writer);
            }
        }

        writer.WriteEndObject();
    }
}
