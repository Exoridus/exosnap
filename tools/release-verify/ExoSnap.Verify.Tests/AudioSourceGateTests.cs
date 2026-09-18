using System.Collections.ObjectModel;
using System.Text.Json;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Reading render endpoints the way the product enumerates them.
/// </summary>
public sealed class AudioEndpointTests
{
    private static JsonElement Snapshot(string json) => JsonDocument.Parse(json).RootElement;

    [Fact]
    public void EndpointsComeFromTheProductsOwnEnvironmentSnapshot()
    {
        var endpoints = AudioEndpoints.From(Snapshot(
            """{"audio":{"outputs":[{"name":"Speakers","default":true},{"name":"CABLE Input","default":false}]}}"""));

        Assert.Equal(2, endpoints.Count);
        Assert.Equal("Speakers", AudioEndpoints.DefaultName(endpoints));
    }

    [Fact]
    public void ASnapshotWithNoOutputsYieldsNoEndpoints()
    {
        Assert.Empty(AudioEndpoints.From(Snapshot("""{"audio":{}}""")));
        Assert.Null(AudioEndpoints.DefaultName(AudioEndpoints.From(Snapshot("""{}"""))));
    }

    [Fact]
    public void AnEndpointWithNoNameIsNotAnEndpoint()
    {
        // The switcher addresses an endpoint by name. One without a name cannot be
        // routed to, and offering it would produce a command that silently does
        // nothing.
        Assert.Empty(AudioEndpoints.From(Snapshot("""{"audio":{"outputs":[{"default":true}]}}""")));
    }

    [Fact]
    public void APatternMatchesByPrefixAndIsCaseInsensitive()
    {
        var endpoints = new[] { new AudioEndpoint("CABLE Input (VB-Audio Virtual Cable)", false) };

        Assert.NotNull(AudioEndpoints.MatchingName(endpoints, "CABLE Input*"));
        Assert.NotNull(AudioEndpoints.MatchingName(endpoints, "cable input*"));
    }

    [Fact]
    public void AnAbsentDeviceIsNotSubstitutedWithTheFirstOutput()
    {
        // The rule that keeps a gate from reconfiguring somebody's speakers because
        // the device it wanted was not there.
        var endpoints = new[] { new AudioEndpoint("Speakers", true), new AudioEndpoint("Headset", false) };

        Assert.Null(AudioEndpoints.MatchingName(endpoints, "CABLE Input*"));
    }

    [Theory]
    [InlineData("44100/24/2", 44100)]
    [InlineData("48000/16/2", 48000)]
    public void TheSampleRateIsReadOutOfTheWaveFormat(string format, int expected)
    {
        Assert.Equal(expected, AudioEndpoints.SampleRateOf(format));
    }

    [Theory]
    [InlineData("")]
    [InlineData("unknown")]
    [InlineData("0/16/2")]
    public void AFormatThatDeclaresNoUsableRateYieldsNothing(string format)
    {
        Assert.Null(AudioEndpoints.SampleRateOf(format));
    }
}

/// <summary>
/// What a window of samples says about a connected but silent source.
/// </summary>
/// <remarks>
/// ADR 0046: degradation means the device is gone, not that it is quiet. The second
/// answer this has to give is the one the PowerShell scenario got wrong -- a window
/// in which no source was ever active proves nothing about how a silent one is
/// treated, the way an empty list satisfies any assertion over it, and calling that a
/// product failure accuses ExoSnap of the harness having listened to nothing.
/// </remarks>
public sealed class AudioSilenceVerdictTests
{
    private static readonly Evidence[] None = [];

    [Fact]
    public void AnActiveSourceThatWasNeverDegradedPasses()
    {
        var verdict = AudioEndpoints.SilenceVerdict(
            [new AudioSample(true, false), new AudioSample(true, false)], None);

        Assert.Equal(ScenarioOutcome.Pass, verdict.Outcome);
    }

