using System.Collections.ObjectModel;
using System.Globalization;

namespace ExoSnap.Verify.Analysis;

/// <summary>The changed region of one frame relative to the baseline.</summary>
/// <param name="FrameIndex">Zero-based frame index.</param>
/// <param name="ChangedPixels">How many pixels differ from the baseline by at least the threshold.</param>
/// <param name="MinX">Left edge of the changed region, or -1 when nothing changed.</param>
/// <param name="MinY">Top edge, or -1.</param>
/// <param name="MaxX">Right edge, or -1.</param>
/// <param name="MaxY">Bottom edge, or -1.</param>
/// <param name="CentroidX">Horizontal centroid of the changed pixels, or NaN when nothing changed.</param>
/// <param name="CentroidY">Vertical centroid, or NaN.</param>
public sealed record ChangedRegion(
    int FrameIndex,
    int ChangedPixels,
    int MinX,
    int MinY,
    int MaxX,
    int MaxY,
    double CentroidX,
    double CentroidY)
{
    /// <summary>Whether anything changed at all in this frame.</summary>
    public bool IsEmpty => this.ChangedPixels == 0;
}

/// <summary>Why a baseline could not be trusted as "the scene without the thing under test".</summary>
public enum BaselineDefect
{
    /// <summary>The requested baseline range contains no frames.</summary>
    Empty,

    /// <summary>
    /// The baseline frames differ from each other, so the scene was already moving while
    /// the reference was being averaged.
    /// </summary>
    Unstable,
}

/// <summary>A baseline frame and what is wrong with it.</summary>
/// <param name="Defect">What is wrong, or null when nothing is.</param>
/// <param name="Detail">The measured values, phrased for a verdict line.</param>
public sealed record BaselineFinding(BaselineDefect Defect, string Detail);

/// <summary>
/// Locates what moved in a sequence of raw 8-bit grayscale frames, by differencing each
/// frame against an averaged baseline.
/// </summary>
/// <remarks>
/// <para>
/// The baseline is the scene as it looks without the thing being measured -- for a cursor
/// run, frames in which the pointer is parked off the captured surface. Everything this
/// class reports is relative to that assumption, which is why it is checked rather than
/// assumed: a baseline averaged over frames in which the pointer was already moving
/// contains a smeared pointer, and the pointer then partly cancels out of every later
/// difference. The measurement does not fail loudly when that happens; it quietly reports
/// a smaller sprite, or none.
/// </para>
/// <para>
/// Grayscale rather than colour on purpose. The decoder converts once, the comparison is
/// one byte per pixel, and no cursor test has ever depended on hue -- a cursor that is
/// present but the wrong colour is a different measurement from a cursor that is absent
/// or in the wrong place.
/// </para>
/// </remarks>
public static class GrayFrames
{
    /// <summary>
    /// Averages a range of frames into a baseline, and reports whether that range was
    /// still enough to be one.
    /// </summary>
    /// <param name="frames">Frame pixels, one array per frame, each width * height bytes.</param>
    /// <param name="from">First baseline frame, inclusive.</param>
    /// <param name="to">Last baseline frame, exclusive.</param>
    /// <param name="stabilityThreshold">
    /// A baseline frame that differs from the running average by more than this, in more
    /// than <paramref name="stabilityPixelBudget"/> pixels, makes the baseline unstable.
    /// </param>
    /// <param name="stabilityPixelBudget">Pixels allowed to differ before the baseline is called unstable.</param>
    public static (byte[] Baseline, BaselineFinding? Finding) Baseline(
        IReadOnlyList<byte[]> frames,
        int from,
        int to,
        int stabilityThreshold = 40,
        int stabilityPixelBudget = 64)
    {
        ArgumentNullException.ThrowIfNull(frames);

        var first = Math.Max(from, 0);
        var last = Math.Min(to, frames.Count);
        if (last <= first)
        {
            return ([], new BaselineFinding(
                BaselineDefect.Empty,
                $"frames {from.ToString(CultureInfo.InvariantCulture)} to {to.ToString(CultureInfo.InvariantCulture)} of {frames.Count.ToString(CultureInfo.InvariantCulture)} contain no baseline"));
        }

        var pixels = frames[first].Length;
        var sums = new long[pixels];
        for (var index = first; index < last; index++)
        {
            var frame = frames[index];
            for (var offset = 0; offset < pixels; offset++)
            {
                sums[offset] += frame[offset];
            }
        }

        var count = last - first;
        var baseline = new byte[pixels];
        for (var offset = 0; offset < pixels; offset++)
        {
            baseline[offset] = (byte)(sums[offset] / count);
        }

        var worstFrame = -1;
        var worstPixels = 0;
        for (var index = first; index < last; index++)
        {
            var frame = frames[index];
            var differing = 0;
            for (var offset = 0; offset < pixels; offset++)
            {
                if (Math.Abs(frame[offset] - baseline[offset]) >= stabilityThreshold)
                {
                    differing++;
                }
            }

            if (differing > worstPixels)
            {
                worstPixels = differing;
                worstFrame = index;
            }
        }

        if (worstPixels > stabilityPixelBudget)
        {
            return (baseline, new BaselineFinding(
                BaselineDefect.Unstable,
                $"frame {worstFrame.ToString(CultureInfo.InvariantCulture)} differs from the baseline in {worstPixels.ToString(CultureInfo.InvariantCulture)} pixel(s), " +
                $"over the {stabilityPixelBudget.ToString(CultureInfo.InvariantCulture)} allowed; the scene was moving while the reference was taken"));
        }

        return (baseline, null);
    }

