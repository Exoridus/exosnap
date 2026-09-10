using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;
using ExoSnap.Verify.Windows;
using ExoSnap.Verify.Windows.Uia;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-VIS-OVERLAY-001: the capture-excluded overlays stay fixed-dark under both
/// Windows appearances.
/// </summary>
/// <remarks>
/// The verdict this body can reach is not the whole scenario. The five overlays set
/// <c>WDA_EXCLUDEFROMCAPTURE</c>, which defeats every pixel instrument the project
/// has, so whether they compose correctly on the glass is a person's judgement. What
/// is automatable, and what this does, is: put every overlay on screen, drive
/// Windows through Light and Dark, and prove through UI Automation - the one reader
/// that survives the exclusion - that the windows really reached the desktop under
/// each appearance. It then hands the colour judgement to the developer as
/// <see cref="ScenarioOutcome.Deferred"/>.
/// </remarks>
public sealed class OverlayAppearanceGate : IScenarioBody
{
    private static readonly string[] OverlaySettings =
        ["app.showRecordingOverlay", "app.showDiagnosticsOverlay", "app.showQuickControls"];

    private static readonly AppsAppearance[] BothAppearances = [AppsAppearance.Light, AppsAppearance.Dark];

    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);
    private static readonly TimeSpan UiaTimeout = TimeSpan.FromSeconds(10);

    private readonly TimeSpan repaintPause;

    /// <summary>Creates the gate with the production repaint pause.</summary>
    public OverlayAppearanceGate()
        : this(TimeSpan.FromSeconds(2))
    {
    }

    /// <summary>Creates the gate with a chosen repaint pause, so a test need not wait it out.</summary>
    internal OverlayAppearanceGate(TimeSpan repaintPause) => this.repaintPause = repaintPause;

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var original = services.SystemAppearance.Current;
        if (original == AppsAppearance.Unknown)
        {
            return ScenarioResult.Unavailable(
                "the Windows apps-colour appearance could not be read, so it cannot be driven and restored");
        }

        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);
        var restoreSettings = await TurnOnAsync(session, OverlaySettings, cancellationToken).ConfigureAwait(false);

        var evidence = new List<Evidence>();
        var perAppearance = new List<AppearanceObservation>();

        try
        {
            await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
            await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
            await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
                .ConfigureAwait(false);

            // The toast is the fifth capture-excluded window and does not appear on its
            // own. Raised synthetically and marked as such: this scenario is about how
            // the overlay composes, never about when the product raises one.
            await session.InvokeAsync(
                    "notification.raise",
                    new Dictionary<string, object?>(StringComparer.Ordinal)
                    {
                        ["type"] = "windowCaptureStalled",
                        ["title"] = "Window capture appears to have stalled",
                        ["body"] = "Synthetic notification, raised so its toast can be judged.",
                        ["action"] = "openDiagnostics",
                    },
                    cancellationToken)
                .ConfigureAwait(false);

            var identity = await session.AppIdentityAsync(cancellationToken).ConfigureAwait(false);
            var processId = (int)(Snapshots.Number(identity.Result, "pid") ?? 0);

            foreach (var appearance in BothAppearances)
            {
                services.SystemAppearance.Apply(appearance);
                await Task.Delay(this.repaintPause, cancellationToken).ConfigureAwait(false);

                var overlays = await session.InvokeAsync("overlay.snapshot", null, cancellationToken)
                    .ConfigureAwait(false);
                var tree = services.Uia.SnapshotProcess(processId, UiaTimeout);

                evidence.Add(GateEvidence.SaveJson(context, $"overlays-{Name(appearance)}.json", overlays.Result));
                evidence.Add(GateEvidence.SaveText(
                    context, $"automation-tree-{Name(appearance)}.json", DescribeTree(tree)));

                var visible = Snapshots.Items(overlays.Result, "overlays")
                    .Count(overlay => Snapshots.IsTrue(overlay, "visible"));
                perAppearance.Add(new AppearanceObservation(appearance, visible, tree.Ok, tree.Elements.Count));
            }
        }
        finally
        {
            services.SystemAppearance.Apply(original);
            await RestoreAsync(session, restoreSettings, cancellationToken).ConfigureAwait(false);
            await StopQuietlyAsync(session, cancellationToken).ConfigureAwait(false);
        }

        if (services.SystemAppearance.Current != original)
        {
            return ScenarioResult.InfrastructureError(
                $"the apps-colour appearance was left at {services.SystemAppearance.Current}, not restored to {original}",
                [.. evidence]);
        }

        var noOverlay = perAppearance.FirstOrDefault(observation => observation.VisibleOverlays == 0);
        if (noOverlay is not null)
        {
            return ScenarioResult.InfrastructureError(
                $"no overlay was on screen in {Name(noOverlay.Appearance)}, so nothing could be judged",
                [.. evidence]);
        }

        var unreadable = perAppearance.FirstOrDefault(observation => !observation.UiaReadable);
        if (unreadable is not null)
        {
            return ScenarioResult.InfrastructureError(
                $"UI Automation could not read the process in {Name(unreadable.Appearance)}; " +
                "the capture-excluded overlays have no other reader",
                [.. evidence]);
        }

        var absent = perAppearance.FirstOrDefault(observation => observation.UiaElements == 0);
        if (absent is not null)
        {
            return ScenarioResult.InfrastructureError(
                $"overlay.snapshot reported overlays in {Name(absent.Appearance)} but UI Automation found no " +
                "window in the process; they did not reach the desktop",
                [.. evidence]);
        }

        var summary = string.Join(
            "; ",
            perAppearance.Select(observation =>
                $"{Name(observation.Appearance)}: {Count(observation.VisibleOverlays)} overlay(s) up, " +
                $"UI Automation saw {Count(observation.UiaElements)} element(s)"));

        return new ScenarioResult(
            ScenarioOutcome.Deferred,
            $"overlays reached the desktop under both appearances ({summary}); the colour judgement " +
            "- every capture-excluded overlay stays fixed-dark with legible text, and no state colour is " +
            "replaced by the accent - is the developer's, with the app on screen",
            [.. evidence]);
    }

    private static async Task<Dictionary<string, object?>> TurnOnAsync(
        ILiveVerifySession session,
        IReadOnlyList<string> keys,
        CancellationToken cancellationToken)
    {
        var originals = new Dictionary<string, object?>(StringComparer.Ordinal);
        foreach (var key in keys)
        {
            // settings.get answers a key-to-value map: the value is under the literal
            // key, dot and all, not nested. A dotted-path read would find nothing and
            // silently treat every overlay as already off.
            var read = await session.InvokeAsync(
                    "settings.get",
                    new Dictionary<string, object?>(StringComparer.Ordinal) { ["key"] = key },
                    cancellationToken)
                .ConfigureAwait(false);
            originals[key] = read.Result.ValueKind == JsonValueKind.Object &&
                             read.Result.TryGetProperty(key, out var value) &&
                             value.ValueKind == JsonValueKind.True;
            await session.SetSettingAsync(key, true, cancellationToken).ConfigureAwait(false);
        }

        return originals;
    }

    private static async Task RestoreAsync(
        ILiveVerifySession session,
        IReadOnlyDictionary<string, object?> originals,
        CancellationToken cancellationToken)
    {
        foreach (var (key, value) in originals)
        {
            await session.SetSettingAsync(key, value, cancellationToken).ConfigureAwait(false);
        }
    }

    private static async Task StopQuietlyAsync(ILiveVerifySession session, CancellationToken cancellationToken)
    {
        try
        {
            await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
            await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (LiveVerify.LiveVerifyException)
        {
            // The verdict has already been decided by what was observed on screen; a
            // stop that could not be confirmed does not change it.
        }
    }

    private static string DescribeTree(UiTreeSnapshot tree)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteBoolean("ok", tree.Ok);
            writer.WriteString("detail", tree.Detail);
            writer.WriteStartArray("elements");
            foreach (var element in tree.Elements)
            {
                writer.WriteStartObject();
                writer.WriteString("name", element.Name);
                writer.WriteString("className", element.ClassName);
                writer.WriteString("controlType", element.ControlType);
                writer.WriteString("automationId", element.AutomationId);
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }

    private static string Name(AppsAppearance appearance) =>
        appearance.ToString().ToLower(CultureInfo.InvariantCulture);

    private static string Count(int value) => value.ToString(CultureInfo.InvariantCulture);

    private sealed record AppearanceObservation(
        AppsAppearance Appearance,
        int VisibleOverlays,
        bool UiaReadable,
        int UiaElements);
}

