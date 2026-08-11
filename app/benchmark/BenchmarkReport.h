#pragma once

// Frontend-neutral benchmark run metadata and report writer.
//
// One writer for both frontends. A run that goes through here cannot produce a
// report the comparison tooling has to guess at: the environment, the exact
// recording configuration and the comparability class of every metric are all
// part of the emitted document. "Do not require manually reconstructed metadata
// later" is the requirement this file exists to satisfy.

#include "BenchmarkEffectiveConfig.h"
#include "BenchmarkMetrics.h"

#include <QString>

namespace exosnap::benchmark {

// Which frontend produced the run. Written into the report and into the
// deterministic file name, so an archived artifact identifies itself.
enum class Frontend : uint8_t {
    Widgets,
    Quick,
    // No UI at all: the headless bare-mode drive loop, which owns a coordinator
    // directly and shows no window. Not a competitor in the A/B — it is the floor
    // the two frontends are measured against, i.e. what the recording costs when
    // nothing is previewing it. Its preview metrics are always unavailable.
    Headless,
};

[[nodiscard]] const char* FrontendName(Frontend value) noexcept;
[[nodiscard]] bool FrontendFromString(const QString& text, Frontend* out);

// Machine and build facts. Collected once per process; none of it depends on
// which frontend is running, which is what makes a cross-frontend delta
// meaningful in the first place.
struct Environment {
    // Build identity. dirty_source_tree matters more than usual here: the Qt Quick
    // migration is uncommitted, so a report claiming a bare commit SHA without the
    // dirty flag would misidentify what was actually measured.
    QString app_version;
    QString git_commit;
    QString git_commit_full;
    bool dirty_source_tree = false;
    bool official_build = false;
    QString build_config; // "Debug" / "Release"

    QString qt_version_runtime;
    QString qt_version_compiled;

    QString os_version; // e.g. "Windows 10.0.26200"
    QString os_edition; // registry product name, when readable
    QString cpu_name;
    int cpu_logical_processors = 0;

    QString gpu_description;
    uint32_t gpu_vendor_id = 0;
    uint32_t gpu_device_id = 0;
    uint64_t gpu_dedicated_vram_mb = 0;
    // User-mode display driver version (DXGI CheckInterfaceSupport), which is what
    // vendor control panels report as "driver version". Empty when unavailable.
    QString gpu_driver_umd_version;

    // The screen the run actually happened on.
    int display_width = 0;
    int display_height = 0;
    double display_refresh_hz = 0.0;
    double display_device_pixel_ratio = 0.0;
    QString display_name;

    // Authoritative monitor identity, straight out of the product's own resolver
    // (services/DisplayIdentityEnumerator, i.e. QueryDisplayConfig +
    // DisplayConfigGetDeviceInfo).
    //
    // It is here so the ARTIFACT proves which physical panel was captured. The
    // alternative — an external script correlating the Win32 screen list with the
    // WMI monitor list — was tried and got it exactly backwards: the two
    // enumerations have no defined ordering relationship, and on this machine
    // their orders are reversed. DisplayConfig returns the source-to-target LINK
    // itself, so no correlation has to be invented. `gdi_name` is the same
    // "\\.\DISPLAY1" string the capture target carries, which is what ties a run's
    // capture_target_description to a real panel.
    struct DisplayFacts {
        QString gdi_name;
        QString friendly_name;
        QString device_path;
        QString edid_vendor;
        QString serial;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool primary = false;
    };
    QList<DisplayFacts> displays;

