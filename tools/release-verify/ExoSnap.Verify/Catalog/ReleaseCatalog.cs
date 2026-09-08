using System.Collections.ObjectModel;
using ExoSnap.Verify.Capabilities;
using ExoSnap.Verify.Engine;
using ExoSnap.Verify.Gates;
using ExoSnap.Verify.Models;

namespace ExoSnap.Verify.Catalog;

/// <summary>
/// The release scenario catalog.
/// </summary>
/// <remarks>
/// One entry per release gate, declaring what it needs and what it proves. Two
/// conventions the whole catalog depends on.
///
/// Aliases, not names. A scenario needing a specific display asks for
/// <c>device.display.main-hdr</c>, never for a monitor model. A scenario that
/// names one monitor is a scenario that can only ever run at one desk, and a
/// friendly name is not stable even there, because two identical panels share one.
///
/// Structured state, not screen text. Every product assertion goes through a
/// typed surface. The visual scenarios are the deliberate exception: a person is
/// judging real desktop composition that no screenshot of ours can capture,
/// because the overlays in question defeat every capture path by design.
///
/// A scenario whose body has not been written yet carries <see cref="NotMigratedBody"/>
/// and reports Skipped rather than Pass: a gate that has not been written must never
/// look like a gate that ran.
/// </remarks>
public static class ReleaseCatalog
{
    /// <summary>Builds the catalog, pairing each declaration with the body it has.</summary>
    public static ScenarioCatalog Create() =>
        new(Descriptors().Select(descriptor => new Scenario(descriptor, BodyFor(descriptor.Id))));

    /// <summary>
    /// The scenario ids whose bodies exist. Everything else in the catalog is a
    /// declaration waiting for one.
    /// </summary>
    public static ReadOnlyCollection<string> MigratedIds() => new(
    [
        .. Descriptors()
            .Select(descriptor => descriptor.Id)
            .Where(id => BodyFor(id) is not NotMigratedBody),
    ]);

    private static IScenarioBody BodyFor(string id) => id switch
    {
        "REL-ENV-001" => new EnvironmentClassificationGate(),
        "REL-ENV-002" => new DeviceAliasGate(),
        "REL-ENV-003" => new EnvironmentMutationGate(),
        "REL-SCHEMA-001" => new FieldContractGate(),
        "REL-PRESENT-001" => new UnelevatedPresentGate(),
        "REL-PRESENT-XCHECK-001" => new PresentCrossCheckGate(),
        "REL-CAP-001" => new RecordingProducedGate(),
        "REL-CAP-QUIET-001" => new QuietStallGate(),
        "REL-AUD-CLOCK-001" => new AudioClockSoakGate(),
        "REL-DISP-REFRESH-001" => new DisplayRefreshGate(),
        "REL-DISP-HDR-001" => new DisplayHdrGate(),
        "REL-DISP-MIXED-001" => new MixedDisplayGate(),
        "REL-DISP-DPI-001" => new DisplayScalingGate(),
        "REL-UPD-PORTABLE-001" => new PortableUpdateGate(),
        "REL-JOURNEY-001" => new ProductJourneyGate(),
        "REL-SHUTDOWN-001" => new ShutdownGate(),
        _ => new NotMigratedBody(),
    };

    /// <summary>
    /// The scenarios a release campaign must have passing before promotion. Opt-in
    /// scenarios are not on this list: they are long or physically disruptive, and
    /// a campaign selects them explicitly.
    /// </summary>
    public static ReadOnlyCollection<string> RequiredIds() =>
        new([.. Descriptors().Where(descriptor => !descriptor.OptIn).Select(descriptor => descriptor.Id)]);

