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
}
