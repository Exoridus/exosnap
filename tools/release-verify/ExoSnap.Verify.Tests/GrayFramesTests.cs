using ExoSnap.Verify.Analysis;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// Locating what moved in a frame sequence, and knowing when the baseline it is
/// measured against cannot be trusted.
/// </summary>
/// <remarks>
/// A baseline averaged over frames in which the pointer was already moving contains
/// a smeared pointer, and the pointer then partly cancels out of every later
/// difference. That does not fail loudly: it quietly reports a smaller sprite, or
/// none, which reads as "the cursor was not drawn".
/// </remarks>
public sealed class GrayFramesTests
{
    private const int Width = 8;
    private const int Height = 8;

    private static byte[] Blank(byte value = 10) => [.. Enumerable.Repeat(value, Width * Height)];

    private static byte[] WithBlock(int x, int y, int size, byte value = 250, byte background = 10)
    {
        var frame = Blank(background);
        for (var row = y; row < y + size; row++)
        {
            for (var column = x; column < x + size; column++)
            {
                frame[(row * Width) + column] = value;
            }
        }

        return frame;
    }

    [Fact]
    public void AStillBaselineIsAccepted()
    {
        List<byte[]> frames = [Blank(), Blank(), Blank()];

        var (baseline, finding) = GrayFrames.Baseline(frames, 0, 3);

        Assert.Null(finding);
        Assert.Equal(Width * Height, baseline.Length);
        Assert.All(baseline, value => Assert.Equal(10, value));
    }

    [Fact]
    public void ABaselineTakenWhileTheSceneMovedIsReported()
    {
        // The failure this check exists for: the reference silently contains the
        // thing it is supposed to exclude.
        List<byte[]> frames = [Blank(), WithBlock(1, 1, 4), Blank()];

        // The default pixel budget is sized for a real frame; this fixture is 64
        // pixels in total, so the budget has to be scaled with it or every
        // contamination fits inside it.
        var (_, finding) = GrayFrames.Baseline(frames, 0, 3, stabilityPixelBudget: 4);

        Assert.NotNull(finding);
        Assert.Equal(BaselineDefect.Unstable, finding!.Defect);
        Assert.Contains("the scene was moving", finding.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void AnEmptyBaselineRangeIsReportedRatherThanThrown()
    {
        var (baseline, finding) = GrayFrames.Baseline([Blank()], 5, 9);

        Assert.NotNull(finding);
        Assert.Equal(BaselineDefect.Empty, finding!.Defect);
        Assert.Empty(baseline);
    }

    [Fact]
    public void ABaselineRangeBeyondTheFramesIsClamped()
    {
        List<byte[]> frames = [Blank(), Blank()];

        var (baseline, finding) = GrayFrames.Baseline(frames, 0, 100);

        Assert.Null(finding);
        Assert.Equal(Width * Height, baseline.Length);
    }

    [Fact]
    public void TheChangedRegionIsLocatedAndItsCentroidComputed()
    {
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);
        List<byte[]> frames = [WithBlock(2, 3, 2)];

        var regions = GrayFrames.Differences(frames, baseline, Width);

        var region = Assert.Single(regions);
        Assert.Equal(4, region.ChangedPixels);
        Assert.Equal(2, region.MinX);
        Assert.Equal(3, region.MinY);
        Assert.Equal(3, region.MaxX);
        Assert.Equal(4, region.MaxY);
        Assert.Equal(2.5, region.CentroidX, 6);
        Assert.Equal(3.5, region.CentroidY, 6);
    }

    [Fact]
    public void AnUnchangedFrameReportsEmptyRatherThanACentroidOfZero()
    {
        // A centroid of (0, 0) is a real position; reporting it for "nothing
        // changed" puts a sprite in the top-left corner of every still frame.
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);

        var region = Assert.Single(GrayFrames.Differences([Blank()], baseline, Width));

        Assert.True(region.IsEmpty);
        Assert.Equal(-1, region.MinX);
        Assert.True(double.IsNaN(region.CentroidX));
    }

    [Fact]
    public void AChangeBelowTheThresholdDoesNotCount()
    {
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);
        List<byte[]> frames = [WithBlock(2, 3, 2, value: 30)];

        var region = Assert.Single(GrayFrames.Differences(frames, baseline, Width, threshold: 40));

        Assert.True(region.IsEmpty);
    }

    [Fact]
    public void AFrameOfTheWrongSizeIsRefusedByName()
    {
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);

        var thrown = Assert.Throws<ArgumentException>(
            () => GrayFrames.Differences([Blank(), new byte[3]], baseline, Width));

        Assert.Contains("frame 1", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void TheMarkerFlashIsFoundByItsWholeFrameLuminanceStep()
    {
        // A pointer sprite changes a few hundred pixels; a flash changes nearly all
        // of them. That is what makes the marker findable without knowing where
        // anything is on screen, and impossible to confuse with a cursor.
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);
        List<byte[]> frames = [Blank(), WithBlock(1, 1, 3), Blank(240), Blank()];

        Assert.Equal(2, GrayFrames.FindMarkerFrame(frames, baseline));
    }

    [Fact]
    public void NoMarkerFrameIsMinusOneRatherThanTheFirstFrame()
    {
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);
        List<byte[]> frames = [Blank(), WithBlock(1, 1, 3), Blank()];

        Assert.Equal(-1, GrayFrames.FindMarkerFrame(frames, baseline));
    }

    [Fact]
    public void TheMarkerSearchCanSkipAnEarlierFlash()
    {
        var (baseline, _) = GrayFrames.Baseline([Blank()], 0, 1);
        List<byte[]> frames = [Blank(240), Blank(), Blank(240)];

        Assert.Equal(2, GrayFrames.FindMarkerFrame(frames, baseline, searchFrom: 1));
    }
}