    /// <summary>Every descriptor, in catalog order.</summary>
    public static ReadOnlyCollection<ScenarioDescriptor> Descriptors() => new(
    [
        Describe(
            id: "REL-ENV-001",
            title: "Environment capability classification is recorded",
            scenarioClass: "environment",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            oracle: ["envctl"],
            source: "ADR 0069 (environment orchestration)"),

        Describe(
            id: "REL-ENV-002",
            title: "Device aliases resolve to stable identifiers, unambiguously",
            scenarioClass: "environment",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            oracle: ["envctl"],
            source: "ADR 0069",
            dependsOn: ["REL-ENV-001"]),

        Describe(
            id: "REL-ENV-003",
            title: "A real mutation is applied, verified and restored exactly",
            scenarioClass: "environment",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.Automated,
            requires: [Device("display.main-hdr")],
            mutates: ["display.main-hdr:refresh-hz"],
            oracle: ["envctl"],
            source: "ADR 0069 (write, read back, compare; exact restore)",
            dependsOn: ["REL-ENV-002"]),

        Describe(
            id: "REL-SCHEMA-001",
            title: "Every field path the catalog reads actually exists",
            scenarioClass: "schema",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Hermetic,
            interaction: ScenarioInteraction.Automated,
            oracle: ["exosnap"],
            source: "Wave D review: scenarios that read fields no emitter emits"),

        Describe(
            id: "REL-PRESENT-001",
            title: "Unelevated present diagnostics explain themselves and open no session",
            scenarioClass: "present",
            layer: ScenarioLayer.ControlChannel,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.Elevated, "false")],
            oracle: ["exosnap"],
            source: "ADR 0033"),

        Describe(
            id: "REL-PRESENT-002",
            title: "Elevated present diagnostics report real presents",
            scenarioClass: "present",
            layer: ScenarioLayer.Secure,
            isolation: ScenarioIsolation.HardwareLab,
            privilege: ScenarioPrivilege.Elevated,
            interaction: ScenarioInteraction.OperatorAssisted,
            requires: [CapabilityRequirement.Is(CapabilityKeys.GpuD3D11, "true")],
            oracle: ["exosnap", "presentmon"],
            source: "ADR 0033; docs/release-checklist.md section 7 (present-mode diagnostics)"),

