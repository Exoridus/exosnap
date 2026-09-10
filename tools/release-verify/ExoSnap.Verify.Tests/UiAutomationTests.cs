using ExoSnap.Verify.Windows.Uia;

namespace ExoSnap.Verify.Tests;

/// <summary>
/// The pure half of the UI Automation adapter: the text matcher a visual gate uses
/// to decide whether a capture-excluded surface reached the desktop with its words
/// on it. No automation client is touched here.
/// </summary>
public sealed class UiAutomationTextTests
{
    private static UiTreeSnapshot TreeOf(params string[] names) =>
        UiTreeSnapshot.Of(names.Select(name => new UiElement(name, "Qt6QWindowIcon", "ControlType.Text", string.Empty)));

    [Fact]
    public void EveryRequiredStringPresentAsASubstringLeavesNothingMissing()
    {
        var tree = TreeOf("Recording", "Window capture appears to have stalled", "Open diagnostics");

        var missing = tree.MissingText(["capture appears to have stalled", "Recording"]);

        Assert.Empty(missing);
    }

    [Fact]
    public void AStringNowhereInTheTreeIsReturnedAsMissing()
    {
        var tree = TreeOf("Recording", "Ready");

        var missing = tree.MissingText(["Warning. Storage running low."]);

        Assert.Equal(["Warning. Storage running low."], missing);
    }

    [Fact]
    public void MatchingIsCaseInsensitive()
    {
        var tree = TreeOf("WINDOW CAPTURE STALLED");

        Assert.Empty(tree.MissingText(["window capture stalled"]));
    }

    [Fact]
    public void OnlyTheMissingStringsComeBackNotTheFoundOnes()
    {
        var tree = TreeOf("Recording saved", "Show in folder");

        var missing = tree.MissingText(["Recording saved", "Edit", "Show in folder", "Delete"]);

        Assert.Equal(["Edit", "Delete"], missing);
    }

    [Fact]
    public void AnEmptyRequirementSetIsVacuouslySatisfied()
    {
        Assert.Empty(TreeOf("anything").MissingText([]));
    }

    [Fact]
    public void AnUnreadableTreeReportsEveryRequiredStringMissing()
    {
        var tree = UiTreeSnapshot.Unreadable("UIAutomationCore could not be loaded");

        var missing = tree.MissingText(["Recording", "Ready"]);

        Assert.Equal(["Recording", "Ready"], missing);
    }
}
