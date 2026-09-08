using System.Collections.ObjectModel;
using System.Globalization;
using System.Text.Json;
using ExoSnap.Verify.Adapters.LiveVerify;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Gates;

/// <summary>
/// REL-DISP-REFRESH-001: a recording is correct at a refresh rate the orchestrator
/// applied and verified.
/// </summary>
/// <remarks>
/// The rate is chosen against the machine, by the same rule as REL-ENV-003: Windows
/// enumerates the nominal and the actual rate of one physical mode separately, so a
/// gate that hardcoded 60 would be answered by a read-back of 59 and the transaction
/// would correctly refuse.
/// </remarks>
public sealed class DisplayRefreshGate : IScenarioBody
{
    private static readonly TimeSpan RecordFor = TimeSpan.FromSeconds(8);
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        if (!services.Envctl.Available)
        {
            return ScenarioResult.Unavailable("exosnap-envctl is not built");
        }

        var desired = await DisplayModes
            .DesiredRefreshAsync(services, EnvironmentMutationGate.DisplayAlias, cancellationToken)
            .ConfigureAwait(false);
        if (desired.Refusal is not null)
        {
            return ScenarioResult.Unavailable(desired.Refusal);
        }

        var outcome = await services.Environment.RunAsync(
            context.Descriptor.Id,
            desired.Desired,
            (begun, token) => RecordAsync(context, services, begun, token),
            cancellationToken).ConfigureAwait(false);

        return GateOutcome.Of(outcome);
    }

    private static async Task<ScenarioResult> RecordAsync(
        ScenarioContext context,
        GateServices services,
        JsonElement transaction,
        CancellationToken cancellationToken)
    {
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);
        await Task.Delay(RecordFor, cancellationToken).ConfigureAwait(false);

        var pipeline = await session.PipelineSnapshotAsync(cancellationToken).ConfigureAwait(false);
        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
            .ConfigureAwait(false);

        var result = await session.RecordResultAsync(cancellationToken).ConfigureAwait(false);
        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "pipeline.json", pipeline.Result),
            GateEvidence.SaveJson(context, "transaction.json", transaction),
        };

        if (!Snapshots.IsTrue(result.Result, "succeeded"))
        {
            var applied = Snapshots.Items(transaction, "applied");
            var rate = applied.Count > 0 ? Snapshots.Text(applied[0], "to") : "the applied rate";
            return ScenarioResult.Fail($"the recording failed at {rate} Hz", evidence);
        }

        var fps = Snapshots.Number(pipeline.Result, "capture.actualFps");
        return ScenarioResult.Pass(
            "recorded at the applied display refresh; capture fps " +
            (fps?.ToString("0.##", CultureInfo.InvariantCulture) ?? "(not measured)"),
            evidence);
    }
}

/// <summary>
/// REL-DISP-HDR-001: HDR is applied, recorded against, and restored exactly - and the
/// product agrees about which display is in HDR.
/// </summary>
/// <remarks>
/// The product's own view of the machine has to agree with the state the orchestrator
/// just applied and verified. A disagreement means one of the two is reading the wrong
/// display, which is precisely the defect a positional DXGI-to-Qt display match can
/// produce.
/// </remarks>
public sealed class DisplayHdrGate : IScenarioBody
{
    /// <summary>The property the transaction is written against.</summary>
    public const string HdrProperty = EnvironmentMutationGate.DisplayAlias + ":hdr";

    private static readonly TimeSpan RecordFor = TimeSpan.FromSeconds(6);
    private static readonly TimeSpan StartTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan StopTimeout = TimeSpan.FromSeconds(60);

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();

        var desired = new Dictionary<string, string>(StringComparer.Ordinal) { [HdrProperty] = "on" };
        var outcome = await services.Environment.RunAsync(
            context.Descriptor.Id,
            desired,
            (begun, token) => RecordAsync(context, services, begun, token),
            cancellationToken).ConfigureAwait(false);