        Describe(
            id: "REL-PRESENT-XCHECK-001",
            title: "An independent observer confirms the presentation path ExoSnap reports",
            scenarioClass: "present",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.DisposableOs,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.PresentMonAvailable, "true")],
            oracle: ["exosnap", "presentmon"],
            source: "ADR 0070 (PresentMon as an independent oracle; required only when the " +
                    "present-diagnostics code or the Windows major version has moved)"),

        Describe(
            id: "REL-CAP-001",
            title: "A recording is produced and validated by an independent tool",
            scenarioClass: "capture",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.FfprobeAvailable, "true")],
            oracle: ["exosnap", "ffprobe"],
            source: "docs/release-checklist.md section 7"),

        Describe(
            id: "REL-CAP-STALL-001",
            title: "A stalled window capture is reported honestly and stays controllable",
            scenarioClass: "capture",
            layer: ScenarioLayer.SemiAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.OperatorAssisted,
            oracle: ["exosnap", "ffprobe"],
            optIn: true,
            source: "docs/release-checklist.md section 7"),

        Describe(
            id: "REL-CAP-QUIET-001",
            title: "A minimized window stalls silently and the recording carries on",
            scenarioClass: "capture",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            oracle: ["exosnap", "ffprobe"],
            optIn: true,
            source: "docs/product-spec.md (capture stall)"),

        Describe(
            id: "REL-CAP-FSE-001",
            title: "True exclusive fullscreen is detected and explained",
            scenarioClass: "capture",
            layer: ScenarioLayer.SemiAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.OperatorAssisted,
            requires: [CapabilityRequirement.Is(CapabilityKeys.GpuD3D11, "true")],
            oracle: ["exosnap", "presentmon"],
            optIn: true,
            source: "docs/superpowers/specs/2026-07-11-exclusive-fullscreen-capture-spec.md"),

        Describe(
            id: "REL-AUD-DEGRADE-001",
            title: "Losing an audio endpoint mid-recording degrades to honest silence and recovers",
            scenarioClass: "audio-physical",
            layer: ScenarioLayer.ManualPhysical,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.OperatorAssisted,
            requires: [Device("audio.render.normal")],
            mutates: ["audio.render.normal:endpoint-state"],
            oracle: ["exosnap", "wasapi", "ffprobe"],
            optIn: true,
            source: "ADR 0046; docs/release-checklist.md section 7"),

        Describe(
            id: "REL-AUD-SILENCE-001",
            title: "A silent but connected source is not reported as degraded",
            scenarioClass: "audio",
            layer: ScenarioLayer.SemiAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.OperatorAssisted,
            oracle: ["exosnap", "ffprobe"],
            optIn: true,
            source: "ADR 0046 (degradation is device loss, not quiet)"),

        Describe(
            id: "REL-AUD-FORMAT-001",
            title: "Recording on a 44.1 kHz endpoint produces correct audio",
            scenarioClass: "audio",
            layer: ScenarioLayer.SemiAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.OperatorAssisted,
            requires:
            [
                Device("audio.render.44100-test"),
                CapabilityRequirement.Is(CapabilityKeys.AudioEndpoint(44100), "true"),
            ],
            mutates: ["audio.render.44100-test:device-format"],
            oracle: ["exosnap", "wasapi", "ffprobe"],
            optIn: true,
            source: "docs/release-checklist.md section 7 (44.1 kHz output device)"),

        Describe(
            id: "REL-AUD-CLOCK-001",
            title: "A long mixed-clock recording stays in sync",
            scenarioClass: "audio-long",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.FfprobeAvailable, "true")],
            oracle: ["exosnap", "ffprobe"],
            optIn: true,
            source: "docs/release-checklist.md section 7 (long-duration soak, clock slaving)"),

        Describe(
            id: "REL-DISP-REFRESH-001",
            title: "Recording is correct across the display refresh rates this machine offers",
            scenarioClass: "display",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.Automated,
            requires: [Device("display.main-hdr")],
            mutates: ["display.main-hdr:refresh-hz"],
            oracle: ["envctl", "exosnap", "ffprobe"],
            optIn: true,
            source: "docs/release-checklist.md section 7"),

        Describe(
            id: "REL-DISP-HDR-001",
            title: "HDR state is applied, recorded against, and restored exactly",
            scenarioClass: "display",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.Automated,
            requires:
            [
                Device("display.main-hdr"),
                CapabilityRequirement.Is(CapabilityKeys.GpuD3D11, "true"),
            ],
            mutates: ["display.main-hdr:hdr"],
            oracle: ["envctl", "dxgi", "exosnap", "ffprobe"],
            optIn: true,
            source: "docs/release-checklist.md section 7"),

        Describe(
            id: "REL-DISP-MIXED-001",
            title: "Capture works on each display of a mixed HDR/SDR desktop",
            scenarioClass: "display",
            layer: ScenarioLayer.ControlChannel,
            isolation: ScenarioIsolation.HardwareLab,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.DisplayHdr, "true")],
            oracle: ["dxgi", "exosnap"],
            optIn: true,
            source: "docs/release-checklist.md section 7"),

        Describe(
            id: "REL-DISP-DPI-001",
            title: "Scaling facts are read and the minimum window size stays usable",
            scenarioClass: "display",
            layer: ScenarioLayer.ControlChannel,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            oracle: ["exosnap"],
            source: "docs/product-spec.md (minimum window size)"),

        Describe(
            id: "REL-VIS-OVERLAY-001",
            title: "Capture-excluded overlays stay fixed-dark under both Windows appearances",
            scenarioClass: "visual",
            layer: ScenarioLayer.ManualVisual,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.OperatorJudged,
            requires: [CapabilityRequirement.Is(CapabilityKeys.InteractiveDesktop, "true")],
            mutates: ["system:apps-theme"],
            oracle: ["operator"],
            optIn: true,
            source: "docs/product-spec.md (fixed-dark capture overlays)"),

        Describe(
            id: "REL-VIS-NOTIFY-001",
            title: "Desktop notifications render with the right severity glyph",
            scenarioClass: "visual",
            layer: ScenarioLayer.ManualVisual,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.OperatorJudged,
            requires: [CapabilityRequirement.Is(CapabilityKeys.InteractiveDesktop, "true")],
            oracle: ["operator"],
            optIn: true,
            source: "docs/product-spec.md (notification severity)"),

        Describe(
            id: "REL-UPD-PORTABLE-001",
            title: "The portable update handoff installs the version it pinned",
            scenarioClass: "update",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.DisposableOs,
            interaction: ScenarioInteraction.Automated,
            oracle: ["exosnap", "filesystem"],
            optIn: true,
            source: "ADR 0068; docs/release-checklist.md section 5"),

        Describe(
            id: "REL-UPD-MSI-DECLINE-001",
            title: "Declining the elevation prompt yields a truthful cancel state",
            scenarioClass: "update",
            layer: ScenarioLayer.Secure,
            isolation: ScenarioIsolation.DisposableOs,
            privilege: ScenarioPrivilege.SecureDesktop,
            interaction: ScenarioInteraction.OperatorAssisted,
            oracle: ["exosnap", "filesystem"],
            optIn: true,
            source: "ADR 0067 (cancel is not failure)"),

        Describe(
            id: "REL-UPD-MSI-001",
            title: "The MSI update elevates, installs and relaunches",
            scenarioClass: "update",
            layer: ScenarioLayer.Secure,
            isolation: ScenarioIsolation.DisposableOs,
            privilege: ScenarioPrivilege.SecureDesktop,
            interaction: ScenarioInteraction.OperatorAssisted,
            oracle: ["exosnap", "msi", "filesystem"],
            optIn: true,
            source: "docs/release-checklist.md sections 5 and 7a"),

        Describe(
            id: "REL-PKG-CHOCO-001",
            title: "The Chocolatey package installs, uninstalls and leaves the machine as it was",
            scenarioClass: "packaging",
            layer: ScenarioLayer.Secure,
            isolation: ScenarioIsolation.DisposableOs,
            privilege: ScenarioPrivilege.SecureDesktop,
            interaction: ScenarioInteraction.OperatorAssisted,
            requires: [CapabilityRequirement.Is(CapabilityKeys.SandboxAvailable, "true")],
            oracle: ["chocolatey", "filesystem", "registry"],
            optIn: true,
            source: "docs/release-checklist.md section 8 (Chocolatey)"),

        Describe(
            id: "REL-JOURNEY-001",
            title: "The full product journey, launch to export to restart",
            scenarioClass: "journey",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            requires: [CapabilityRequirement.Is(CapabilityKeys.FfprobeAvailable, "true")],
            oracle: ["exosnap", "ffprobe"],
            source: "Wave C product journey"),

        Describe(
            id: "REL-SHUTDOWN-001",
            title: "Unread control-channel events never hold the application open",
            scenarioClass: "journey",
            layer: ScenarioLayer.FullAuto,
            isolation: ScenarioIsolation.Desktop,
            interaction: ScenarioInteraction.Automated,
            oracle: ["exosnap"],
            source: "ADR 0067 (no flush-on-stop dependency)"),
    ]);

    private static CapabilityRequirement Device(string alias) =>
        CapabilityRequirement.Is(CapabilityKeys.Device(alias), CapabilityKeys.Bound);

    private static ScenarioDescriptor Describe(
        string id,
        string title,
        string scenarioClass,
        ScenarioLayer layer,
        ScenarioIsolation isolation,
        ScenarioInteraction interaction,
        string source,
        IReadOnlyList<CapabilityRequirement>? requires = null,
        ScenarioPrivilege privilege = ScenarioPrivilege.Standard,
        IReadOnlyList<string>? mutates = null,
        IReadOnlyList<string>? oracle = null,
        bool optIn = false,
        IReadOnlyList<string>? dependsOn = null) =>
        new(
            id,
            title,
            scenarioClass,
            layer,
            new ReadOnlyCollection<CapabilityRequirement>([.. requires ?? []]),
            isolation,
            privilege,
            interaction,
            new ReadOnlyCollection<string>([.. mutates ?? []]),
            new ReadOnlyCollection<string>([.. oracle ?? []]),
            optIn,
            source,
            new ReadOnlyCollection<string>([.. dependsOn ?? []]));
}