    /// <summary>
    /// The changed region of every frame, relative to a baseline.
    /// </summary>
    /// <param name="frames">Frame pixels, one array per frame.</param>
    /// <param name="baseline">The baseline to difference against, the same length as one frame.</param>
    /// <param name="width">Frame width in pixels.</param>
    /// <param name="threshold">Per-pixel absolute difference that counts as changed.</param>
    public static ReadOnlyCollection<ChangedRegion> Differences(
        IReadOnlyList<byte[]> frames,
        byte[] baseline,
        int width,
        int threshold = 40)
    {
        ArgumentNullException.ThrowIfNull(frames);
        ArgumentNullException.ThrowIfNull(baseline);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);

        var regions = new List<ChangedRegion>(frames.Count);
        for (var index = 0; index < frames.Count; index++)
        {
            var frame = frames[index];
            if (frame.Length != baseline.Length)
            {
                throw new ArgumentException(
                    $"frame {index.ToString(CultureInfo.InvariantCulture)} is {frame.Length.ToString(CultureInfo.InvariantCulture)} bytes against a {baseline.Length.ToString(CultureInfo.InvariantCulture)}-byte baseline",
                    nameof(frames));
            }

            var changed = 0;
            long sumX = 0;
            long sumY = 0;
            var minX = int.MaxValue;
            var minY = int.MaxValue;
            var maxX = -1;
            var maxY = -1;

            for (var offset = 0; offset < frame.Length; offset++)
            {
                if (Math.Abs(frame[offset] - baseline[offset]) < threshold)
                {
                    continue;
                }

                var x = offset % width;
                var y = offset / width;
                changed++;
                sumX += x;
                sumY += y;
                if (x < minX) { minX = x; }
                if (x > maxX) { maxX = x; }
                if (y < minY) { minY = y; }
                if (y > maxY) { maxY = y; }
            }

            regions.Add(changed == 0
                ? new ChangedRegion(index, 0, -1, -1, -1, -1, double.NaN, double.NaN)
                : new ChangedRegion(
                    index,
                    changed,
                    minX,
                    minY,
                    maxX,
                    maxY,
                    (double)sumX / changed,
                    (double)sumY / changed));
        }

        return new ReadOnlyCollection<ChangedRegion>(regions);
    }

    /// <summary>
    /// Finds the frame a full-surface marker flash was drawn in.
    /// </summary>
    /// <remarks>
    /// The marker is a whole-frame luminance step, which is what makes it findable without
    /// knowing where anything is on screen and impossible to confuse with a cursor: a
    /// pointer sprite changes a few hundred pixels, a flash changes nearly all of them.
    /// The first frame whose mean luminance departs from the baseline mean by at least
    /// <paramref name="minimumMeanStep"/> is the marker frame.
    /// </remarks>
    /// <param name="frames">Frame pixels, one array per frame.</param>
    /// <param name="baseline">The baseline to compare mean luminance against.</param>
    /// <param name="minimumMeanStep">Mean luminance step that identifies the flash.</param>
    /// <param name="searchFrom">First frame to consider.</param>
    /// <returns>The frame index, or -1 when no frame carries the marker.</returns>
    public static int FindMarkerFrame(
        IReadOnlyList<byte[]> frames,
        byte[] baseline,
        double minimumMeanStep = 40,
        int searchFrom = 0)
    {
        ArgumentNullException.ThrowIfNull(frames);
        ArgumentNullException.ThrowIfNull(baseline);
        if (baseline.Length == 0)
        {
            return -1;
        }

        double baselineMean = 0;
        foreach (var value in baseline)
        {
            baselineMean += value;
        }

        baselineMean /= baseline.Length;

        for (var index = Math.Max(searchFrom, 0); index < frames.Count; index++)
        {
            var frame = frames[index];
            if (frame.Length != baseline.Length)
            {
                continue;
            }

            double mean = 0;
            foreach (var value in frame)
            {
                mean += value;
            }

            mean /= frame.Length;
            if (Math.Abs(mean - baselineMean) >= minimumMeanStep)
            {
                return index;
            }
        }

        return -1;
    }
}
