using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsHarnessPlumbingTests
{
    [Fact]
    public async Task TheHarnessDisposableOsFakeIsReachableFromAGateContext()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes => fakes.DisposableOs.Run = DisposableOsRun.Completed(new DisposableOsRunResult([])),
            TestContext.Current.CancellationToken);

        var services = harness.Context.RequireServices();
        var run = await services.DisposableOs.RunAsync(
            new DisposableOsWorkerRequest("noop.ps1", [], []), TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.Single(harness.Fakes.DisposableOs.Requests);
    }
}

/// <summary>REL-UPD-MSI-DECLINE-001: UpdateDeclineGate.</summary>
public sealed class UpdateDeclineGateTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(),
        "exosnap-base-" + Guid.NewGuid().ToString("N") + ".msi");

    public UpdateDeclineGateTests() => File.WriteAllText(this.baseMsi, "msi");

    public void Dispose() => File.Delete(this.baseMsi);

    [Fact]
    public async Task IsUnavailableWhenNoBaseMsiIsConfigured()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001", DisposableOsGateFixture.StageWorker, TestContext.Current.CancellationToken);

        var result = await new UpdateDeclineGate(() => null).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("EXOSNAP_UPDATE_FROM_MSI", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenTheWorkerScriptIsNotInTheRepository()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001", configure: null, TestContext.Current.CancellationToken);

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
        Assert.Contains("sandbox-update-worker.ps1", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsUnavailableWhenNoTransportCanRun()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Unavailable("no transport"));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task AFaultedRunIsAnInfrastructureErrorNeverAFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Faulted("the sandbox wrote no marker"));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
    }

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", true, "offered"),
            new("decline-apply", true, "applied"),
            new("decline-state", true, "failureCase uacDeclined, installState intact"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        var request = Assert.Single(harness.Fakes.DisposableOs.Requests);
        Assert.Equal("sandbox-update-worker.ps1", request.WorkerFileName);
        Assert.Contains(this.baseMsi, request.SourceFiles);
        Assert.Contains(Path.GetFileName(this.baseMsi), request.WorkerArguments);
    }

    [Fact]
    public async Task AFailedStepIsFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", false, "update.check timed out"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
    }

    [Fact]
    public async Task AStepTheWorkerNeverReachedIsAnInfrastructureErrorNotAFail()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
        ])));

        var result = await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("decline-offer", result.Message, StringComparison.Ordinal);
    }

    private static Task<GateHarness> HarnessAsync(DisposableOsRun run) => GateHarness.CreateAsync(
        "REL-UPD-MSI-DECLINE-001",
        fakes =>
        {
            DisposableOsGateFixture.StageWorker(fakes);
            fakes.DisposableOs.Run = run;
        },
        TestContext.Current.CancellationToken);
}

/// <summary>REL-UPD-MSI-001: UpdateAcceptGate.</summary>
public sealed class UpdateAcceptGateTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(),
        "exosnap-base-" + Guid.NewGuid().ToString("N") + ".msi");

    public UpdateAcceptGateTests() => File.WriteAllText(this.baseMsi, "msi");

    public void Dispose() => File.Delete(this.baseMsi);

    [Fact]
    public async Task AllRequiredStepsPassingIsPass()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("updater-gone-before-accept", true, "no stale updater"),
            new("accept-offer", true, "offered"),
            new("accept-apply", true, "applied"),
            new("accept-installed", true, "product version advanced"),
        ])));

        var result = await new UpdateAcceptGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }

    [Fact]
    public async Task TheDeclineStepsAreNotRequiredOfTheAcceptGate()
    {
        using var harness = await HarnessAsync(DisposableOsRun.Completed(new DisposableOsRunResult(
        [
            new("install-base", true, "installed"),
            new("decline-offer", false, "the decline half failed"),
            new("updater-gone-before-accept", true, "no stale updater"),
            new("accept-offer", true, "offered"),
            new("accept-apply", true, "applied"),
            new("accept-installed", true, "product version advanced"),
        ])));

        var result = await new UpdateAcceptGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }

    private static Task<GateHarness> HarnessAsync(DisposableOsRun run) => GateHarness.CreateAsync(
        "REL-UPD-MSI-001",
        fakes =>
        {
            DisposableOsGateFixture.StageWorker(fakes);
            fakes.DisposableOs.Run = run;
        },
        TestContext.Current.CancellationToken);
}

internal static class DisposableOsGateFixture
{
    /// <summary>Puts the guest worker script where a gate looks for it in the repository.</summary>
    internal static void StageWorker(GateFakes fakes)
    {
        var path = Path.Combine(fakes.RepositoryRoot, "scripts", "lib", UpdateDeclineGate.WorkerFileName);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, "# worker");
    }
}
