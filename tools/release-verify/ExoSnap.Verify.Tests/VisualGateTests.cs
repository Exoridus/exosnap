using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Tests;

/// <summary>REL-VIS-OVERLAY-001: OverlayAppearanceGate scenario logic, against fakes.</summary>
public sealed class OverlayAppearanceGateTests
{
    private static readonly TimeSpan NoPause = TimeSpan.Zero;

    private static void WireOverlaysUp(GateFakes fakes, bool visible = true, int automationElements = 3)
    {
        fakes.SystemAppearance.Current = AppsAppearance.Dark;
        fakes.Session.SetResult("app.identity", """{"pid":4242}""");
        fakes.Session.SetResult(
            "overlay.snapshot",
            $$"""{"overlays":[{"objectName":"quickOverlayRecordingPill","visible":{{(visible ? "true" : "false")}}}],"count":1}""");
        fakes.Uia.SeeElements(
            [.. Enumerable.Range(0, automationElements).Select(index => $"quickOverlay element {index}")]);
    }

    [Fact]
    public async Task IsUnavailableWhenTheAppsColourAppearanceCannotBeRead()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes => fakes.SystemAppearance.Current = AppsAppearance.Unknown,
            TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Unavailable, result.Outcome);
    }

    [Fact]
    public async Task DefersTheColourJudgementWhenTheOverlaysReachedTheDesktopUnderBothAppearances()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001", fakes => WireOverlaysUp(fakes), TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Deferred, result.Outcome);
        Assert.Contains("colour judgement", result.Message, StringComparison.Ordinal);
        Assert.Contains("light:", result.Message, StringComparison.Ordinal);
        Assert.Contains("dark:", result.Message, StringComparison.Ordinal);
        Assert.NotEmpty(result.Evidence);
    }

    [Fact]
    public async Task DrivesLightThenDarkAndRestoresTheOriginalAppearance()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001", fakes => WireOverlaysUp(fakes), TestContext.Current.CancellationToken);

        await new OverlayAppearanceGate(NoPause).RunAsync(harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(
            new[] { AppsAppearance.Light, AppsAppearance.Dark, AppsAppearance.Dark },
            harness.Fakes.SystemAppearance.Applied);
        Assert.Equal(AppsAppearance.Dark, harness.Fakes.SystemAppearance.Current);
    }

    [Fact]
    public async Task RestoresEachOverlaySettingToTheValueItReadNotToOff()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes =>
            {
                WireOverlaysUp(fakes);
                // The developer already had the recording overlay on; the gate must
                // put it back on, not reset it to the default.
                fakes.Session.SetResult("settings.get", """{"app.showRecordingOverlay":true}""");
            },
            TestContext.Current.CancellationToken);

        await new OverlayAppearanceGate(NoPause).RunAsync(harness.Context, TestContext.Current.CancellationToken);

        var writes = harness.Fakes.Session.SettingsSet;
        Assert.Equal((true, false, false), (
            LastValueFor(writes, "app.showRecordingOverlay"),
            LastValueFor(writes, "app.showDiagnosticsOverlay"),
            LastValueFor(writes, "app.showQuickControls")));
    }

    private static object? LastValueFor(IEnumerable<(string Key, object? Value)> writes, string key) =>
        writes.Where(write => write.Key == key).Select(write => write.Value).LastOrDefault();

    [Fact]
    public async Task IsInfrastructureErrorWhenNoOverlayIsOnScreen()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes => WireOverlaysUp(fakes, visible: false),
            TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("no overlay was on screen", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenUiAutomationCannotReadTheProcess()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes =>
            {
                WireOverlaysUp(fakes);
                fakes.Uia.Result = Windows.Uia.UiTreeSnapshot.Unreadable("UIAutomationCore did not load");
            },
            TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("UI Automation could not read", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenOverlaysAreClaimedButUiAutomationFindsNoWindow()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes => WireOverlaysUp(fakes, automationElements: 0),
            TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("did not reach the desktop", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenTheAppearanceIsNotRestored()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-OVERLAY-001",
            fakes =>
            {
                WireOverlaysUp(fakes);
                fakes.SystemAppearance.DriftAfterApply = AppsAppearance.Light;
            },
            TestContext.Current.CancellationToken);

        var result = await new OverlayAppearanceGate(NoPause).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("not restored", result.Message, StringComparison.Ordinal);
    }
}