        return GateOutcome.Of(outcome);
    }

    private static async Task<ScenarioResult> RecordAsync(
        ScenarioContext context,
        GateServices services,
        JsonElement transaction,
        CancellationToken cancellationToken)
    {
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);
        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);

        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "environment.json", environment.Result),
            GateEvidence.SaveJson(context, "transaction.json", transaction),
        };

        var hdrDisplays = Snapshots.Items(environment.Result, "displays.screens")
            .Count(screen => Snapshots.IsTrue(screen, "hdrActive"));
        if (hdrDisplays == 0)
        {
            return ScenarioResult.Fail(
                "the orchestrator applied and verified HDR on, but ExoSnap reports no HDR-active display",
                evidence);
        }

        await session.SelectTargetAsync("monitor", null, cancellationToken).ConfigureAwait(false);
        await session.StartRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync([RecordingStates.Recording], StartTimeout, cancellationToken)
            .ConfigureAwait(false);
        await Task.Delay(RecordFor, cancellationToken).ConfigureAwait(false);
        await session.StopRecordingAsync(cancellationToken).ConfigureAwait(false);
        await session.WaitForRecordingStateAsync(RecordingStates.Terminal, StopTimeout, cancellationToken)
            .ConfigureAwait(false);

        var result = await session.RecordResultAsync(cancellationToken).ConfigureAwait(false);
        if (!Snapshots.IsTrue(result.Result, "succeeded"))
        {
            return ScenarioResult.Fail("the HDR recording did not succeed", evidence);
        }

        return ScenarioResult.Pass(
            $"HDR active on {hdrDisplays.ToString(CultureInfo.InvariantCulture)} display(s); recording succeeded",
            evidence);
    }
}

/// <summary>
/// REL-DISP-MIXED-001: crossing between displays does not freeze the live preview.
/// </summary>
/// <remarks>
/// The gate is about what the crossing does to a live preview, so the precondition is
/// established first: judging a preview that never delivered a frame would pass on the
/// strength of nothing having been published.
/// </remarks>
public sealed class MixedDisplayGate : IScenarioBody
{
    private static readonly TimeSpan PreviewWindow = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan SamplePause = TimeSpan.FromMilliseconds(250);
    private const int SampleCount = 6;

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var displays = Snapshots.Items(environment.Result, "displays.screens");
        if (displays.Count < 2)
        {
            return ScenarioResult.Unavailable(
                $"this machine has {displays.Count.ToString(CultureInfo.InvariantCulture)} display(s); the mixed-monitor scenario needs two");
        }

        var secondary = displays.Where(screen => !Snapshots.IsTrue(screen, "primary")).ToList();
        var target = secondary.Count > 0 ? secondary[0] : displays[1];

        var targetName = Snapshots.Text(target, "name");

        var before = await PreviewProgress.WatchAsync(session, PreviewWindow, cancellationToken).ConfigureAwait(false);
        if (!before.Live)
        {
            return ScenarioResult.InfrastructureError(
                $"the preview never consumed a frame before the crossing ({before.Message})");
        }

        var moved = await session.MoveToScreenAsync(targetName, cancellationToken).ConfigureAwait(false);
        if (!moved.Ok)
        {
            return ScenarioResult.Fail($"window.moveToScreen '{targetName}' refused: {moved.Refusal}");
        }

        var samples = new List<JsonElement>(SampleCount);
        for (var index = 0; index < SampleCount; index++)
        {
            if (index > 0)
            {
                await Task.Delay(SamplePause, cancellationToken).ConfigureAwait(false);
            }

            var preview = await session.PreviewSnapshotAsync(cancellationToken).ConfigureAwait(false);
            samples.Add(preview.Result.Clone());
        }

        var windows = await session.WindowsSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "windows.json", windows.Result),
            SaveSamples(context, samples),
        };

        var verdict = PreviewProgress.FreezeVerdict(samples);
        return verdict with
        {
            Message =
                $"moved to '{targetName}' of {displays.Count.ToString(CultureInfo.InvariantCulture)} displays; {verdict.Message}",
            Evidence = evidence,
        };
    }

    private static Evidence SaveSamples(ScenarioContext context, IReadOnlyList<JsonElement> samples)
    {
        var path = Path.Combine(context.EvidenceDirectory, "preview.json");
        Directory.CreateDirectory(context.EvidenceDirectory);

        using (var stream = File.Create(path))
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartArray();
            foreach (var sample in samples)
            {
                sample.WriteTo(writer);
            }

            writer.WriteEndArray();
        }

        return Evidence.ForFile("preview", path);
    }
}

/// <summary>
/// REL-DISP-DPI-001: the scaling facts are readable, and the shell honours its
/// declared minimum at the current scale.
/// </summary>
/// <remarks>
/// Per-monitor DPI setting has no documented API, so the 125/150/175/200 per cent
/// sweep stays a human gate. What is automatable is that the scale factors were read
/// at all and that the window is not smaller than the product minimum.
/// </remarks>
public sealed class DisplayScalingGate : IScenarioBody
{
    /// <summary>The product's minimum main-window width, in device-independent pixels.</summary>
    public const int MinimumWidth = 860;

    /// <summary>The product's minimum main-window height, in device-independent pixels.</summary>
    public const int MinimumHeight = 700;

