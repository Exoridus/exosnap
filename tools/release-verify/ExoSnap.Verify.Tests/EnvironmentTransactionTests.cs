using ExoSnap.Verify.Adapters.Envctl;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The environment transaction orchestrator: restore-state mapping, refresh-rate
/// selection, the machine fingerprint, and the transaction lifecycle itself.
/// </summary>
public sealed class EnvironmentTransactionTests
{
    [Fact]
    public async Task BeginExceptionRunsUncancelledRestoreAndRecordsTheDebt()
    {
        var fake = new FakeEnvctl
        {
            BeginException = new OperationCanceledException("begin interrupted"),
            RestoreJson = """{"ok":false,"state":"RestorePending","error":"still owed"}""",
        };
        using var directory = FixtureTool.NewTemporaryDirectory("-begin-exception");
        var orchestrator = await OpenAsync(fake, directory, TestContext.Current.CancellationToken);
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();

        var outcome = await orchestrator.RunAsync(
            "REL-ENV-BEGIN-001",
            new Dictionary<string, string> { ["display:hdr"] = "on" },
            (_, _) => Task.FromResult(ScenarioResult.Pass("not reached")),
            cancelled.Token);

        Assert.Equal(RestoreResult.RestorePending, outcome.Restore);
        Assert.Equal("begin_exception", outcome.SetupErrorCode);
        Assert.True(orchestrator.Dirty);
        Assert.Contains("restore", fake.Calls);
        Assert.False(fake.RestoreToken.CanBeCanceled);
    }

    private static async Task<EnvironmentOrchestrator> OpenAsync(
        FakeEnvctl envctl,
        TemporaryDirectory directory,
        CancellationToken cancellationToken)
    {
        var journalDirectory = Path.Combine(directory.Path, "journal");
        return await EnvironmentOrchestrator.OpenAsync(
                envctl,
                "test-run",
                journalDirectory,
                Path.Combine(journalDirectory, "env-journal.json"),
                cancellationToken)
            .ConfigureAwait(false);
    }

    [Theory]
    [InlineData("Restored", true, RestoreResult.Restored)]
    [InlineData("RestorePending", true, RestoreResult.RestorePending)]
    [InlineData("RestorePendingDeviceUnavailable", true, RestoreResult.RestorePendingDeviceUnavailable)]
    [InlineData("RestoreFailed", true, RestoreResult.RestoreFailed)]
    [InlineData("some-state-nobody-declared", true, RestoreResult.RestoreFailed)]
    [InlineData("Clean", false, RestoreResult.RestoreFailed)]
    [InlineData("Clean", true, RestoreResult.Restored)]
    public void MapRestoreStateIsTotal(string state, bool ok, RestoreResult expected) =>
        Assert.Equal(expected, EnvironmentOrchestrator.MapRestoreState(state, ok));

    [Fact]
    public void SelectUntwinnedRefreshRateSkipsATwinnedPairAndPicksTheNextDistinctRate()
    {
        var target = EnvironmentOrchestrator.SelectUntwinnedRefreshRate([60, 59, 30], current: 0);
        Assert.Equal(30, target);
    }

    [Fact]
    public void SelectUntwinnedRefreshRateSkipsTheCurrentRate()
    {
        var target = EnvironmentOrchestrator.SelectUntwinnedRefreshRate([60, 30], current: 60);
        Assert.Equal(30, target);
    }

    [Fact]
    public void SelectUntwinnedRefreshRateReturnsNullWhenEveryOfferedRateIsTwinnedOrCurrent()
    {
        var target = EnvironmentOrchestrator.SelectUntwinnedRefreshRate([60, 59], current: 0);
        Assert.Null(target);
    }

    [Fact]
    public void FingerprintIsOrderIndependentAndExcludesTheFingerprintKey()
    {
        var forward = new Dictionary<string, string>(StringComparer.Ordinal) { ["a"] = "1", ["b"] = "2" };
        var reversed = new Dictionary<string, string>(StringComparer.Ordinal) { ["b"] = "2", ["a"] = "1" };
        var withFingerprint = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["a"] = "1",
            ["b"] = "2",
            ["fingerprint"] = "should-not-matter",
        };

