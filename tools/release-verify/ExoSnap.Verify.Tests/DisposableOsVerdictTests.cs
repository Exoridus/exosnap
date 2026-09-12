using ExoSnap.Verify.Adapters.DisposableOs;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Telling a product defect apart from the harness failing to set up the test.
/// </summary>
/// <remarks>
/// The schema rules -- a missing required step, a duplicate, an unparseable
/// document -- are pinned by DisposableOsVerdictTests in DisposableOsTransportTests.
/// This class is only about the step KIND, which is what was missing.
///
/// A step with ok=false used to mean "ExoSnap is wrong" whatever the reason, so
/// "msiexec could not start because a staged dependency was missing" was reported
/// as a failing product gate -- the harness accusing the product of the harness's
/// own setup problem, on a gate that is required for promotion.
///
/// Steps now say which they are. A bootstrap failure built no environment, so
/// nothing downstream of it measured anything, and the verdict is Unverified
/// (INFRA_ERROR upstream) rather than Fail.
/// </remarks>
public sealed class DisposableOsBootstrapVerdictTests
{
    private static readonly string[] Required = ["install-base", "decline-offer", "decline-state"];

    private static DisposableOsStepResult Product(string name, bool ok) =>
        new(name, ok, ok ? "measured" : "assertion did not hold", DisposableOsStepKind.Product);

    private static DisposableOsStepResult Bootstrap(string name, bool ok) =>
        new(name, ok, ok ? "ready" : "could not be set up", DisposableOsStepKind.Bootstrap);

    [Fact]
    public void AFailedBootstrapStepIsNotAProductVerdict()
    {
        // The defect. install-base failing because msiexec could not start says
        // nothing about ExoSnap, and this gate is required for promotion -- so
        // calling it a product failure is the harness blaming the product for its
        // own missing dependency.
        var verdict = DisposableOsVerdict.From(
            new DisposableOsRunResult([
                new DisposableOsStepResult(
                    "install-base",
                    false,
                    "msiexec could not start because a staged dependency was missing",
                    DisposableOsStepKind.Bootstrap),
            ]),
            Required);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("could not be built", verdict.Message, StringComparison.Ordinal);
        Assert.Contains("msiexec", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ABootstrapFailureIsReportedEvenWhenAProductStepAlsoFailed()
    {
        // Order matters: a run that never got off the ground must not be read as a
        // defect just because a later step also reported not ok. Nothing downstream
        // of a failed bootstrap measured anything.
        var verdict = DisposableOsVerdict.From(
            new DisposableOsRunResult([
                Bootstrap("install-base", false),
                Product("decline-offer", false),
                Product("decline-state", false),
            ]),
            Required);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
    }

    [Fact]
    public void ABootstrapStepTheGateDoesNotRequireStillStopsTheRun()
    {
        // select-channel is not in this gate's required list, and every step after
        // it depends on the channel it was selecting.
        var verdict = DisposableOsVerdict.From(
            new DisposableOsRunResult([
                Bootstrap("install-base", true),
                Bootstrap("select-channel", false),
                Product("decline-offer", true),
                Product("decline-state", true),
            ]),
            Required);

        Assert.Equal(DisposableOsVerdictKind.Unverified, verdict.Kind);
        Assert.Contains("select-channel", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AStepWithNoKindIsReadAsAProductAssertion()
    {
        // The conservative default, and the compatibility rule for a document an
        // older worker wrote: it can fail a gate, but it can never hide a product
        // defect behind an infrastructure label.
        var verdict = DisposableOsVerdict.From(
            new DisposableOsRunResult([
                new DisposableOsStepResult("install-base", true, "ok"),
                new DisposableOsStepResult("decline-offer", true, "ok"),
                new DisposableOsStepResult("decline-state", false, "assertion did not hold"),
            ]),
            Required);

        Assert.Equal(DisposableOsVerdictKind.Fail, verdict.Kind);
    }

    [Fact]
    public void AnOlderDocumentWithoutKindStillParses()
    {
        // The worker's document is written by a script; a field this build added is
        // simply absent in one an older build wrote.
        var parsed = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"install-base","ok":true,"detail":"ok"}]}""");

        Assert.NotNull(parsed);
        var step = Assert.Single(parsed.Steps);
        Assert.Equal(DisposableOsStepKind.Product, step.Kind);
        Assert.True(step.IsProductAssertion);
    }

    [Fact]
    public void ADocumentDeclaringBootstrapParsesAsBootstrap()
    {
        var parsed = DisposableOsRunResult.Parse(
            """{"steps":[{"name":"install-base","ok":false,"detail":"no msiexec","kind":"bootstrap"}]}""");

        Assert.NotNull(parsed);
        Assert.False(Assert.Single(parsed.Steps).IsProductAssertion);
    }
}

/// <summary>How completely a worker's evidence reached the host.</summary>
/// <remarks>
/// The collection swallowed IOException and UnauthorizedAccessException and
/// returned, and the caller then deleted the staging directory -- so a failed copy
/// destroyed the only source and nothing anywhere said so.
/// </remarks>
public sealed class EvidenceOutcomeTests
{
    [Fact]
    public void NotRequestedAndCompleteAreBothComplete()
    {
        Assert.True(EvidenceOutcome.NotRequested.IsComplete);
        Assert.True(new EvidenceOutcome(EvidenceOutcomeState.Complete, 3, 0, "3 collected").IsComplete);
    }

    [Fact]
    public void PartialAndFailedAreNotComplete()
    {
        // A caller whose evidence is contractually required cannot treat either as
        // a fully answered run, whatever the product verdict was.
        Assert.False(new EvidenceOutcome(EvidenceOutcomeState.Partial, 2, 1, "one lost").IsComplete);
        Assert.False(new EvidenceOutcome(EvidenceOutcomeState.Failed, 0, 4, "all lost").IsComplete);
    }

    [Fact]
    public void MissingIsItsOwnStateAndNotACopyFailure()
    {
        // The worker declared an evidence directory and wrote none. Nothing failed
        // to copy; there was nothing to copy, which is a different thing to report.
        var missing = new EvidenceOutcome(EvidenceOutcomeState.Missing, 0, 0, "the worker wrote none");
        Assert.False(missing.IsComplete);
        Assert.Equal(0, missing.FilesFailed);
    }

    [Fact]
    public void ARunCarriesItsEvidenceOutcome()
    {
        // So a record can tell a product verdict apart from whether it can be
        // looked into at all.
        var run = DisposableOsRun.Completed(new DisposableOsRunResult([]))
            with { Evidence = new EvidenceOutcome(EvidenceOutcomeState.Failed, 0, 2, "both lost") };

        Assert.Equal(DisposableOsRunKind.Completed, run.Kind);
        Assert.False(run.Evidence.IsComplete);
        Assert.Equal(2, run.Evidence.FilesFailed);
    }

    [Fact]
    public void ARunWithoutAnExplicitOutcomeAsksForNothing()
    {
        Assert.Equal(EvidenceOutcomeState.NotRequested, DisposableOsRun.Unavailable("no transport").Evidence.State);
    }
}
