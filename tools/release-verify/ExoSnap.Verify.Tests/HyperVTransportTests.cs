using ExoSnap.Verify.Adapters.DisposableOs;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// What decides whether the Hyper-V transport can carry a run here.
/// </summary>
/// <remarks>
/// Hyper-V is administered through a group, not only through elevation: a member of
/// the local Hyper-V Administrators group creates, starts and removes machines
/// without a consent prompt. A transport that demanded elevation anyway would put a
/// Secure Desktop prompt in front of every campaign -- something no automation can
/// answer and the developer has to be sitting there for -- on a machine where the
/// rights were already granted.
/// </remarks>
public sealed class HyperVAccessTests
{
    private static HyperVAccess Granted => new(
        ModulePresent: true, InAdministratorsGroup: true, Elevated: false, HostServiceRunning: true);

    [Fact]
    public void GroupMembershipAloneIsEnough()
    {
        Assert.True(Granted.Usable);
        Assert.Empty(Granted.DescribeUnavailable());
    }

    [Fact]
    public void ElevationAloneIsAlsoEnough()
    {
        // An elevated session administers Hyper-V whether or not the account is in
        // the group, so refusing it would be wrong in the other direction.
        var elevated = Granted with { InAdministratorsGroup = false, Elevated = true };

        Assert.True(elevated.Usable);
    }

    [Fact]
    public void NeitherTheGroupNorElevationNamesTheGroupFirst()
    {
        // The group is the fix worth suggesting: it costs one prompt now and none on
        // any later run. Advising a developer to run every campaign elevated instead
        // would put a Secure Desktop prompt in front of automation forever, so the
        // message has to point at the group and say what it buys.
        var reason = (Granted with { InAdministratorsGroup = false }).DescribeUnavailable();

        Assert.Contains("Hyper-V Administrators", reason, StringComparison.Ordinal);
        Assert.Contains("no prompt afterwards", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void AMissingModuleIsNotAPermissionProblem()
    {
        // Windows without the Hyper-V feature. Reported as the feature it is, because
        // adding a group membership will not help and the message would send someone
        // to the wrong place.
        var reason = (Granted with { ModulePresent = false }).DescribeUnavailable();

        Assert.Contains("Hyper-V", reason, StringComparison.Ordinal);
        Assert.DoesNotContain("Hyper-V Administrators", reason, StringComparison.Ordinal);
    }

    [Fact]
    public void AStoppedHostServiceIsItsOwnReason()
    {
        var reason = (Granted with { HostServiceRunning = false }).DescribeUnavailable();

        Assert.Contains("vmms", reason, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void EveryMissingPieceIsNamedAtOnce()
    {
        var reason = new HyperVAccess(false, false, false, false).DescribeUnavailable();

        Assert.Contains("Hyper-V", reason, StringComparison.Ordinal);
        Assert.Contains("vmms", reason, StringComparison.OrdinalIgnoreCase);
    }
}

/// <summary>
/// Which transport may carry a run that declares what it needs.
/// </summary>
/// <remarks>
/// A worker that captures the desktop needs a machine whose interactive session was
/// proven, and the fallback order must not quietly hand such a run to a transport
/// that cannot prove it. The failure that produces is the one this whole area exists
/// to prevent: every file copies, the channel answers, and the capture fails for a
/// reason that reads like a product defect.
/// </remarks>
public sealed class TransportSelectionTests
{
    private static DisposableOsWorkerRequest Capture => new("worker.ps1", ["worker.ps1"], [])
    {
        RequiresInteractiveGuest = true,
    };

    private static DisposableOsWorkerRequest Plain => new("worker.ps1", ["worker.ps1"], []);

    [Fact]
    public async Task ARunNeedingAProvenDesktopIsNotHandedToATransportThatCannotProveOne()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("sandbox", available: true, DisposableOsRun.Completed(new DisposableOsRunResult([]))),
        ]);

        var run = await runner.RunAsync(Capture, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Contains("interactive", run.Detail, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task ARunNeedingNothingSpecialStillUsesTheFirstAvailableTransport()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("sandbox", available: true, DisposableOsRun.Completed(new DisposableOsRunResult([]))),
        ]);

        var run = await runner.RunAsync(Plain, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
    }

    [Fact]
    public async Task ATransportThatProvesADesktopIsPreferredOverOneThatDoesNot()
    {
        // Order in the list is the fallback order, and a capable transport that is
        // available takes the run even though the other one would also answer.
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("vm", available: true, DisposableOsRun.Completed(new DisposableOsRunResult([])))
            {
                ProvesInteractiveGuest = true,
            },
            new FixedTransport("sandbox", available: true, DisposableOsRun.Faulted("wrong transport")),
        ]);

        var run = await runner.RunAsync(Capture, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
    }

    [Fact]
    public async Task AnUnavailableCapableTransportFallsBackOnlyForRunsThatDoNotNeedIt()
    {
        var transports = new List<IDisposableOsTransport>
        {
            new FixedTransport("vm", available: false, DisposableOsRun.Faulted("n/a")) { ProvesInteractiveGuest = true },
            new FixedTransport("sandbox", available: true, DisposableOsRun.Completed(new DisposableOsRunResult([]))),
        };

        var plain = await new DisposableOsRunner(transports).RunAsync(Plain, TestContext.Current.CancellationToken);
        Assert.Equal(DisposableOsRunKind.Completed, plain.Kind);

        var capture = await new DisposableOsRunner(transports).RunAsync(Capture, TestContext.Current.CancellationToken);
        Assert.Equal(DisposableOsRunKind.Unavailable, capture.Kind);
        Assert.Contains("vm", capture.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void TheSandboxTransportDoesNotClaimToProveAnInteractiveGuest()
    {
        // Not a statement about what Windows Sandbox can do. Nothing in this transport
        // measures the session a worker lands in, so it cannot back the claim, and a
        // transport that claimed it anyway would be the harness asserting something it
        // never checked.
        using var processes = new ExoSnap.Verify.Processes.ProcessRunner();
        var transport = new SandboxTransport(
            processes,
            new ExoSnap.Verify.Capabilities.ToolResolver(
                readEnvironment: _ => null,
                fileExists: path => path.EndsWith("WindowsSandbox.exe", StringComparison.OrdinalIgnoreCase),
                readPath: () => @"C:\Windows\System32"),
            Path.Combine(Path.GetTempPath(), "exosnap-selection-tests"),
            Path.Combine(Path.GetTempPath(), "exosnap-selection-tests", "pwsh"));

        Assert.False(transport.ProvesInteractiveGuest);
    }
}
