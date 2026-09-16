using ExoSnap.Verify.Catalog;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Whether a stalled window capture was reported honestly and stayed controllable.
/// </summary>
/// <remarks>
/// The third assertion is the one worth having. A product that guessed the
/// explanation from window shape would be right often enough to look correct while
/// telling the user something nobody measured, so an explanation blaming exclusive
/// fullscreen is only accepted when a present mode was actually measured as one.
/// </remarks>
public sealed class CaptureStallVerdictTests
{
    private static readonly Evidence[] None = [];

    private static StallObservation Observed(
        bool notice = true,
        string text = "Window capture appears to have stalled",
        string lifecycle = "recording",
        string mode = "composed",
        string availability = "measured") =>
        new(notice, text, lifecycle, mode, availability);

    [Fact]
    public void AStandingNoticeOnARunningRecordingPasses()
    {
        Assert.Equal(ScenarioOutcome.Pass, CaptureStallHonesty.Verdict(Observed(), None).Outcome);
    }

    [Fact]
    public void APausedRecordingIsStillRunning()
    {
        // Pausing is a control the user still has, so it is not the recording dying.
        Assert.Equal(
            ScenarioOutcome.Pass,
            CaptureStallHonesty.Verdict(Observed(lifecycle: "paused"), None).Outcome);
    }

    [Fact]
    public void NoNoticeAtAllFails()
    {
        var verdict = CaptureStallHonesty.Verdict(Observed(notice: false), None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("no standing capture-stall notification", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ARecordingThatStoppedFails()
    {
        var verdict = CaptureStallHonesty.Verdict(Observed(lifecycle: "failed"), None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("must not stop it", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void BlamingExclusiveFullscreenWithoutMeasuringItFails()
    {
        // The honesty rule. The notice is standing and the recording is alive, and the
        // gate still fails, because the explanation was not measured.
        var verdict = CaptureStallHonesty.Verdict(
            Observed(text: "Capture stalled: the window is in exclusive fullscreen", mode: "composed"),
            None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("blames exclusive fullscreen", verdict.Message, StringComparison.Ordinal);
        Assert.Contains("composed", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void BlamingExclusiveFullscreenAfterMeasuringItPasses()
    {
        var verdict = CaptureStallHonesty.Verdict(
            Observed(text: "Capture stalled: exclusive fullscreen", mode: "exclusiveFullscreen"),
            None);

        Assert.Equal(ScenarioOutcome.Pass, verdict.Outcome);
        Assert.Contains("fseMeasured=True", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AnUnmeasuredModeIsNamedAsUnmeasuredRatherThanAsEmpty()
    {
        // The present group is absent entirely while the measurement is invalid, so an
        // empty mode is the ordinary case and has to read as one.
        var verdict = CaptureStallHonesty.Verdict(
            Observed(text: "stalled: exclusive fullscreen", mode: "", availability: "not_elevated"),
            None);

        Assert.Contains("not measured", verdict.Message, StringComparison.Ordinal);
        Assert.Contains("not_elevated", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ANoticeThatExplainsNothingIsNotHeldToTheFseRule()
    {
        // Only an explanation can be dishonest. A plain stall notice claims nothing
        // about why, and requiring a measurement for it would fail every correct run.
        Assert.Equal(
            ScenarioOutcome.Pass,
            CaptureStallHonesty.Verdict(Observed(text: "Window capture stalled", mode: ""), None).Outcome);
    }
}

/// <summary>
/// Detecting true exclusive fullscreen, and only when it was measured.
/// </summary>
/// <remarks>
/// A borderless window covering a monitor looks the same to a person and is a
/// completely different capture path, so window shape decides nothing: only a present
/// measurement does. A run with none is unavailable, never a pass.
/// </remarks>
public sealed class ExclusiveFullscreenVerdictTests
{
    private static readonly Evidence[] None = [];

    [Fact]
    public void AMeasuredExclusiveFullscreenPresentPasses()
    {
        var verdict = ExclusiveFullscreenDetection.Verdict(
            new PresentMeasurement(true, "measured", "exclusiveFullscreen", 312), string.Empty, None);

        Assert.Equal(ScenarioOutcome.Pass, verdict.Outcome);
        Assert.Contains("312", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void WithoutPresentDiagnosticsTheScenarioIsUnavailableAndNotAPass()
    {
        // The rule the ticket names: unavailable is not pass. With the opt-in off or
        // the session unelevated, no probe can make this measurable, and a scenario
        // nobody could have passed must not be failed either.
        var verdict = ExclusiveFullscreenDetection.Verdict(
            new PresentMeasurement(false, "not_elevated", string.Empty, 0), string.Empty, None);

        Assert.Equal(ScenarioOutcome.Unavailable, verdict.Outcome);
        Assert.Contains("not_elevated", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AComposedPresentIsNotExclusiveFullscreen()
    {
        // A borderless window. It is the case this gate exists to tell apart, and it
        // is a failure of the detection rather than an unavailable machine.
        var verdict = ExclusiveFullscreenDetection.Verdict(
            new PresentMeasurement(true, "measured", "composed", 400), string.Empty, None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("composed", verdict.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AnOracleThatDisagreesFailsEvenWhenOurOwnModeIsRight()
    {
        // Both decoders read the same ETW events, so a disagreement means one of them
        // is wrong about the most consequential capture path the product has.
        var verdict = ExclusiveFullscreenDetection.Verdict(
            new PresentMeasurement(true, "measured", "exclusiveFullscreen", 500),
            "PresentMon classified the same window as Composed: Flip",
            None);

        Assert.Equal(ScenarioOutcome.Fail, verdict.Outcome);
        Assert.Contains("PresentMon", verdict.Message, StringComparison.Ordinal);
    }
}

/// <summary>Which capture scenarios now carry an executable body.</summary>
public sealed class CaptureGateMigrationTests
{
    [Theory]
    [InlineData("REL-CAP-STALL-001")]
    [InlineData("REL-CAP-FSE-001")]
    public void TheTwoRemainingCaptureScenariosAreMigrated(string id)
    {
        Assert.Contains(id, ReleaseCatalog.MigratedIds());
    }
}
