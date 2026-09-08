using System.Collections.ObjectModel;
using System.Globalization;
using ExoSnap.Verify.Adapters.Ffprobe;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Adapters.PresentMon;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Small JSON snippets shared across gate tests, so a test body reads as the
/// document it is shaping rather than as string concatenation.
/// </summary>
internal static class GateJsonSamples
{
    public static string Environment(int hdrDisplays = 0, int totalDisplays = 1)
    {
        var screens = string.Join(
            ",",
            Enumerable.Range(0, totalDisplays).Select(index => string.Format(
                CultureInfo.InvariantCulture,
                "{{\"name\":\"DISPLAY{0}\",\"primary\":{1},\"hdrActive\":{2},\"devicePixelRatio\":1.0}}",
                index + 1,
                index == 0 ? "true" : "false",
                index < hdrDisplays ? "true" : "false")));
        return $$$"""{"displays":{"screens":[{{{screens}}}]}}""";
    }

    public static string RecordResult(bool succeeded, string outputPath = "") =>
        $$"""{"succeeded":{{(succeeded ? "true" : "false")}},"outputPath":"{{outputPath.Replace("\\", "\\\\")}}"}""";

    public static string Preview(int consumed, bool owed, int renders) =>
        $$"""{"consumedFrames":{{consumed}},"updateGate":{"owed":{{(owed ? "true" : "false")}},"renderPasses":{{renders}}},"active":true,"frameReady":true,"statusText":"ok"}""";

    public static string Windows(bool nativeWindowCreated, int width, int height) =>
        $$$"""{"windows":[{"role":"main","nativeWindowCreated":{{{(nativeWindowCreated ? "true" : "false")}}},"native":{"width":{{{width}}},"height":{{{height}}}}}]}""";
}

/// <summary>REL-ENV-001: EnvironmentClassificationGate.</summary>
public sealed class EnvironmentClassificationGateTests
{
    [Fact]
    public async Task IsUnavailableWhenEnvctlIsNotBuilt()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-001", fakes => fakes.Envctl.Available = false, TestContext.Current.CancellationToken);

