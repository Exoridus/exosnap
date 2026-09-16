using ExoSnap.Verify.Adapters.DisposableOs;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// A product pass whose evidence was destroyed does not qualify a release.
/// </summary>
/// <remarks>
/// Taken through the real path, not by constructing the models: gate body, then the
/// verdict the engine records, then the promotion decision. Every layer of this
/// looked correct on its own while the information was lost between two of them --
/// the transport has reported an evidence outcome since the collection stopped
/// swallowing failures, and for three packages nothing read it, so a run whose logs
/// were gone reached the record as an ordinary pass.
/// </remarks>
public sealed class EvidenceReachesTheRecordTests : IDisposable
{
    private readonly string baseMsi = Path.Combine(
        Path.GetTempPath(), "exosnap-evidence-base-" + Guid.NewGuid().ToString("N") + ".msi");

    public EvidenceReachesTheRecordTests() => File.WriteAllText(this.baseMsi, "older release");

    public void Dispose() => File.Delete(this.baseMsi);

    private static DisposableOsRunResult AllStepsPassed => new(
    [
        new("install-base", true, "installed", DisposableOsStepKind.Bootstrap),
        new("decline-offer", true, "offered", DisposableOsStepKind.Product),
        new("decline-apply", true, "applied", DisposableOsStepKind.Product),
        new("decline-state", true, "failureCase uacDeclined, installState intact", DisposableOsStepKind.Product),
    ]);

    [Fact]
    public async Task AGateWhoseEvidenceWasLostStillReportsTheProductVerdictItMeasured()
    {
        // The first half of the separation: the assertions really did hold, and
        // turning that into a failure would accuse the product of the harness losing
        // a file.
        var result = await this.RunGateAsync(
            DisposableOsRun.Completed(AllStepsPassed)
                with { Evidence = new EvidenceOutcome(EvidenceOutcomeState.Failed, 0, 2, "both logs lost") });

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
    }

    [Fact]
    public async Task AndSaysTheEvidenceDidNotReachTheHost()
    {
        var result = await this.RunGateAsync(
            DisposableOsRun.Completed(AllStepsPassed)
                with { Evidence = new EvidenceOutcome(EvidenceOutcomeState.Failed, 0, 2, "both logs lost") });

        Assert.False(result.EvidenceComplete);
        Assert.Contains("both logs lost", result.EvidenceGap, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ARunWhoseEvidenceArrivedSaysNothingAboutIt()
    {
        var result = await this.RunGateAsync(
            DisposableOsRun.Completed(AllStepsPassed)
                with { Evidence = new EvidenceOutcome(EvidenceOutcomeState.Complete, 3, 0, "3 collected") });

        Assert.Equal(ScenarioOutcome.Pass, result.Outcome);
        Assert.True(result.EvidenceComplete);
    }

    [Fact]
    public async Task AWorkerThatProducedNoEvidenceAtAllIsAlsoAGap()
    {
        // Missing is not a copy failure and it is not nothing either: a gate that
        // expected evidence has none, and the record cannot be looked into.
        var result = await this.RunGateAsync(
            DisposableOsRun.Completed(AllStepsPassed)
                with { Evidence = new EvidenceOutcome(EvidenceOutcomeState.Missing, 0, 0, "the worker wrote none") });

        Assert.False(result.EvidenceComplete);
    }

    [Fact]
    public void ThePromotionContractRefusesAPassNobodyCanLookInto()
    {
        // The end of the path. The verdict is a pass, the scenario is required, and
        // the release is still not qualified.
        var verdict = new ScenarioVerdict("REL-UPD-MSI-DECLINE-001", ScenarioOutcome.Pass, "all four steps", 10, [])
        {
            EvidenceGap = "2 evidence file(s) could not be collected",
        };

        var objections = Qualification.Objections([verdict], ["REL-UPD-MSI-DECLINE-001"], []);

        Assert.Contains(
            objections,
            objection => objection.Contains("did not reach the record", StringComparison.Ordinal));
    }

    [Fact]
    public void ThatRefusalIsNotReportedAsAProductFailure()
    {
        // The wording matters: whoever reads the record has to be able to tell "the
        // product is wrong" from "we cannot show that it is right".
        var verdict = new ScenarioVerdict("REL-UPD-MSI-DECLINE-001", ScenarioOutcome.Pass, "all four steps", 10, [])
        {
            EvidenceGap = "2 evidence file(s) could not be collected",
        };

        var objection = Assert.Single(Qualification.Objections([verdict], ["REL-UPD-MSI-DECLINE-001"], []));

        Assert.DoesNotContain("product failure", objection, StringComparison.Ordinal);
        Assert.Contains("passed", objection, StringComparison.Ordinal);
    }

    [Fact]
    public void ACompleteRunQualifies()
    {
        // So the rule above is not vacuously satisfied by everything failing.
        var verdict = new ScenarioVerdict("REL-UPD-MSI-DECLINE-001", ScenarioOutcome.Pass, "all four steps", 10, []);

        Assert.Empty(Qualification.Objections([verdict], ["REL-UPD-MSI-DECLINE-001"], []));
    }

    [Fact]
    public void AGapOnAScenarioThatDidNotPassIsNotADoubleObjection()
    {
        // A failing scenario is already an objection, and reporting its missing logs
        // as a second one would say the same run was wrong twice.
        var verdict = new ScenarioVerdict("REL-UPD-MSI-DECLINE-001", ScenarioOutcome.Fail, "decline-state", 10, [])
        {
            EvidenceGap = "2 evidence file(s) could not be collected",
        };

        Assert.Single(Qualification.Objections([verdict], ["REL-UPD-MSI-DECLINE-001"], []));
    }

    /// <summary>Runs the real update gate over a transport that returns one prepared run.</summary>
    private async Task<ScenarioResult> RunGateAsync(DisposableOsRun run)
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-UPD-MSI-DECLINE-001",
            fakes =>
            {
                DisposableOsGateFixture.StageWorker(fakes);
                fakes.DisposableOs.Run = run;
            },
            TestContext.Current.CancellationToken);

        return await new UpdateDeclineGate(() => this.baseMsi).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);
    }
}