        var one = EnvironmentOrchestrator.Fingerprint(forward);
        Assert.Equal(one, EnvironmentOrchestrator.Fingerprint(reversed));
        Assert.Equal(one, EnvironmentOrchestrator.Fingerprint(withFingerprint));
    }

    [Theory]
    [InlineData("apply_rejected")]
    [InlineData("device_not_present")]
    [InlineData("unknown_property")]
    [InlineData("not_mutable")]
    public void SetupOutcomeTreatsAbsentHardwareCodesAsUnavailable(string code)
    {
        var outcome = new EnvironmentTransactionOutcome(
            null, RestoreResult.NotApplicable, string.Empty, string.Empty, string.Empty,
            code, "the desk cannot offer this", string.Empty);

        Assert.Equal(ScenarioOutcome.Unavailable, outcome.SetupOutcome().Outcome);
    }

    [Fact]
    public void SetupOutcomeTreatsEveryOtherCodeAsInfrastructureError()
    {
        var outcome = new EnvironmentTransactionOutcome(
            null, RestoreResult.NotApplicable, string.Empty, string.Empty, string.Empty,
            "mechanism_misbehaved", "the setter reported success and read back wrong", string.Empty);

        Assert.Equal(ScenarioOutcome.InfrastructureError, outcome.SetupOutcome().Outcome);
    }

    [Fact]
    public async Task ATransactionWithAnEmptyDesiredStateRunsTheBodyAndReportsNotApplicable()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-env-empty");
        var envctl = new FakeEnvctl();
        var orchestrator = await OpenAsync(envctl, directory, TestContext.Current.CancellationToken);

        var ran = false;
        var outcome = await orchestrator.RunAsync(
            "REL-TEST-000",
            new Dictionary<string, string>(StringComparer.Ordinal),
            (_, _) =>
            {
                ran = true;
                return Task.FromResult(ScenarioResult.Pass("nothing was mutated"));
            },
            TestContext.Current.CancellationToken);

        Assert.True(ran, "the body did not run for an empty desired state");
        Assert.Equal(RestoreResult.NotApplicable, outcome.Restore);
        Assert.Equal(ScenarioOutcome.Pass, outcome.Product?.Outcome);
        Assert.DoesNotContain("begin", envctl.Calls);
    }

    [Fact]
    public async Task ABodyThatThrowsStillRestoresAndTheOutcomeCarriesTheBodyError()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-env-throws");
        var envctl = new FakeEnvctl
        {
            BeginJson = """{"ok":true,"command":"begin","state":"Active","applied":[{"property":"p","from":"1","to":"2"}]}""",
            RestoreJson = """{"ok":true,"command":"restore","state":"Restored"}""",
        };
        var orchestrator = await OpenAsync(envctl, directory, TestContext.Current.CancellationToken);

        var outcome = await orchestrator.RunAsync(
            "REL-TEST-001",
            new Dictionary<string, string>(StringComparer.Ordinal) { ["alias:property"] = "2" },
            (_, _) => throw new InvalidOperationException("the fixture is inconsistent"),
            TestContext.Current.CancellationToken);

        Assert.Contains("the fixture is inconsistent", outcome.BodyError, StringComparison.Ordinal);
        Assert.Equal(RestoreResult.Restored, outcome.Restore);
        Assert.Contains("restore", envctl.Calls);
        Assert.True(
            envctl.Calls.IndexOf("begin:REL-TEST-001") < envctl.Calls.IndexOf("restore"),
            "restore did not run after begin");
    }

    [Fact]
    public async Task ABeginRefusedWithStateCleanReportsNotApplicableAndLeavesTheOrchestratorClean()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-env-refused-clean");
        var envctl = new FakeEnvctl
        {
            BeginJson = """{"ok":false,"command":"begin","state":"Clean","errorCode":"apply_rejected","error":"no such mode"}""",
        };
        var orchestrator = await OpenAsync(envctl, directory, TestContext.Current.CancellationToken);

        var outcome = await orchestrator.RunAsync(
            "REL-TEST-002",
            new Dictionary<string, string>(StringComparer.Ordinal) { ["alias:property"] = "2" },
            (_, _) => Task.FromResult(ScenarioResult.Pass("unreached")),
            TestContext.Current.CancellationToken);

        Assert.Equal(RestoreResult.NotApplicable, outcome.Restore);
        Assert.Equal("apply_rejected", outcome.SetupErrorCode);
        Assert.False(orchestrator.Dirty, "a begin refused while Clean must not mark the orchestrator dirty");
    }

    [Fact]
    public async Task ABeginRefusedWithAnyOtherStateMarksTheOrchestratorDirty()
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-env-refused-dirty");
        var envctl = new FakeEnvctl
        {
            BeginJson =
                """{"ok":false,"command":"begin","state":"RestorePending","errorCode":"apply_rejected","error":"rollback incomplete"}""",
        };
        var orchestrator = await OpenAsync(envctl, directory, TestContext.Current.CancellationToken);

        var outcome = await orchestrator.RunAsync(
            "REL-TEST-003",
            new Dictionary<string, string>(StringComparer.Ordinal) { ["alias:property"] = "2" },
            (_, _) => Task.FromResult(ScenarioResult.Pass("unreached")),
            TestContext.Current.CancellationToken);

        Assert.Equal(RestoreResult.RestorePending, outcome.Restore);
        Assert.True(orchestrator.Dirty, "a begin refused outside Clean/Restored must mark the orchestrator dirty");
    }
}