        var result = await new EnvironmentClassificationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task FailsOnAnEmptyCatalogue()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-001",
            fakes => fakes.Envctl.DescribeJson = """{"ok":true,"catalogue":[]}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentClassificationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("empty", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsOnAnUnclassifiedEntry()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-001",
            fakes => fakes.Envctl.DescribeJson =
                """{"ok":true,"catalogue":[{"capability":"NOT_A_REAL_CLASS","readMechanism":"x"}]}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentClassificationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("no recognised capability class", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsOnAMutateSafeEntryWithNoMutateMechanism()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-001",
            fakes => fakes.Envctl.DescribeJson =
                """{"ok":true,"catalogue":[{"capability":"ENV_MUTATE_SAFE","readMechanism":"x"}]}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentClassificationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("name no mechanism", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesOnTheRealFixture()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-001",
            fakes => fakes.Envctl.DescribeJson = Fixtures.Read("envctl-describe.json"),
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentClassificationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("21 properties classified", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-ENV-002: DeviceAliasGate.</summary>
public sealed class DeviceAliasGateTests
{
    [Fact]
    public async Task IsUnavailableWhenEnvctlIsNotBuilt()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-002", fakes => fakes.Envctl.Available = false, TestContext.Current.CancellationToken);

        var result = await new DeviceAliasGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task FailsOnAnAmbiguousDevice()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-002",
            fakes => fakes.Envctl.ResolveAliasesJson =
                """{"ok":true,"bindings":[{"alias":"display.main-hdr","status":"ambiguous_device"}],"errors":[],"candidates":[]}""",
            TestContext.Current.CancellationToken);

        var result = await new DeviceAliasGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("ambiguous_device for: display.main-hdr", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenThereAreNoBindingsAndNoErrors()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-002",
            fakes => fakes.Envctl.ResolveAliasesJson =
                """{"ok":true,"bindings":[],"errors":[],"candidates":[{"kind":"display"}]}""",
            TestContext.Current.CancellationToken);

        var result = await new DeviceAliasGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("no alias profile", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesOnTheRealFixture()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-002",
            fakes => fakes.Envctl.ResolveAliasesJson = Fixtures.Read("envctl-resolve-aliases.json"),
            TestContext.Current.CancellationToken);

        var result = await new DeviceAliasGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("4 alias(es) bound", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-ENV-003: EnvironmentMutationGate.</summary>
public sealed class EnvironmentMutationGateTests
{
    private const string TwoUntwinnedModes =
        """{"ok":true,"displays":[{"modes":[{"refreshHz":60},{"refreshHz":30}],"current":{"refreshHz":60}}]}""";

    [Fact]
    public async Task IsUnavailableWhenEnvctlIsNotBuilt()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003", fakes => fakes.Envctl.Available = false, TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task IsUnavailableWhenTheDisplayModesCannotBeEnumerated()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003",
            fakes => fakes.Envctl.ListModesJson = """{"ok":false,"command":"list-modes"}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task IsUnavailableWhenNoDisplayResolves()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003",
            fakes => fakes.Envctl.ListModesJson = """{"ok":true,"displays":[]}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task IsUnavailableWhenEveryOfferedRateIsTwinned()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003",
            fakes => fakes.Envctl.ListModesJson =
                """{"ok":true,"displays":[{"modes":[{"refreshHz":60},{"refreshHz":59}],"current":{"refreshHz":0}}]}""",
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("no alternative refresh rate", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheTransactionAppliedMoreThanOneProperty()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003",
            fakes =>
            {
                fakes.Envctl.ListModesJson = TwoUntwinnedModes;
                fakes.Envctl.BeginJson =
                    """
                    {"ok":true,"command":"begin","state":"Active","applied":[
                        {"property":"display.main-hdr:refresh-hz","from":"60","to":"30"},
                        {"property":"display.main-hdr:mode","from":"a","to":"b"}
                    ]}
                    """;
            },
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("touched 2 properties", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenTheTransactionAppliedExactlyOneProperty()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-ENV-003",
            fakes =>
            {
                fakes.Envctl.ListModesJson = TwoUntwinnedModes;
                fakes.Envctl.BeginJson =
                    """{"ok":true,"command":"begin","state":"Active","applied":[{"property":"display.main-hdr:refresh-hz","from":"60","to":"30"}]}""";
            },
            TestContext.Current.CancellationToken);

        var result = await new EnvironmentMutationGate().RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("verified by read-back", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-SCHEMA-001: FieldContractGate.</summary>
public sealed class FieldContractGateTests
{
    private static void ConfigureCleanContract(FakeLiveVerifySession session)
    {
        session.SetResult(
            "app.identity", """{"productVersion":"0.9.1","executableSha256":"deadbeefdeadbeef"}""");
        session.SetResult(
            "environment.snapshot",
            """
            {"present":{"optIn":true,"elevated":false,"available":false,"availability":"requiresElevation"},
             "displays":{"screens":[{"name":"DISPLAY1","primary":true,"hdrActive":false,"devicePixelRatio":1.0}]}}
            """);
        session.SetResult(
            "windows.snapshot",
            """{"windows":[{"role":"main","nativeWindowCreated":true,"native":{"width":1000,"height":800}}]}""");
        session.SetResult(
            "preview.snapshot",
            """{"active":true,"frameReady":true,"updateGate":{"owed":false,"renderPasses":10},"consumedFrames":5,"statusText":"ok"}""");
        session.SetResult(
            "record.snapshot", """{"systemAudioEnabled":true,"recording":false,"finalizing":false,"sourceName":"Desktop"}""");
        session.SetResult("overlay.snapshot", """{"overlays":[{"visible":false}]}""");
        session.SetResult(
            "notifications.snapshot",
            """{"entries":[{"sequence":"1","title":"Recording saved","body":"C:\\out.mkv","severity":"info","unread":false,"actions":[]}]}""");
        session.SetResult(
            "update.getState",
            """{"updateAvailable":false,"state":"upToDate","blocker":"","currentVersion":"0.9.1","updaterLaunch":{"controlRunId":"","controlPipe":""}}""");
        session.SetResult(
            "pipeline.snapshot",
            """
            {"valid":true,"lifecycle":"recording","capture":{"actualFps":60.0},
             "sourcePresentation":{"presentMode":"composed","modeAvailability":"available"},
             "audio":{"active":true,"sourceDegraded":false,"degradedSources":[]},
             "avTiming":{"avDriftMs":5.0,"avDriftAvailability":"available"}}
            """);
        session.SetResult("record.result", """{"succeeded":true,"outputPath":"C:\\out.mkv"}""");
    }

    [Fact]
    public async Task FailsNamingTheMissingPath()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-SCHEMA-001",
            fakes =>
            {
                ConfigureCleanContract(fakes.Session);

                // productVersion is dropped, so app.identity.productVersion cannot resolve.
                fakes.Session.SetResult("app.identity", """{"executableSha256":"deadbeefdeadbeef"}""");
            },
            TestContext.Current.CancellationToken);

        var result = await new FieldContractGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("app.identity.productVersion", result.Message, StringComparison.Ordinal);
        Assert.Contains("is not emitted", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWithAnEmptyCollectionReportedAsUncheckedNotAsAPass()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-SCHEMA-001",
            fakes =>
            {
                ConfigureCleanContract(fakes.Session);

                // The collection is genuinely there and empty; its element shape cannot
                // be walked, and that must show up as "unchecked", not as a silent pass.
                fakes.Session.SetResult("overlay.snapshot", """{"overlays":[]}""");
            },
            TestContext.Current.CancellationToken);

        var result = await new FieldContractGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("unchecked", result.Message, StringComparison.Ordinal);
        Assert.Contains("overlays", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-PRESENT-001: UnelevatedPresentGate.</summary>
public sealed class UnelevatedPresentGateTests
{
    [Fact]
    public async Task IsUnavailableWhenTheProcessIsElevated()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-001",
            fakes => fakes.Session.SetResult(
                "environment.snapshot",
                """{"present":{"optIn":true,"elevated":true,"available":true,"availability":"available"}}"""),
            TestContext.Current.CancellationToken);

        var result = await new UnelevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task FailsWhenPresentDataIsClaimedWithoutElevation()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-001",
            fakes => fakes.Session.SetResult(
                "environment.snapshot",
                """{"present":{"optIn":false,"elevated":false,"available":true,"availability":"available"}}"""),
            TestContext.Current.CancellationToken);

        var result = await new UnelevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("opt-in did not reach", result.Message, StringComparison.Ordinal);
        Assert.Contains("available without elevation", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenTheOptInReachedTheSnapshotAndNothingIsClaimed()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-001",
            fakes => fakes.Session.SetResult(
                "environment.snapshot",
                """{"present":{"optIn":true,"elevated":false,"available":false,"availability":"requiresElevation"}}"""),
            TestContext.Current.CancellationToken);

        var result = await new UnelevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }
}

/// <summary>REL-PRESENT-XCHECK-001: PresentCrossCheckGate.</summary>
public sealed class PresentCrossCheckGateTests
{
    [Fact]
    public async Task IsSkippedWithAPointerWhenCodeHashAndOsMajorBothMatch()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-XCHECK-001",
            fakes =>
            {
                fakes.CapabilityValues[CapabilityKeys.OsVersion] = "11.0.26200";
                fakes.LastPresentConfirmation = new("run-x", CapabilityKeys.Unknown, "11.0.9999");
            },
            TestContext.Current.CancellationToken);

        var result = await new PresentCrossCheckGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Skipped, result.Outcome);
        Assert.Contains("confirmed by run run-x", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsRequiredAgainWhenThePresentCodeHasMoved()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-XCHECK-001",
            fakes =>
            {
                fakes.CapabilityValues[CapabilityKeys.OsVersion] = "11.0.26200";
                fakes.LastPresentConfirmation = new("run-x", "some-other-hash", "11.0.9999");
                fakes.PresentMon.Available = false;
                fakes.PresentMon.UnavailableReason = "PresentMon is not resolvable on this machine";
            },
            TestContext.Current.CancellationToken);

        var result = await new PresentCrossCheckGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Equal("PresentMon is not resolvable on this machine", result.Message);
    }

    [Fact]
    public async Task FailsOnAGenuineDisagreement()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-XCHECK-001",
            fakes =>
            {
                var capturePath = Path.Combine(fakes.RepositoryRoot, "presentmon.csv");
                File.WriteAllText(capturePath, "placeholder");
                fakes.PresentCapturePath = capturePath;
                fakes.PresentMon.Available = true;
                fakes.PresentMon.Capture = new PresentMonCapture(
                    new ReadOnlyCollection<PresentRecord>(
                    [
                        new PresentRecord(
                            "exosnap.exe", 4242, "0x1", PresentMode.IndependentFlip,
                            "Hardware: Independent Flip", 1, false, 0.0, 16.0),
                    ]),
                    new ReadOnlyCollection<string>(["Application", "ProcessID", "PresentMode"]));
                fakes.Session.SetResult(
                    "pipeline.snapshot", """{"sourcePresentation":{"presentMode":"composed"}}""");
                fakes.Session.SetResult("app.snapshot", """{"processId":4242}""");
            },
            TestContext.Current.CancellationToken);

        var result = await new PresentCrossCheckGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("composed", result.Message, StringComparison.Ordinal);
        Assert.Contains("independentFlip", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenTheCaptureAttributedNoPresentToThePid()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-XCHECK-001",
            fakes =>
            {
                var capturePath = Path.Combine(fakes.RepositoryRoot, "presentmon.csv");
                File.WriteAllText(capturePath, "placeholder");
                fakes.PresentCapturePath = capturePath;
                fakes.PresentMon.Available = true;
                fakes.PresentMon.Capture = new PresentMonCapture(
                    new ReadOnlyCollection<PresentRecord>([]),
                    new ReadOnlyCollection<string>(["Application", "ProcessID", "PresentMode"]));
                fakes.Session.SetResult(
                    "pipeline.snapshot", """{"sourcePresentation":{"presentMode":"composed"}}""");
                fakes.Session.SetResult("app.snapshot", """{"processId":4242}""");
            },
            TestContext.Current.CancellationToken);

        var result = await new PresentCrossCheckGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }
}

/// <summary>REL-CAP-001: RecordingProducedGate.</summary>
public sealed class RecordingProducedGateTests
{
    [Fact]
    public async Task IsUnavailableWhenFfprobeIsNotAvailable()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-CAP-001", fakes => fakes.Ffprobe.Available = false, TestContext.Current.CancellationToken);

        var result = await new RecordingProducedGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task FailsWhenFfprobeFindsNoVideoStream()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-cap-novideo");
        var outputPath = Path.Combine(directory.Path, "out.mkv");
        File.WriteAllText(outputPath, "not a real container, just needs to exist");

        using var harness = await GateHarness.CreateAsync(
            "REL-CAP-001",
            fakes =>
            {
                fakes.Session.ScriptRecordingStates("Recording");
                fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(true, outputPath));
                fakes.Ffprobe.InspectResult = new FfprobeResult(
                    new ReadOnlyCollection<FfprobeTrack>([new FfprobeTrack(0, "audio", "aac", 48000, 2)]),
                    new FfprobeFormat("matroska", 10.0),
                    "{}");
            },
            TestContext.Current.CancellationToken);

        var result = await new RecordingProducedGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("expected exactly one video stream", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheContainerIsShorterThanThreeSeconds()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-cap-short");
        var outputPath = Path.Combine(directory.Path, "out.mkv");
        File.WriteAllText(outputPath, "placeholder");

        using var harness = await GateHarness.CreateAsync(
            "REL-CAP-001",
            fakes =>
            {
                fakes.Session.ScriptRecordingStates("Recording");
                fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(true, outputPath));
                fakes.Ffprobe.InspectResult = new FfprobeResult(
                    new ReadOnlyCollection<FfprobeTrack>([new FfprobeTrack(0, "video", "av1", null, null)]),
                    new FfprobeFormat("matroska", 1.0),
                    "{}");
            },
            TestContext.Current.CancellationToken);

        var result = await new RecordingProducedGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("reports only 1", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheNamedOutputFileDoesNotExist()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-CAP-001",
            fakes =>
            {
                fakes.Session.ScriptRecordingStates("Recording");
                fakes.Session.SetResult(
                    "record.result",
                    GateJsonSamples.RecordResult(true, Path.Combine(fakes.RepositoryRoot, "no-such-file.mkv")));
            },
            TestContext.Current.CancellationToken);

        var result = await new RecordingProducedGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("does not exist", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>
/// REL-CAP-QUIET-001: QuietStallGate. The only case a hermetic test can drive
/// without a real stall probe process is the resolver reporting none built.
/// </summary>
public sealed class QuietStallGateTests
{
    [Fact]
    public async Task IsUnavailableWhenNoStallProbeIsBuilt()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-CAP-QUIET-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new QuietStallGate(() => null).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("not built", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-DISP-REFRESH-001: DisplayRefreshGate.</summary>
public sealed class DisplayRefreshGateTests
{
    private const string UntwinnedModes =
        """{"ok":true,"displays":[{"modes":[{"refreshHz":60},{"refreshHz":30}],"current":{"refreshHz":60}}]}""";

    private const string OneAppliedProperty =
        """{"ok":true,"command":"begin","state":"Active","applied":[{"property":"display.main-hdr:refresh-hz","from":"60","to":"30"}]}""";

    [Fact]
    public async Task FailsWhenTheRecordingDoesNotSucceedAtTheAppliedRate()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-REFRESH-001",
            fakes =>
            {
                fakes.Envctl.ListModesJson = UntwinnedModes;
                fakes.Envctl.BeginJson = OneAppliedProperty;
                fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(false));
            },
            TestContext.Current.CancellationToken);

        var result = await new DisplayRefreshGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("failed at 30 Hz", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenTheRecordingSucceedsAtTheAppliedRate()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-REFRESH-001",
            fakes =>
            {
                fakes.Envctl.ListModesJson = UntwinnedModes;
                fakes.Envctl.BeginJson = OneAppliedProperty;
                fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(true));
                fakes.Session.SetResult("pipeline.snapshot", """{"capture":{"actualFps":29.97}}""");
            },
            TestContext.Current.CancellationToken);

        var result = await new DisplayRefreshGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("29.97", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-DISP-HDR-001: DisplayHdrGate.</summary>
public sealed class DisplayHdrGateTests
{
    [Fact]
    public async Task FailsWhenNoDisplayReportsHdrActiveAfterTheTransaction()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-HDR-001",
            fakes => fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(hdrDisplays: 0)),
            TestContext.Current.CancellationToken);

        var result = await new DisplayHdrGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("no HDR-active display", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenHdrIsActiveAndTheRecordingSucceeds()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-HDR-001",
            fakes =>
            {
                fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(hdrDisplays: 1));
                fakes.Session.SetResult("record.result", GateJsonSamples.RecordResult(true));
            },
            TestContext.Current.CancellationToken);

        var result = await new DisplayHdrGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("HDR active on 1 display", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-DISP-MIXED-001: MixedDisplayGate.</summary>
public sealed class MixedDisplayGateTests
{
    [Fact]
    public async Task IsUnavailableWithOnlyOneDisplay()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-MIXED-001",
            fakes => fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(totalDisplays: 1)),
            TestContext.Current.CancellationToken);

        var result = await new MixedDisplayGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenThePreviewNeverConsumedAFrame()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-MIXED-001",
            fakes =>
            {
                fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(totalDisplays: 2));

                // Never advances, so the ten-second watch window genuinely elapses.
                fakes.Session.SetResult("preview.snapshot", GateJsonSamples.Preview(3, owed: false, renders: 1));
            },
            TestContext.Current.CancellationToken);

        var result = await new MixedDisplayGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("never consumed a frame", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheDebtStandsAcrossEverySampleWithNoConsumedFrame()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-MIXED-001",
            fakes =>
            {
                fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(totalDisplays: 2));

                // One increase to establish liveness quickly, then frozen with the
                // publish debt standing for the rest of the samples.
                fakes.Session.SetResultSequence(
                    "preview.snapshot",
                    GateJsonSamples.Preview(0, owed: false, renders: 1),
                    GateJsonSamples.Preview(5, owed: true, renders: 2));
            },
            TestContext.Current.CancellationToken);

        var result = await new MixedDisplayGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("frozen preview", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenConsumedFramesAdvanced()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-MIXED-001",
            fakes =>
            {
                fakes.Session.SetResult("environment.snapshot", GateJsonSamples.Environment(totalDisplays: 2));
                fakes.Session.SetResultSequence(
                    "preview.snapshot",
                    GateJsonSamples.Preview(0, owed: false, renders: 1),
                    GateJsonSamples.Preview(5, owed: true, renders: 2),
                    GateJsonSamples.Preview(6, owed: true, renders: 3),
                    GateJsonSamples.Preview(7, owed: true, renders: 4),
                    GateJsonSamples.Preview(8, owed: true, renders: 5),
                    GateJsonSamples.Preview(9, owed: true, renders: 6),
                    GateJsonSamples.Preview(10, owed: true, renders: 7),
                    GateJsonSamples.Preview(11, owed: true, renders: 8));
            },
            TestContext.Current.CancellationToken);

        var result = await new MixedDisplayGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("kept presenting", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-DISP-DPI-001: DisplayScalingGate.</summary>
public sealed class DisplayScalingGateTests
{
    [Fact]
    public async Task FailsBelowTheProductMinimumSize()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-DPI-001",
            fakes => fakes.Session.SetResult("windows.snapshot", GateJsonSamples.Windows(true, 800, 600)),
            TestContext.Current.CancellationToken);

        var result = await new DisplayScalingGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("below the 860x700", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenNoNativeWindowExistsYet()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-DPI-001",
            fakes => fakes.Session.SetResult(
                "windows.snapshot", """{"windows":[{"role":"main","nativeWindowCreated":false}]}"""),
            TestContext.Current.CancellationToken);

        var result = await new DisplayScalingGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }

    [Fact]
    public async Task PassesAtOrAboveTheProductMinimumSize()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-DISP-DPI-001",
            fakes => fakes.Session.SetResult("windows.snapshot", GateJsonSamples.Windows(true, 1000, 800)),
            TestContext.Current.CancellationToken);

        var result = await new DisplayScalingGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }
}

/// <summary>REL-UPD-PORTABLE-001: PortableUpdateGate.</summary>
public sealed class PortableUpdateGateTests
{
    [Fact]
    public async Task IsUnavailableWhenTheHandoffScriptIsMissing()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-PORTABLE-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new PortableUpdateGate(() => null).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("live-verify-update-handoff.ps1", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-JOURNEY-001: ProductJourneyGate.</summary>
public sealed class ProductJourneyGateTests
{
    [Fact]
    public async Task IsUnavailableWhenTheJourneyScriptIsMissing()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-JOURNEY-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new ProductJourneyGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("live-verify-product-journey.ps1", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-SHUTDOWN-001: ShutdownGate.</summary>
public sealed class ShutdownGateTests
{
    [Fact]
    public async Task PassesWhenTheApplicationExitedAndEndedTheSharedSessionFirst()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-SHUTDOWN-001",
            fakes =>
            {
                fakes.Session.ShutdownResult = SessionShutdown.Exited;
                fakes.Session.BufferedEventCount = 3;
            },
            TestContext.Current.CancellationToken);

        var result = await new ShutdownGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Equal(1, harness.Fakes.SessionHost.EndCallCount);
        Assert.Equal(["sessions.end", "factory.launch"], harness.Fakes.SessionLog);
    }

    [Fact]
    public async Task FailsWhenTheApplicationDidNotExit()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-SHUTDOWN-001",
            fakes => fakes.Session.ShutdownResult = SessionShutdown.StillRunning,
            TestContext.Current.CancellationToken);

        var result = await new ShutdownGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenTheApplicationOwnedNoWindowToAsk()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-SHUTDOWN-001",
            fakes => fakes.Session.ShutdownResult = SessionShutdown.NotRequestable,
            TestContext.Current.CancellationToken);

        var result = await new ShutdownGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }
}