/// <summary>
/// REL-VIS-NOTIFY-001: desktop notifications render with the right severity glyph.
/// </summary>
/// <remarks>
/// The hub is the harness's own record of what it published; it cannot say what
/// reached the desktop. UI Automation can - the toast window is capture-excluded, so
/// no screenshot ever shows it - and this asserts that every new hub entry's
/// severity word and title are present in the automation tree. The glyph shape and
/// the tint are what no API reports, so those stay the developer's judgement.
/// </remarks>
public sealed class NotificationSeverityGate : IScenarioBody
{
    private static readonly TimeSpan HubPoll = TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan UiaTimeout = TimeSpan.FromSeconds(10);

    private readonly TimeSpan hubDeadline;

    /// <summary>Creates the gate with the production hub-settle deadline.</summary>
    public NotificationSeverityGate()
        : this(TimeSpan.FromSeconds(10))
    {
    }

    /// <summary>Creates the gate with a chosen hub-settle deadline, so a test need not wait it out.</summary>
    internal NotificationSeverityGate(TimeSpan hubDeadline) => this.hubDeadline = hubDeadline;

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var before = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var baseline = SequencesOf(before.Result);

        // A real product notification first: ExoSnap notifies about faults, and a
        // clean machine has none, so this often publishes nothing.
        await session.InvokeAsync("diagnostics.run", null, cancellationToken).ConfigureAwait(false);
        var (fresh, latest) = await WaitForFreshAsync(session, baseline, cancellationToken).ConfigureAwait(false);