    /// <inheritdoc/>
    public async Task<ScenarioResult> RunAsync(ScenarioContext context, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(context);
        var services = context.RequireServices();
        var session = await services.Sessions.EnsureAsync(cancellationToken).ConfigureAwait(false);

        var environment = await session.EnvironmentSnapshotAsync(cancellationToken).ConfigureAwait(false);
        var windows = await session.WindowsSnapshotAsync(cancellationToken).ConfigureAwait(false);

        var evidence = new[]
        {
            GateEvidence.SaveJson(context, "displays.json", Snapshots.Value(environment.Result, "displays.screens")),
            GateEvidence.SaveJson(context, "windows.json", windows.Result),
        };

        var mainWindows = Snapshots.Items(windows.Result, "windows")
            .Where(window => string.Equals(Snapshots.Text(window, "role"), "main", StringComparison.Ordinal))
            .ToList();
        var main = mainWindows.Count > 0 ? mainWindows[0] : default;
        if (main.ValueKind != JsonValueKind.Object)
        {
            return ScenarioResult.Fail("no main window was reported", evidence);
        }

        // Geometry lives under `native`, and only when a native window exists: the
        // snapshot deliberately never asks for a window handle, because that would
        // create one and the snapshot would have changed what it was observing.
        if (!Snapshots.IsTrue(main, "nativeWindowCreated"))
        {
            return ScenarioResult.InfrastructureError(
                "the main window has no native window yet, so its size cannot be read", evidence);
        }

        var width = (int)(Snapshots.Number(main, "native.width") ?? 0);
        var height = (int)(Snapshots.Number(main, "native.height") ?? 0);

        if (width < MinimumWidth || height < MinimumHeight)
        {
            return ScenarioResult.Fail(
                $"the main window is {Size(width, height)}, below the {Size(MinimumWidth, MinimumHeight)} product minimum",
                evidence);
        }

        var scales = Snapshots.Items(environment.Result, "displays.screens")
            .Select(screen => Snapshots.Number(screen, "devicePixelRatio"))
            .Select(scale => scale?.ToString("0.##", CultureInfo.InvariantCulture) ?? "(not reported)");

        return ScenarioResult.Pass(
            $"device pixel ratios: {string.Join(", ", scales)}; main window {Size(width, height)}", evidence);
    }

    private static string Size(int width, int height) =>
        $"{width.ToString(CultureInfo.InvariantCulture)}x{height.ToString(CultureInfo.InvariantCulture)}";
}

/// <summary>The refresh rate a display transaction should ask for.</summary>
/// <param name="Desired">The desired-state document, empty when none could be chosen.</param>
/// <param name="Refusal">Why none could be chosen, or null.</param>
public sealed record DesiredRefresh(IReadOnlyDictionary<string, string> Desired, string? Refusal);

/// <summary>Chooses display modes a transaction can actually verify.</summary>
public static class DisplayModes
{
    private static readonly ReadOnlyDictionary<string, string> None =
        new(new Dictionary<string, string>(StringComparer.Ordinal));

    /// <summary>The refresh rate to ask this display for, or a refusal naming why there is none.</summary>
    public static async Task<DesiredRefresh> DesiredRefreshAsync(
        GateServices services,
        string deviceAlias,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(services);

        var listed = await services.Envctl.ListModesAsync(deviceAlias, cancellationToken).ConfigureAwait(false);
        if (!listed.Ok)
        {
            return new DesiredRefresh(None, "the display modes could not be enumerated");
        }

        var display = listed.Array("displays").FirstOrDefault();
        if (display.ValueKind != JsonValueKind.Object)
        {
            return new DesiredRefresh(None, $"no display resolved for {deviceAlias}");
        }

        var offered = Snapshots.Items(display, "modes")
            .Select(mode => Snapshots.Number(mode, "refreshHz"))
            .Where(rate => rate is not null)
            .Select(rate => (int)rate!.Value)
            .ToList();
        var current = (int)(Snapshots.Number(display, "current.refreshHz") ?? 0);

        var target = Adapters.Envctl.EnvironmentOrchestrator.SelectUntwinnedRefreshRate(offered, current);
        if (target is null)
        {
            return new DesiredRefresh(
                None, "this display enumerates no alternative refresh rate a read-back could confirm");
        }

        return new DesiredRefresh(
            new ReadOnlyDictionary<string, string>(new Dictionary<string, string>(StringComparer.Ordinal)
            {
                [deviceAlias + ":refresh-hz"] = target.Value.ToString(CultureInfo.InvariantCulture),
            }),
            null);
    }
}
