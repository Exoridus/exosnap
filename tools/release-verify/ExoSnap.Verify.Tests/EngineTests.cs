using System.Collections.ObjectModel;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Processes;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// How the engine turns a catalog and a machine into verdicts, and what it
/// refuses to call a product failure.
/// </summary>
public sealed class EngineTests
{
    private static CapabilitySet SetOf(params (string Key, string Value)[] values) =>
        new(
            values.ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.OrdinalIgnoreCase),
            values.ToDictionary(pair => pair.Key, _ => "test", StringComparer.OrdinalIgnoreCase));

    private static ScenarioDescriptor Descriptor(
        string id,
        IReadOnlyList<CapabilityRequirement>? requires = null,
        IReadOnlyList<string>? dependsOn = null,
        bool optIn = false,
        string scenarioClass = "test") =>
        new(
            id,
            $"{id} title",
            scenarioClass,
            ScenarioLayer.FullAuto,
            new ReadOnlyCollection<CapabilityRequirement>([.. requires ?? []]),
            ScenarioIsolation.Hermetic,
            ScenarioPrivilege.Standard,
            ScenarioInteraction.Automated,
            new ReadOnlyCollection<string>([]),
            new ReadOnlyCollection<string>(["test"]),
            optIn,
            "tests",
            new ReadOnlyCollection<string>([.. dependsOn ?? []]));

    private sealed class FixedBody(ScenarioResult result) : IScenarioBody
    {
        public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken) =>
            Task.FromResult(result);
    }

    private sealed class ThrowingBody : IScenarioBody
    {
        public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken) =>
            throw new InvalidOperationException("the fixture is inconsistent");
    }

    private static async Task<ReadOnlyCollection<ScenarioVerdict>> RunAsync(
        ScenarioCatalog catalog,
        CapabilitySet capabilities,
        ScenarioSelection? selection = null)
    {
        using var directory = FixtureTool.NewTemporaryDirectory("-run");
        using var processes = new ProcessRunner();
        var engine = new VerifyEngine(catalog);
        var plan = engine.Plan(selection ?? new ScenarioSelection(IncludeOptIn: true), capabilities);
        return await engine.RunAsync(
            plan,
            capabilities,
            RunDirectory.Open(directory.Path),
            processes,
            TestContext.Current.CancellationToken);
    }

    [Fact]
    public void AnUnsatisfiedRequirementYieldsUnavailableWithTheCapabilityMessage()
    {
        var catalog = new ScenarioCatalog(
        [
            new Scenario(
                Descriptor("A", requires: [CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true")]),
                new FixedBody(ScenarioResult.Pass("would not get here"))),
        ]);

        var plan = new VerifyEngine(catalog).Plan(
            new ScenarioSelection(IncludeOptIn: true),
            SetOf((CapabilityKeys.DisplayHdr, "false")));

        Assert.Equal(ScenarioOutcome.Unavailable, plan.Scenarios[0].Outcome);
        Assert.Equal("capability display.hdr=true not satisfied", plan.Scenarios[0].Message);
        Assert.False(plan.Scenarios[0].WouldRun);
    }

    [Fact]
    public async Task AnEvidenceGapOnAResultReachesTheVerdictTheEngineRecords()
    {
        // The join between two layers that each look correct on their own. A gate can
        // report that its evidence never arrived and the promotion contract can refuse
        // such a pass, and the release still qualifies if the engine drops the fact in
        // between -- which is how the transport's evidence outcome went unread for
        // three packages after it started being reported.
        var catalog = new ScenarioCatalog(
        [
            new Scenario(
                Descriptor("A"),
                new FixedBody(ScenarioResult.Pass("the product did what it was asked")
                    with { EvidenceGap = "2 evidence file(s) could not be collected" })),
        ]);

        var verdicts = await RunAsync(catalog, SetOf());

        Assert.Equal(ScenarioOutcome.Pass, verdicts[0].Outcome);
        Assert.Equal("2 evidence file(s) could not be collected", verdicts[0].EvidenceGap);

        // And through to the end of the path, on the verdict the engine produced
        // rather than one written by hand here.
        Assert.Contains(
            Qualification.Objections(verdicts, ["A"], []),
            objection => objection.Contains("did not reach the record", StringComparison.Ordinal));
    }

    [Fact]
    public async Task AResultWithNoEvidenceGapRecordsNone()
    {
        var catalog = new ScenarioCatalog([new Scenario(Descriptor("A"), new FixedBody(ScenarioResult.Pass("ok")))]);
        var verdicts = await RunAsync(catalog, SetOf());

        Assert.Empty(verdicts[0].EvidenceGap);
        Assert.Empty(Qualification.Objections(verdicts, ["A"], []));
    }

    [Fact]
    public async Task AnExceptionInAScenarioBodyBecomesAnInfrastructureErrorNotAFailure()
    {
        var catalog = new ScenarioCatalog([new Scenario(Descriptor("A"), new ThrowingBody())]);
        var verdicts = await RunAsync(catalog, SetOf());

        Assert.Equal(ScenarioOutcome.InfrastructureError, verdicts[0].Outcome);
        Assert.Contains("InvalidOperationException", verdicts[0].Message, StringComparison.Ordinal);
        Assert.DoesNotContain(verdicts, verdict => verdict.Outcome == ScenarioOutcome.Fail);
    }

    [Fact]
    public async Task ADependentIsBlockedWhenItsDependencyDoesNotPass()
    {
        var catalog = new ScenarioCatalog(
        [
            new Scenario(Descriptor("A"), new FixedBody(ScenarioResult.Fail("the product is wrong"))),
            new Scenario(Descriptor("B", dependsOn: ["A"]), new FixedBody(ScenarioResult.Pass("unreached"))),
        ]);

        var verdicts = await RunAsync(catalog, SetOf());

        Assert.Equal(ScenarioOutcome.Fail, verdicts[0].Outcome);
        Assert.Equal(ScenarioOutcome.Blocked, verdicts[1].Outcome);
        Assert.Contains("depends on A", verdicts[1].Message, StringComparison.Ordinal);
    }

    [Fact]
    public void OptInScenariosAreSkippedUnlessAskedFor()
    {
        var catalog = new ScenarioCatalog(
        [
            new Scenario(Descriptor("A"), new FixedBody(ScenarioResult.Pass("ok"))),
            new Scenario(Descriptor("B", optIn: true), new FixedBody(ScenarioResult.Pass("ok"))),
        ]);

        var plan = new VerifyEngine(catalog).Plan(new ScenarioSelection(), SetOf());

        Assert.True(plan.Scenarios[0].WouldRun);
        Assert.Equal(ScenarioOutcome.Skipped, plan.Scenarios[1].Outcome);
        Assert.Contains("opt-in", plan.Scenarios[1].Message, StringComparison.Ordinal);
    }

    [Fact]
    public void SelectingByClassExcludesEverythingElse()
    {
        var catalog = new ScenarioCatalog(
        [
            new Scenario(Descriptor("A", scenarioClass: "capture"), new FixedBody(ScenarioResult.Pass("ok"))),
            new Scenario(Descriptor("B", scenarioClass: "audio"), new FixedBody(ScenarioResult.Pass("ok"))),
        ]);

        var plan = new VerifyEngine(catalog).Plan(new ScenarioSelection(Classes: ["capture"]), SetOf());

        Assert.True(plan.Scenarios[0].WouldRun);
        Assert.Equal(ScenarioOutcome.Skipped, plan.Scenarios[1].Outcome);
    }

    [Fact]
    public async Task AScenarioBodyGetsItsOwnEvidenceDirectory()
    {
        string? captured = null;
        var existed = false;
        var catalog = new ScenarioCatalog(
        [
            new Scenario(
                Descriptor("REL-TEST-001"),
                new CapturingBody(path =>
                {
                    captured = path;
                    existed = Directory.Exists(path);
                })),
        ]);

        await RunAsync(catalog, SetOf());

        Assert.NotNull(captured);
        Assert.EndsWith(Path.Combine(RunDirectory.EvidenceFolderName, "REL-TEST-001"), captured, StringComparison.Ordinal);
        Assert.True(existed, "the evidence directory did not exist while the scenario was running");
    }

    private sealed class CapturingBody(Action<string> capture) : IScenarioBody
    {
        public Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
        {
            capture(context.EvidenceDirectory);
            return Task.FromResult(ScenarioResult.Pass("captured"));
        }
    }

    [Fact]
    public void ARecordedVerdictGoesStaleWhenTheArtifactsChange()
    {
        var state = new RunState(
            RunState.CurrentSchemaVersion,
            "run-1",
            "v0.9.1-rc1",
            "abc123",
            "artifact-aaaa",
            "catalog-bbbb",
            DateTimeOffset.UtcNow,
            new ReadOnlyCollection<ScenarioVerdict>(
            [
                new ScenarioVerdict("A", ScenarioOutcome.Pass, "ok", 1, new ReadOnlyCollection<Evidence>([])),
            ]));

        Assert.Equal(ScenarioOutcome.Pass, state.VerdictsFor("artifact-aaaa", "catalog-bbbb")[0].Outcome);

        var afterRebuild = state.VerdictsFor("artifact-cccc", "catalog-bbbb")[0];
        Assert.Equal(ScenarioOutcome.Stale, afterRebuild.Outcome);
        Assert.Contains("artifacts changed", afterRebuild.Message, StringComparison.Ordinal);

        var afterCatalogEdit = state.VerdictsFor("artifact-aaaa", "catalog-dddd")[0];
        Assert.Equal(ScenarioOutcome.Stale, afterCatalogEdit.Outcome);
        Assert.Contains("catalog changed", afterCatalogEdit.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ARunDirectoryRoundTripsItsState()
    {
        using var temporary = FixtureTool.NewTemporaryDirectory("-state");
        var directory = RunDirectory.Open(temporary.Path);

        Assert.Null(directory.ReadState());

        var state = new RunState(
            RunState.CurrentSchemaVersion,
            "run-2",
            "v0.9.1-rc1",
            "abc123",
            "artifact-aaaa",
            "catalog-bbbb",
            DateTimeOffset.UtcNow,
            new ReadOnlyCollection<ScenarioVerdict>([]));

        directory.WriteState(state);
        var read = directory.ReadState();

        Assert.NotNull(read);
        Assert.Equal(state.RunId, read.RunId);
        Assert.Equal(state.ArtifactFingerprint, read.ArtifactFingerprint);
    }
}
