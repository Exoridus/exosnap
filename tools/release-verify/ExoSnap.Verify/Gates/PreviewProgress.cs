using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>Whether the preview was presenting while it was watched.</summary>
/// <param name="Live">A frame was consumed during the window.</param>
/// <param name="Message">What was observed, in one sentence.</param>
public sealed record PreviewObservation(bool Live, string Message);

/// <summary>
/// Reads the preview's own bookkeeping and decides whether it is presenting.
/// </summary>
/// <remarks>
/// <c>updateGate.owed</c> is "published newer than presented", which a healthy preview
/// crosses between every publish and the render pass that follows it. It is therefore
/// not a verdict on its own: one sample fails a live preview at whatever share of the
/// frame period that window occupies, and passes a preview that has published nothing
/// at all.
///
/// Progress is what separates the two. A preview that keeps consuming frames is
/// presenting them whatever a single instant of the debt flag says; a standing debt
/// with no consumed frame is the stall this is written for; and a preview that never
/// published anything proves nothing either way.
/// </remarks>
public static class PreviewProgress
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(200);

    /// <summary>Waits until the preview has consumed a new frame, or the window expires.</summary>
    public static async Task<PreviewObservation> WatchAsync(
        ILiveVerifySession session,
        TimeSpan window,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(session);

        var snapshot = await session.PreviewSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var baseline = Snapshots.Number(snapshot.Result, "consumedFrames");
        if (baseline is null)
        {
            return new PreviewObservation(false, "the preview snapshot carries no consumedFrames");
        }

        var deadline = DateTime.UtcNow + window;
        var current = baseline;
        while (DateTime.UtcNow < deadline)
        {
            await Task.Delay(PollInterval, cancellationToken).ConfigureAwait(false);
            snapshot = await session.PreviewSnapshotAsync(cancellationToken).ConfigureAwait(false);
            current = Snapshots.Number(snapshot.Result, "consumedFrames");
            if (current is not null && current.Value > baseline.Value)
            {
                return new PreviewObservation(
                    true, $"consumed {Format(baseline.Value)} -> {Format(current.Value)}");
            }
        }

        var status = Snapshots.Text(snapshot.Result, "statusText");
        return new PreviewObservation(
            false, $"consumedFrames stayed at {Format(baseline.Value)}; status '{status}'");
    }

    /// <summary>
    /// Decides from a series of preview snapshots whether the preview is frozen.
    /// </summary>
    /// <remarks>
    /// Three answers. A preview that kept consuming frames passes. A standing publish
    /// debt across every sample with no consumed frame is the frozen preview and
    /// fails. A window in which the preview consumed nothing and owed nothing
    /// exercised no presentation at all, which is an infrastructure error rather than
    /// a verdict about the product.
    /// </remarks>
    public static ScenarioResult FreezeVerdict(IReadOnlyList<JsonElement> samples)
    {
        ArgumentNullException.ThrowIfNull(samples);

        if (samples.Count < 2)
        {
            return ScenarioResult.InfrastructureError(
                "a transient publish debt needs at least two observations");
        }

        var first = samples[0];
        var last = samples[^1];

        var consumedFrom = Snapshots.Number(first, "consumedFrames");
        var consumedTo = Snapshots.Number(last, "consumedFrames");
        var rendersFrom = Snapshots.Number(first, "updateGate.renderPasses");
        var rendersTo = Snapshots.Number(last, "updateGate.renderPasses");

        if (consumedFrom is null || consumedTo is null || rendersTo is null)
        {
            return ScenarioResult.InfrastructureError(
                "the preview snapshots carry no consumedFrames/updateGate counters");
        }

        var owedThroughout = samples.All(sample => Snapshots.IsTrue(sample, "updateGate.owed"));
        var renders = $"renders {Format(rendersFrom)} -> {Format(rendersTo)}";

        if (consumedTo.Value > consumedFrom.Value)
        {
            return ScenarioResult.Pass(
                $"preview kept presenting: consumed {Format(consumedFrom)} -> {Format(consumedTo)}, {renders}");
        }

        if (owedThroughout)
        {
            return ScenarioResult.Fail(
                $"a published preview frame stayed unrendered across {samples.Count.ToString(CultureInfo.InvariantCulture)} samples " +
                $"(frozen preview); consumed stuck at {Format(consumedTo)}, {renders}");
        }

        return ScenarioResult.InfrastructureError(
            $"the preview consumed no frame during the window (consumed {Format(consumedTo)}), " +
            "so presenting one was never exercised");
    }

    private static string Format(double? value) =>
        value is null ? "(not reported)" : value.Value.ToString("0.###", CultureInfo.InvariantCulture);
}