        var synthetic = false;
        if (fresh.Count == 0)
        {
            var raised = await session.InvokeAsync(
                    "notification.raise",
                    new Dictionary<string, object?>(StringComparer.Ordinal)
                    {
                        ["type"] = "windowCaptureStalled",
                        ["title"] = "Window capture appears to have stalled",
                        ["body"] = "Synthetic notification, raised so the toast and hub entry can be judged.",
                        ["action"] = "openDiagnostics",
                    },
                    cancellationToken)
                .ConfigureAwait(false);
            if (!raised.Ok)
            {
                return ScenarioResult.InfrastructureError(
                    $"the product published no notification and none could be raised to judge the surface: {raised.Refusal}");
            }

            synthetic = true;
            (fresh, latest) = await WaitForFreshAsync(session, baseline, cancellationToken).ConfigureAwait(false);
            if (fresh.Count == 0)
            {
                return ScenarioResult.InfrastructureError("a notification was raised but never reached the hub");
            }
        }

        var identity = await session.AppIdentityAsync(cancellationToken).ConfigureAwait(false);
        var processId = (int)(Snapshots.Number(identity.Result, "pid") ?? 0);
        var tree = services.Uia.SnapshotProcess(processId, UiaTimeout);

        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "notifications.json", latest),
            GateEvidence.SaveText(context, "automation-tree.json", TreeText(tree)),
        };

        var wordless = fresh
            .Where(entry => string.IsNullOrWhiteSpace(Snapshots.Text(entry, "severity")))
            .Select(entry => Snapshots.Text(entry, "title"))
            .ToList();
        if (wordless.Count > 0)
        {
            return ScenarioResult.Fail(
                $"a hub entry carries no severity word, only a colour: {string.Join(" | ", wordless)}", evidence);
        }

        var required = fresh
            .SelectMany(entry => new[] { Snapshots.Text(entry, "severity"), Snapshots.Text(entry, "title") })
            .Where(text => !string.IsNullOrWhiteSpace(text))
            .Distinct(StringComparer.Ordinal)
            .ToList();

        var onDesktop = "UI Automation could not be asked";
        if (tree.Ok)
        {
            var missing = tree.MissingText(required);
            if (missing.Count > 0)
            {
                return ScenarioResult.Fail(
                    $"the hub recorded the notification but it never reached the desktop: {string.Join(" | ", missing)}",
                    evidence);
            }

            onDesktop = $"all {required.Count.ToString(CultureInfo.InvariantCulture)} required string(s) are on the desktop";
        }

        var origin = synthetic ? "[synthetic] " : string.Empty;
        return new ScenarioResult(
            ScenarioOutcome.Deferred,
            $"{origin}{fresh.Count.ToString(CultureInfo.InvariantCulture)} new notification(s) reached the hub with a " +
            $"severity word and the desktop with their text ({onDesktop}); the glyph shape and the severity tint " +
            "are the developer's judgement",
            evidence);
    }

    private async Task<(IReadOnlyList<JsonElement> Fresh, JsonElement Latest)> WaitForFreshAsync(
        ILiveVerifySession session,
        IReadOnlyCollection<double> baseline,
        CancellationToken cancellationToken)
    {
        var deadline = DateTime.UtcNow.Add(this.hubDeadline);
        JsonElement latest = default;
        while (true)
        {
            var snapshot = await session.NotificationsSnapshotAsync(cancellationToken).ConfigureAwait(false);
            latest = snapshot.Result.Clone();
            var fresh = Snapshots.NotificationEntries(latest)
                .Where(entry => !baseline.Contains(Snapshots.Number(entry, "sequence") ?? double.NaN))
                .ToList();
            if (fresh.Count > 0 || DateTime.UtcNow >= deadline)
            {
                return (fresh, latest);
            }

            await Task.Delay(HubPoll, cancellationToken).ConfigureAwait(false);
        }
    }

    private static IReadOnlyCollection<double> SequencesOf(JsonElement snapshot) =>
    [
        .. Snapshots.NotificationEntries(snapshot)
            .Select(entry => Snapshots.Number(entry, "sequence"))
            .Where(sequence => sequence is not null)
            .Select(sequence => sequence!.Value),
    ];

    private static string TreeText(UiTreeSnapshot tree)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteBoolean("ok", tree.Ok);
            writer.WriteString("detail", tree.Detail);
            writer.WriteStartArray("names");
            foreach (var element in tree.Elements)
            {
                writer.WriteStringValue(element.Name);
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }
}