    // Wall-clock start of the run, ISO 8601 local time with offset.
    QString timestamp;
};

[[nodiscard]] Environment CollectEnvironment();

// Descendant native windows under `top_level` (an HWND, passed untyped so this
// header stays free of windows.h). Returns -1 when the handle is null.
//
// The count is the structural difference the whole migration is about — the
// Widgets preview is a native child window, the Quick preview is a scene-graph
// item — so every benchmark report carries it, and both entry points read it
// from here rather than each keeping a private copy of the enumeration.
[[nodiscard]] int CountChildWindows(void* top_level);

// Where a harness run puts its frontend window, and how big.
//
// Both frontends resolve it from here rather than each carrying its own rule.
// The canonical A/B keeps the application VISIBLE on the secondary display while
// the primary one carries the captured workload, and it compares two windows of
// the same logical size — two frontends placed by two slightly different
// open-coded rules would silently be rendering different areas.
struct HarnessWindowPlacement {
    int x = 0;
    int y = 0;
    int width = 1280;
    int height = 820;
    // Empty when no screen could be resolved (headless); the caller then leaves
    // the window where it is rather than moving it to a made-up origin.
    QString screen_name;
    bool on_secondary_screen = false;
};

[[nodiscard]] HarnessWindowPlacement ResolveHarnessWindowPlacement();

// The exact scenario the run executed. Everything a repeat of this run would
// need, and everything a reader needs to know two runs are comparable.
struct RunConfig {
    Frontend frontend = Frontend::Quick;
    QString scenario;    // e.g. "desktop-idle-1440p144"
    int run_index = 1;   // 1-based repetition within the scenario
    int repetitions = 1; // total repetitions requested

    int warmup_seconds = 0;
    int measured_seconds = 0;

    QString capture_target_kind; // "monitor" / "window"
    QString capture_target_description;

    int requested_fps = 0;
    QString container;
    QString video_codec;
    QString audio_codec;
    int chroma = 0;
    int bit_depth = 0;
    QString hdr_mode;
    QString audio_rows;

    // Free-form notes the operator supplies for an externally driven scenario
    // (game title, its graphics preset, whether frame generation was on). The
    // report must be able to state frame-generation status, and no in-process
    // probe can know it.
    QString source_notes;
};

// What the recording itself produced.
struct RunOutcome {
    bool succeeded = false;
    QString output_path;
    qint64 output_file_bytes = 0;
    double media_duration_seconds = 0.0;
    int output_width = 0;
    int output_height = 0;
    QString error_phase;
    QString error_detail;
};

// Process CPU/memory cost over the measured window.
//
// Exists so the percentage is computed in exactly one place. Three copies of this
// arithmetic had grown across the tree (the Widgets auto-record report and both
// ad-hoc Quick benchmark paths), which is precisely the kind of duplication that
// makes a cross-frontend delta untrustworthy.
class ProcessSampler {
  public:
    // Call when the warm-up ends and the measured window begins.
    void Start() noexcept;

    // `elapsed_seconds` is the measured window, not the total run. Returns
    // unavailable metrics when Start() was never reached (e.g. the recording
    // never entered Recording), rather than a zero that reads as "no cost".
    [[nodiscard]] ProcessMetrics Sample(double elapsed_seconds) const;

  private:
    uint64_t cpu_100ns_at_start_ = 0;
    bool started_ = false;
};

// Writes the complete report as JSON. Returns false on any I/O failure; the
// caller is expected to surface that rather than treat a missing file as a
// zero-cost run.
// `effective` is the configuration the engine was actually handed. It is a
// required argument rather than an optional extra: a report without it cannot
// establish that two runs are comparable, which is the one thing the A/B needs
// before any delta means anything.
[[nodiscard]] bool WriteReport(const QString& path, const Environment& environment, const RunConfig& config,
                               const EffectiveRecordingConfig& effective, const RunOutcome& outcome,
                               const PreviewMetrics& preview, const RecordingMetrics& recording,
                               const ProcessMetrics& process);

// Deterministic artifact base name: "<frontend>-<scenario>-run<NN>".
// Used for the metrics JSON, the recording, and the run log so an archived set
// is self-describing without a side-channel note.
[[nodiscard]] QString ArtifactBaseName(const RunConfig& config);

} // namespace exosnap::benchmark
