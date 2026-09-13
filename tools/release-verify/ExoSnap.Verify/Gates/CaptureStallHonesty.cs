using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>What the product said about a stalled capture, and what it measured.</summary>
/// <param name="StandingNotice">A capture-stall notification is standing.</param>
/// <param name="NoticeText">Its title and body, which the honesty rule reads.</param>
/// <param name="Lifecycle">The recording lifecycle at the moment of the reading.</param>
/// <param name="MeasuredMode">The present mode actually measured, or an empty string.</param>
/// <param name="ModeAvailability">Why the mode is what it is, for the message.</param>
public sealed record StallObservation(
    bool StandingNotice,
    string NoticeText,
    string Lifecycle,
    string MeasuredMode,
    string ModeAvailability);

/// <summary>
/// Whether a stalled window capture was reported honestly and stayed controllable.
/// </summary>
/// <remarks>
/// Three separate things, and the third is the one worth having. The notice has to
/// appear; the recording has to keep running, because a stall must not stop it; and
/// an explanation that blames exclusive fullscreen is only allowed when a present
/// mode was actually measured as exclusive fullscreen. A product that guessed the
/// explanation from window shape would be right often enough to look correct and
/// would be telling the user something nobody checked.
/// </remarks>
public static class CaptureStallHonesty
{
    private static readonly string[] RunningLifecycles = ["recording", "paused"];

    /// <summary>The present mode this product calls true exclusive fullscreen.</summary>
    public const string ExclusiveFullscreen = "exclusiveFullscreen";

    /// <summary>Whether a notice text blames exclusive fullscreen for the stall.</summary>
    public static bool ClaimsExclusiveFullscreen(string noticeText) =>
        !string.IsNullOrEmpty(noticeText)
        && (noticeText.Contains("exclusive", StringComparison.OrdinalIgnoreCase)
            || noticeText.Contains("fullscreen", StringComparison.OrdinalIgnoreCase));

    /// <summary>The verdict for one reading of a stalled capture.</summary>
    public static ScenarioResult Verdict(StallObservation observation, IReadOnlyList<Evidence> evidence)
    {
        ArgumentNullException.ThrowIfNull(observation);
        ArgumentNullException.ThrowIfNull(evidence);

        if (!observation.StandingNotice)
        {
            return ScenarioResult.Fail("no standing capture-stall notification appeared", [.. evidence]);
        }

        if (!RunningLifecycles.Contains(observation.Lifecycle, StringComparer.Ordinal))
        {
            return ScenarioResult.Fail(
                $"the recording left the running lifecycle ({observation.Lifecycle}); a stall must not stop it",
                [.. evidence]);
        }

        var claims = ClaimsExclusiveFullscreen(observation.NoticeText);
        var measured = string.Equals(observation.MeasuredMode, ExclusiveFullscreen, StringComparison.Ordinal);
        if (claims && !measured)
        {
            var mode = string.IsNullOrEmpty(observation.MeasuredMode) ? "not measured" : observation.MeasuredMode;
            return ScenarioResult.Fail(
                $"the stall notice blames exclusive fullscreen but the present mode was '{mode}' "
                + $"(availability {observation.ModeAvailability})",
                [.. evidence]);
        }

        return ScenarioResult.Pass(
            $"standing stall notice, lifecycle {observation.Lifecycle}, "
            + $"fseClaim={claims} fseMeasured={measured}",
            [.. evidence]);
    }
}

/// <summary>What a present measurement says about true exclusive fullscreen.</summary>
/// <param name="Available">Present diagnostics answered at all.</param>
/// <param name="Availability">Why they did or did not, for the message.</param>
/// <param name="Mode">The measured present mode.</param>
/// <param name="PresentCount">How many presents the measurement covers.</param>
public sealed record PresentMeasurement(bool Available, string Availability, string Mode, double PresentCount);

/// <summary>
/// Whether true exclusive fullscreen was detected, and only when it was measured.
/// </summary>
/// <remarks>
/// The separation the gate exists for: a borderless window covering a monitor looks
/// the same to a person and is a completely different capture path. So window shape
/// decides nothing here -- only a present measurement does, and a run that has none
/// is unavailable rather than passing on a guess.
/// </remarks>
public static class ExclusiveFullscreenDetection
{
    /// <summary>The verdict for one present measurement and an optional oracle reading.</summary>
    /// <param name="measurement">What the product measured.</param>
    /// <param name="oracleDisagreement">
    /// Why an independent decoder disagreed, or an empty string when it agreed or was
    /// not consulted. A disagreement fails: both read the same ETW events, so one of
    /// them is wrong about the most consequential capture path the product has.
    /// </param>
    /// <param name="evidence">What was saved for the record.</param>
    public static ScenarioResult Verdict(
        PresentMeasurement measurement, string oracleDisagreement, IReadOnlyList<Evidence> evidence)
    {
        ArgumentNullException.ThrowIfNull(measurement);
        ArgumentNullException.ThrowIfNull(evidence);

        if (!measurement.Available)
        {
            // An unmet precondition, never a failure earned by a scenario nobody could
            // have passed: with the opt-in off or the session unelevated, no probe can
            // make this measurable.
            return ScenarioResult.Unavailable(
                $"present diagnostics are unavailable ({measurement.Availability}); run the elevated present "
                + "scenario first");
        }

        if (!string.Equals(measurement.Mode, CaptureStallHonesty.ExclusiveFullscreen, StringComparison.Ordinal))
        {
            return ScenarioResult.Fail(
                $"the present mode is '{measurement.Mode}', not {CaptureStallHonesty.ExclusiveFullscreen}",
                [.. evidence]);
        }

        if (!string.IsNullOrEmpty(oracleDisagreement))
        {
            return ScenarioResult.Fail(oracleDisagreement, [.. evidence]);
        }

        return ScenarioResult.Pass(
            $"present mode {CaptureStallHonesty.ExclusiveFullscreen} over "
            + $"{measurement.PresentCount:0} presents",
            [.. evidence]);
    }
}
