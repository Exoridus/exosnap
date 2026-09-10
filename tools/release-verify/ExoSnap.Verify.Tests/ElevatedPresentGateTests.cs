using ExoSnap.Verify.Adapters.Elevation;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Tests;

/// <summary>REL-PRESENT-002: ElevatedPresentGate scenario logic, against a fake worker host.</summary>
public sealed class ElevatedPresentGateTests
{
    private static Action<GateFakes> WithInteractiveDesktop(Action<GateFakes>? more = null) => fakes =>
    {
        fakes.CapabilityValues[CapabilityKeys.InteractiveDesktop] = "true";
        more?.Invoke(fakes);
    };

    private static ElevatedWorkerResult Result(
        ElevatedWorkerOutcome outcome, string message, bool elevated = true, long presentCount = 0,
        string presentMode = "", string oracleNote = "") =>
        ElevatedWorkerResult.For(
            "REL-PRESENT-002", outcome, message, elevated, presentCount, presentMode, oracleNote: oracleNote);

    [Fact]
    public async Task IsUnavailableWhenTheWorkerExecutableIsMissing()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Available = false),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task IsUnavailableWithoutAnInteractiveDesktop()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            fakes => fakes.ElevatedWorker.Completes(Result(ElevatedWorkerOutcome.Pass, "unused")),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("no interactive desktop", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task PassesWhenTheWorkerReportsElevatedPresents()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Completes(Result(
                ElevatedWorkerOutcome.Pass,
                "elevated present diagnostics decoded 2400 present(s), mode independentFlip",
                presentCount: 2400,
                presentMode: "independentFlip",
                oracleNote: "an independent PresentMon read is REL-PRESENT-XCHECK-001"))),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.Contains("decoded 2400", result.Message, StringComparison.Ordinal);
        Assert.Contains("REL-PRESENT-XCHECK-001", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheWorkerReportsElevatedButNoPresents()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Completes(Result(
                ElevatedWorkerOutcome.Fail,
                "the elevated session opened but decoded no presents while the monitor was recording"))),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("decoded no presents", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsDeferredWhenTheOperatorDeclinedTheElevationPrompt()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Completes(Result(
                ElevatedWorkerOutcome.Deferred,
                "the operator declined the elevation prompt; the elevated observation was not taken",
                elevated: false))),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Deferred, result.Outcome);
        Assert.Contains("declined the elevation prompt", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenTheWorkerFaulted()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Run =
                ElevatedWorkerRun.Faulted("the worker exited without writing a result document")),
            TestContext.Current.CancellationToken);

        var result = await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }

    [Fact]
    public async Task EndsTheSharedSessionAndPassesTheArtifactPathToTheWorker()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-PRESENT-002",
            WithInteractiveDesktop(fakes => fakes.ElevatedWorker.Completes(Result(
                ElevatedWorkerOutcome.Pass, "decoded presents", presentCount: 12, presentMode: "composed"))),
            TestContext.Current.CancellationToken);

        await new ElevatedPresentGate().RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.True(harness.Fakes.SessionHost.EndCallCount >= 1);
        var call = Assert.Single(harness.Fakes.ElevatedWorker.RunCalls);
        Assert.Equal("REL-PRESENT-002", call.TaskId);
        Assert.Equal(harness.Fakes.ExecutablePath, call.TargetExe);
        Assert.False(call.SelfTest);
    }
}
