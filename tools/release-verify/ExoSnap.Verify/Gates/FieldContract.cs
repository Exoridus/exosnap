using System.Collections.ObjectModel;

namespace ExoSnap.Verify.Gates;

/// <summary>What the product must be doing for a field path to exist at all.</summary>
public enum ContractStage
{
    /// <summary>Readable at any time.</summary>
    Idle,

    /// <summary>
    /// A recording is running. The pipeline's measurement groups are absent while
    /// <c>valid</c> is false, by design: emitting them idle would hand a reader a
    /// complete, entirely zero pipeline that reads as a healthy recording.
    /// </summary>
    Recording,

    /// <summary>After a recording has completed.</summary>
    Result,
}

/// <summary>One field path the catalog reads, and who reads it.</summary>
/// <param name="Command">The control-channel command that emits it.</param>
/// <param name="Stage">What the product must be doing for it to exist.</param>
/// <param name="Path">The dotted path; <c>[]</c> after a segment means "and then its first element".</param>
/// <param name="UsedBy">The gates that depend on it.</param>
public sealed record ContractEntry(string Command, ContractStage Stage, string Path, string UsedBy);

/// <summary>
/// Every control-channel field path the catalog reads.
/// </summary>
/// <remarks>
/// The contract between this catalog and the product's emitters, written down in one
/// place so one unattended pass can check all of it.
///
/// A path is listed here because a gate depends on it, and <c>UsedBy</c> is not
/// decoration: when a path disappears it names, without searching, exactly which gates
/// are about to throw - including the ones that would otherwise only throw after a
/// person had already unplugged something.
/// </remarks>
public static class FieldContract
{
    /// <summary>Every entry, grouped by the command that emits it.</summary>
    public static ReadOnlyCollection<ContractEntry> Entries() => new(
    [
        .. Group("app.identity", ContractStage.Idle, "REL-UPD-MSI-001, REL-PRESENT-002",
            "productVersion", "executableSha256"),

        .. Group("environment.snapshot", ContractStage.Idle, "REL-PRESENT-001, REL-PRESENT-002",
            "present.optIn", "present.elevated", "present.available", "present.availability"),

        .. Group("environment.snapshot", ContractStage.Idle,
            "REL-DISP-HDR-001, REL-DISP-MIXED-001, REL-DISP-DPI-001",
            "displays.screens[].name", "displays.screens[].primary",
            "displays.screens[].hdrActive", "displays.screens[].devicePixelRatio"),

        .. Group("windows.snapshot", ContractStage.Idle, "REL-DISP-DPI-001",
            "windows[].role", "windows[].nativeWindowCreated"),

        // The repaint bookkeeping is a group of its own: `owed` at the top level would
        // be a claim about the preview, and it is a claim about the update gate that
        // drives it.
        .. Group("preview.snapshot", ContractStage.Idle, "REL-DISP-MIXED-001",
            "active", "frameReady", "updateGate.owed", "updateGate.renderPasses"),

        .. Group("record.snapshot", ContractStage.Idle,
            "REL-AUD-DEGRADE-001, REL-AUD-SILENCE-001, REL-AUD-FORMAT-001",
            "systemAudioEnabled"),

        .. Group("overlay.snapshot", ContractStage.Idle, "REL-VIS-OVERLAY-001", "overlays[].visible"),

        .. Group("notifications.snapshot", ContractStage.Idle, "REL-CAP-STALL-001, REL-VIS-NOTIFY-001",
            "entries[].sequence", "entries[].title", "entries[].body"),

        // `installState` and `phase` are the updater's vocabulary, not the app's. The
        // app reports its own update state machine plus the endpoint of the child it
        // launched; whoever wants the updater's answer connects to that child.
        .. Group("update.getState", ContractStage.Idle, "REL-UPD-MSI-001, REL-UPD-MSI-DECLINE-001",
            "updateAvailable", "state", "blocker", "currentVersion",
            "updaterLaunch.controlRunId", "updaterLaunch.controlPipe"),

        .. Group("pipeline.snapshot", ContractStage.Recording,
            "REL-CAP-001, REL-CAP-STALL-001, REL-AUD-DEGRADE-001", "valid", "lifecycle"),

        .. Group("pipeline.snapshot", ContractStage.Recording, "REL-DISP-REFRESH-001", "capture.actualFps"),

        .. Group("pipeline.snapshot", ContractStage.Recording, "REL-CAP-STALL-001",
            "sourcePresentation.presentMode", "sourcePresentation.modeAvailability"),

        .. Group("pipeline.snapshot", ContractStage.Recording, "REL-AUD-DEGRADE-001, REL-AUD-SILENCE-001",
            "audio.active", "audio.sourceDegraded", "audio.degradedSources"),

        .. Group("pipeline.snapshot", ContractStage.Recording, "REL-AUD-CLOCK-001",
            "avTiming.avDriftMs", "avTiming.avDriftAvailability"),

        .. Group("record.result", ContractStage.Result,
            "REL-CAP-001, REL-AUD-FORMAT-001, REL-AUD-CLOCK-001, REL-DISP-HDR-001",
            "succeeded", "outputPath"),
    ]);

    private static IEnumerable<ContractEntry> Group(
        string command,
        ContractStage stage,
        string usedBy,
        params string[] paths) =>
        paths.Select(path => new ContractEntry(command, stage, path, usedBy));
}