/// <summary>REL-VIS-NOTIFY-001: NotificationSeverityGate scenario logic, against fakes.</summary>
public sealed class NotificationSeverityGateTests
{
    private static readonly TimeSpan ShortHub = TimeSpan.FromMilliseconds(400);

    private const string Empty = """{"entries":[]}""";

    private static string HubWith(string severity, string title, long sequence = 7) =>
        $$"""{"entries":[{"sequence":{{sequence}},"title":"{{title}}","body":"b","severity":"{{severity}}","unread":true}]}""";

    [Fact]
    public async Task DefersTheGlyphJudgementWhenAFreshEntryReachesTheHubAndTheDesktop()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-NOTIFY-001",
            fakes =>
            {
                fakes.Session.SetResultSequence(
                    "notifications.snapshot", Empty, HubWith("warning", "Storage running low"));
                fakes.Session.SetResult("app.identity", """{"pid":4242}""");
                fakes.Uia.SeeElements("Warning. Storage running low.", "Storage running low");
            },
            TestContext.Current.CancellationToken);

        var result = await new NotificationSeverityGate(ShortHub).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Deferred, result.Outcome);
        Assert.Contains("severity word", result.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("[synthetic]", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task MarksTheVerdictSyntheticWhenItHadToRaiseTheNotificationItself()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-NOTIFY-001",
            fakes =>
            {
                // Empty through the baseline read and the whole first wait; the raised
                // one only shows up once the second wait starts.
                fakes.Session.SetResultSequence(
                    "notifications.snapshot",
                    Empty, Empty, Empty, Empty, Empty, Empty,
                    HubWith("failure", "Recording stopped unexpectedly"));
                fakes.Session.SetResult("app.identity", """{"pid":4242}""");
                fakes.Uia.SeeElements("Failure. Recording stopped unexpectedly.");
            },
            TestContext.Current.CancellationToken);

        var result = await new NotificationSeverityGate(ShortHub).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Deferred, result.Outcome);
        Assert.Contains("[synthetic]", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenAHubEntryCarriesNoSeverityWord()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-NOTIFY-001",
            fakes =>
            {
                fakes.Session.SetResultSequence(
                    "notifications.snapshot",
                    Empty,
                    """{"entries":[{"sequence":9,"title":"Something happened","severity":"","unread":true}]}""");
                fakes.Session.SetResult("app.identity", """{"pid":4242}""");
                fakes.Uia.SeeElements("Something happened");
            },
            TestContext.Current.CancellationToken);

        var result = await new NotificationSeverityGate(ShortHub).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("no severity word", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task FailsWhenTheHubRecordsAnEntryThatNeverReachedTheDesktop()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-NOTIFY-001",
            fakes =>
            {
                fakes.Session.SetResultSequence(
                    "notifications.snapshot", Empty, HubWith("warning", "A source lost its device"));
                fakes.Session.SetResult("app.identity", """{"pid":4242}""");
                // UI Automation is readable but the toast text is absent.
                fakes.Uia.SeeElements("ExoSnap", "Record", "Edit");
            },
            TestContext.Current.CancellationToken);

        var result = await new NotificationSeverityGate(ShortHub).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.Fail, result.Outcome);
        Assert.Contains("never reached the desktop", result.Message, StringComparison.Ordinal);
    }

    [Fact]
    public async Task IsInfrastructureErrorWhenNoNotificationCouldBeRaised()
    {
        using var harness = await GateHarness.CreateAsync(
            "REL-VIS-NOTIFY-001",
            fakes =>
            {
                fakes.Session.SetResult("notifications.snapshot", Empty);
                fakes.Session.Refuse("notification.raise", "unavailable", "the notification manager is not ready");
            },
            TestContext.Current.CancellationToken);

        var result = await new NotificationSeverityGate(ShortHub).RunAsync(
            harness.Context, TestContext.Current.CancellationToken);

        Assert.Equal(ScenarioOutcome.InfrastructureError, result.Outcome);
        Assert.Contains("none could be raised", result.Message, StringComparison.Ordinal);
    }
}