    [Fact]
    public void ASingleDegradedSampleFails()
    {
        // Not a majority vote: the product reported the device gone while it was
        // connected, once, and that is the defect.
        var verdict = AudioEndpoints.SilenceVerdict(
            [new AudioSample(true, false), new AudioSample(true, true), new AudioSample(true, false)], None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("1 of 3", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AWindowWithNoActiveSourceIsAnInfrastructureErrorNotAFailure()
    {
        // The correction to the PowerShell scenario, which reported this as FAIL. What
        // was not measured is never a defect.
        var verdict = AudioEndpoints.SilenceVerdict(
            [new AudioSample(false, false), new AudioSample(false, false)], None);

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdict.Outcome);
        Assert.Contains("nothing was observed", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AWindowWithNoSamplesAtAllIsAlsoAnInfrastructureError()
    {
        var verdict = AudioEndpoints.SilenceVerdict([], None);

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdict.Outcome);
        Assert.Contains("never polled", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AnInactiveSampleAmongActiveOnesDoesNotVoidTheWindow()
    {
        // A source is briefly inactive between buffers. The requirement is that audio
        // was observed at all, not that every poll caught it.
        var verdict = AudioEndpoints.SilenceVerdict(
            [new AudioSample(false, false), new AudioSample(true, false)], None);

        Assert.Equal(ScenarioOutcome.Pass, verdict.Outcome);
    }
}

/// <summary>
/// What a recording made on a 44.1 kHz endpoint has to carry.
/// </summary>
/// <remarks>
/// The recorded rate is deliberately not required to equal the endpoint's. The engine
/// mixes at 48 kHz and resampling to it is correct; what would be wrong is no audio
/// at all, or a rate that is neither, which means something in the path invented one.
/// </remarks>
public sealed class AudioFormatVerdictTests
{
    private static readonly Evidence[] None = [];

    [Theory]
    [InlineData(44100)]
    [InlineData(48000)]
    public void TheEndpointRateAndTheEngineMixRateAreBothCorrect(int recorded)
    {
        Assert.Equal(
            ScenarioOutcome.Pass,
            AudioFormatGate.AudioFormatVerdict([recorded], "Test Endpoint", None).Outcome);
    }

    [Fact]
    public void NoAudioTrackAtAllIsAFailure()
    {
        var verdict = AudioFormatGate.AudioFormatVerdict([], "Test Endpoint", None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("no audio track", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AThirdRateNobodyAskedForIsAFailureAndIsNamed()
    {
        var verdict = AudioFormatGate.AudioFormatVerdict([32000], "Test Endpoint", None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("32000", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStreamWhoseRateFfprobeCouldNotReadIsNotTreatedAsCorrect()
    {
        var verdict = AudioFormatGate.AudioFormatVerdict([null], "Test Endpoint", None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("unknown", verdict.Message, StringComparison.Ordinal);
    }
}

/// <summary>
/// Which of the audio scenarios now carry an executable body.
/// </summary>
/// <remarks>
/// REL-AUD-DEGRADE-001 needs an audio endpoint to physically disappear mid-recording,
/// which the envctl catalogue classifies as PHYSICAL for a reason -- no API causes it.
/// It is migrated through <see cref="OperatorGate"/> asking a person to do it, rather
/// than the harness faking the unplug.
/// </remarks>
public sealed class AudioGateMigrationTests
{
    [Theory]
    [InlineData("REL-AUD-DEGRADE-001")]
    [InlineData("REL-AUD-SILENCE-001")]
    [InlineData("REL-AUD-FORMAT-001")]
    public void TheAutomatableAudioScenariosAreMigrated(string id)
    {
        Assert.Contains(id, ReleaseCatalog.MigratedIds());
    }

    [Fact]
    public void NeitherMigratedAudioGateIsRequiredForPromotion()
    {
        // Both need a prepared machine -- a virtual cable, a 44.1 kHz endpoint -- so
        // they are opt-in. A required gate that is Unavailable on every machine
        // without that hardware would block every promotion.
        Assert.DoesNotContain("REL-AUD-SILENCE-001", ReleaseCatalog.RequiredIds());
        Assert.DoesNotContain("REL-AUD-FORMAT-001", ReleaseCatalog.RequiredIds());
    }
}

/// <summary>
/// REL-AUD-DEGRADE-001: losing an audio endpoint mid-recording degrades to honest
/// silence and recovers.
/// </summary>
public sealed class AudioDegradeGateTests
{
    private static string Pipeline(string lifecycle, bool degraded)
    {
        var degradedText = degraded ? "true" : "false";
        return $$$"""{"valid":true,"lifecycle":"{{{lifecycle}}}","audio":{"active":true,"sourceDegraded":{{{degradedText}}}}}""";
    }

    private static void ConfigureAudioEnabledAndRecording(GateFakes fakes)
    {
        fakes.Session.SetResult("record.snapshot", """{"systemAudioEnabled":true}""");
        fakes.Session.ScriptRecordingStates("Recording", "Completed");
    }

    private static void ConfigurePassingOutput(GateFakes fakes)
    {
        fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(true, "out.mkv"));
        fakes.Ffprobe.InspectResult = new FfprobeResult(
            new ReadOnlyCollection<FfprobeTrack>([new FfprobeTrack(0, "audio", "aac", 48000, 2)]),
            new FfprobeFormat("matroska", 10.0),
            "{}");
    }

    // The sample bound, not the window, is what ends these polls: a 60 ms window took one
    // sample on a loaded runner and three on an idle one, so the outcome the test asserted
    // was the machine's speed. The window is long enough that it never decides anything.
    private const int PollSamples = 3;

    private static AudioDegradeGate Gate() =>
        new(pollFor: TimeSpan.FromSeconds(30), pollEvery: TimeSpan.Zero, maxSamples: PollSamples);

    private static int SnapshotsTaken(GateHarness harness) =>
        harness.Fakes.Session.InvokedCommands.Count(
            command => string.Equals(command, "pipeline.snapshot", StringComparison.Ordinal));

    [Fact]
    public async Task AConfirmedAnswerObservesTheDegradeAndRecoverCycleAndPasses()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Session.SetResultSequence(
                    "pipeline.snapshot",
                    Pipeline("recording", degraded: false),
                    Pipeline("recording", degraded: true),
                    Pipeline("recording", degraded: false));
                ConfigurePassingOutput(fakes);
                fakes.Operator = new OperatorGate(new ScriptedOperator(""));
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("recovered", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ADeclinedAnswerFailsWithoutPolling()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Operator = new OperatorGate(new ScriptedOperator("n"));
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("declined", result.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("pipeline.snapshot", harness.Fakes.Session.InvokedCommands);
    }

    [Fact]
    public async Task AttestationStillVerifiesTheConsequenceAndMarksTheRecord()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Session.SetResultSequence(
                    "pipeline.snapshot",
                    Pipeline("recording", degraded: false),
                    Pipeline("recording", degraded: true),
                    Pipeline("recording", degraded: false));
                ConfigurePassingOutput(fakes);
                fakes.Operator = new OperatorGate(console: null, attested: ["REL-AUD-DEGRADE-001"]);
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.StartsWith("[attested]", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task AnUnattendedRunIsUnavailableNeverPassOrFail()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            ConfigureAudioEnabledAndRecording,
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("nobody is at the machine", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheRecordingLeavesTheRunningLifecycleDuringThePoll()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Session.SetResultSequence(
                    "pipeline.snapshot",
                    Pipeline("recording", degraded: false),
                    Pipeline("completed", degraded: false));
                fakes.Operator = new OperatorGate(new ScriptedOperator(""));
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("ADR 0046", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenDegradationIsNeverObserved()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Session.SetResult("pipeline.snapshot", Pipeline("recording", degraded: false));
                fakes.Operator = new OperatorGate(new ScriptedOperator(""));
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("no audio-source degradation was observed", result.Message, StringComparison.Ordinal);

        // Without this the test passes just as well when the poll never sampled at all,
        // which is the one way this message can be printed for a reason nobody meant.
        Assert.Equal(PollSamples, SnapshotsTaken(harness));
    }

    [Fact]
    public async Task FailsWhenDegradationNeverClears()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-AUD-DEGRADE-001",
            fakes =>
            {
                ConfigureAudioEnabledAndRecording(fakes);
                fakes.Session.SetResultSequence(
                    "pipeline.snapshot",
                    Pipeline("recording", degraded: false),
                    Pipeline("recording", degraded: true));
                fakes.Operator = new OperatorGate(new ScriptedOperator(""));
            },
            TestContext.Current.CancellationToken);

        var result = await Gate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("never cleared", result.Message, StringComparison.Ordinal);
        Assert.Equal(PollSamples, SnapshotsTaken(harness));
    }
}
