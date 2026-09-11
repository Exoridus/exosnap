using ExoSnap.Verify.Adapters.DisposableOs;

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
