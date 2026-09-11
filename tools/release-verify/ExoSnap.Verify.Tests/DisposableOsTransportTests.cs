using ExoSnap.Verify.Adapters.DisposableOs;

namespace ExoSnap.Verify.Tests;

public sealed class DisposableOsVerdictTests
{
    private static readonly string[] DeclineSteps =
        ["install-base", "decline-offer", "decline-apply", "decline-state"];

    [Fact]
    public void EveryRequiredStepPresentAndOkIsPass()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-pass.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Pass, verdict.Kind);
        Assert.Contains("4", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepThatRanAndFailedIsFail()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-fail.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("decline-apply", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ARequiredStepTheWorkerNeverReachedIsUnverified()
    {
        var result = DisposableOsRunResult.Parse(Fixtures.Read("disposable-os-steps-incomplete.json"));

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void NoResultDocumentAtAllIsUnverified()
    {
        var verdict = DisposableOsVerdict.From(null, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("no result document", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void UnparseableJsonParsesToNull()
    {
        Assert.Null(DisposableOsRunResult.Parse("{ not json"));
    }

    [Fact]
    public void AStepWithoutANameIsUnverifiedRatherThanACrash()
    {
        var result = DisposableOsRunResult.Parse("""{"steps":[{"ok":true,"detail":"ran"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("install-base", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepWithoutADetailStillReportsItsFailure()
    {
        var result = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"install-base","ok":true,"detail":"ran"},{"name":"decline-offer","ok":false}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ANullStepEntryIsIgnored()
    {
        var result = DisposableOsRunResult.Parse("""{"steps":[null,{"name":"install-base","ok":true,"detail":"ran"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("decline-offer", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ADuplicateStepCannotMaskAFailure()
    {
        var result = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"decline-apply","ok":false,"detail":"applied anyway"},{"name":"decline-apply","ok":true,"detail":"retried"}]}""");

        var verdict = DisposableOsVerdict.From(result, DeclineSteps);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
        Assert.Contains("applied anyway", verdict.Message, StringComparison.Ordinal);
    }
}

public sealed class DisposableOsRunnerTests
{
    private sealed class FixedTransport : IDisposableOsTransport
    {
        private readonly DisposableOsRun run;

        public FixedTransport(string name, bool available, DisposableOsRun run)
        {
            this.Name = name;
            this.Available = available;
            this.run = run;
        }

        public string Name { get; }

        public bool Available { get; }

        public string UnavailableReason => this.Available ? string.Empty : $"{this.Name} is not available";

        public List<DisposableOsWorkerRequest> Requests { get; } = [];

        public Task<DisposableOsRun> RunWorkerAsync(DisposableOsWorkerRequest request, CancellationToken cancellationToken)
        {
            this.Requests.Add(request);
            return Task.FromResult(this.run);
        }
    }

    [Fact]
    public async Task SkipsAnUnavailableTransportAndUsesTheNextOne()
    {
        var unavailable = new FixedTransport("primary", available: false, DisposableOsRun.Faulted("unreachable"));
        var completed = DisposableOsRun.Completed(new DisposableOsRunResult([]));
        var fallback = new FixedTransport("fallback", available: true, completed);
        var runner = new DisposableOsRunner([unavailable, fallback]);
        var request = new DisposableOsWorkerRequest("worker.ps1", [], []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Same(completed, run);
        Assert.Empty(unavailable.Requests);
        Assert.Single(fallback.Requests);
    }

    [Fact]
    public async Task NoAvailableTransportIsUnavailableNotFaulted()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("primary", available: false, DisposableOsRun.Faulted("n/a")),
        ]);
        var request = new DisposableOsWorkerRequest("worker.ps1", [], []);

        var run = await runner.RunAsync(request, TestContext.Current.CancellationToken);

        Assert.Equal(DisposableOsRunKind.Unavailable, run.Kind);
        Assert.Contains("no disposable-OS transport", run.Detail, StringComparison.Ordinal);
        Assert.Contains("primary is not available", run.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void TransportNamesReportsEveryTransportInOrder()
    {
        var runner = new DisposableOsRunner(
        [
            new FixedTransport("sandbox", available: false, DisposableOsRun.Faulted("n/a")),
            new FixedTransport("vm", available: true, DisposableOsRun.Faulted("n/a")),
        ]);

        Assert.Equal(["sandbox", "vm"], runner.TransportNames);
    }
}
