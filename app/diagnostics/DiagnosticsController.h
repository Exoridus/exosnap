#pragma once

#include "CapabilitySummary.h"
#include "ConfigSummary.h"
#include "DiagnosticResult.h"
#include "PresentProvider.h"
#include "RecommendationEngine.h"
#include "WindowTargetFacts.h"

#include <capability/audio_ui_state.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <recorder_core/pipeline_diagnostics.h>
#include <recorder_core/recorder_session.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Diagnostics presentation policy, extracted out of the Qt Widgets DiagnosticsPage.
//
// Everything here is plain C++ over plain data: no QObject, no QWidget, no QML.
// Both frontends can own one of these; the frontend's only job is to render the
// structs and to feed the blocking probes back in (see DiagnosticsProbe.h) so the
// GUI thread never does file/COM/DXGI work.
namespace exosnap::diagnostics {

// ── Verdict ─────────────────────────────────────────────────────────────────────

enum class VerdictState {
    Neutral,  // nothing measured yet
    Checking, // a check is in flight
    Ready,
    Warn,
    Blocked,
};

// Stable string keys — the QML layer keys tint + glyph off these, never off the
// enum's integer value.
[[nodiscard]] std::string_view VerdictStateKey(VerdictState state) noexcept;

struct Verdict {
    VerdictState state = VerdictState::Neutral;
    std::string headline;
    std::string subline;
    int blockers = 0;
    // Tier-2 measured problems only. Tier-3 optimisations bundle into the tip
    // chip and must never turn the verdict amber (the honesty rail).
    int notices = 0;
    int cap_passes = 0;
};

// ── Issue cards + tips ──────────────────────────────────────────────────────────

enum class IssueTone {
    Pass,
    Notice,
    Blocker,
};

[[nodiscard]] std::string_view IssueToneKey(IssueTone tone) noexcept;

// Mirrors FixAction::Safety as a stable integer for the view layer. Auto renders a
// button that applies after a confirm, Assisted a navigation link, External a bare
// label with no control at all.
enum class FixSafetyKind {
    Auto = 0,
    Assisted = 1,
    External = 2,
};

[[nodiscard]] FixSafetyKind FixKindOf(const FixAction& fix) noexcept;

struct IssueCard {
    std::string id; // mono chip; empty for synthesised cards (profile invalidity, hotkeys)
    IssueTone tone = IssueTone::Notice;
    std::string title;
    std::string summary;
    std::string why;         // L2 "Why" evidence row (DiagnosticResult::recommendation)
    std::string measured;    // L2 "Measured" evidence row (DiagnosticResult::current_value)
    std::string log_excerpt; // L3 collapsed evidence (DiagnosticResult::detail)
    bool needs_elevation = false;
    bool has_fix = false;
    std::string fix_id;
    std::string fix_label;
    std::string fix_changes_summary;
    FixSafetyKind fix_safety = FixSafetyKind::Auto;

    [[nodiscard]] bool has_evidence() const noexcept;

    // Value equality over every field, so a consumer can tell an issue that
    // genuinely changed from the same issue delivered again. The live path
    // rebuilds this list twice a second while recording and almost always
    // rebuilds it identical.
    friend bool operator==(const IssueCard&, const IssueCard&) = default;
};

struct TipEntry {
    std::string id;
    std::string summary;
    bool has_fix = false;
    std::string fix_id;
    std::string fix_label;
    std::string changes;
    FixSafetyKind fix_safety = FixSafetyKind::Auto;
};

struct TopIssues {
    std::vector<IssueCard> cards;
    std::vector<TipEntry> tips;
};

// The card list is capped so the calm surface never becomes a wall of alarm.
inline constexpr int kMaxIssueCards = 6;

// ── Readiness tiles ─────────────────────────────────────────────────────────────

enum class TileTone {
    Neutral,
    Notice,
    Blocker,
};

[[nodiscard]] std::string_view TileToneKey(TileTone tone) noexcept;

struct ReadinessTile {
    std::string key; // "readiness" | "encoder" | "disk" | "display" | "audio" | "target" | "session"
    std::string title;
    std::string value;
    std::string sub;
    TileTone tone = TileTone::Neutral;
    bool has_usage_bar = false;
    int usage_percent = 0;
    // The Readiness tile earns a trailing check glyph only when everything passed.
    bool show_ok_glyph = false;
};

// Everything the tile builder needs, already resolved by the caller. Screen facts
// and the volume total come from Qt GUI / QStorageInfo, which the pure policy must
// not reach for itself.
struct ReadinessTileInputs {
    bool data_ready = false;
    int blockers = 0;
    int notices = 0;
    int cap_passes = 0;

    std::string gpu_adapter_name;
    capability::VideoCodec video_codec = capability::VideoCodec::H264;
    capability::AudioCodec audio_codec = capability::AudioCodec::Aac;
    capability::Container container = capability::Container::Matroska;

    std::optional<uint64_t> free_bytes; // nullopt = volume not queryable
    uint64_t total_bytes = 0;           // 0 = unknown; suppresses the usage bar
    std::string output_drive_label;

    int display_width = 0;
    int display_height = 0;
    int display_refresh_hz = 0;

    int audio_sources = 0;
    uint32_t audio_sample_rate = 0;
    uint32_t audio_channels = 0;

    bool target_selected = false;
    bool target_is_window = false;
    std::string target_description;

    bool has_last_recording = false;
};

[[nodiscard]] std::vector<ReadinessTile> BuildReadinessTiles(const ReadinessTileInputs& inputs);

// ── Fact / configuration tables ─────────────────────────────────────────────────

struct KeyValueRow {
    std::string label;
    std::string value;
};

// ── Self-test ───────────────────────────────────────────────────────────────────

enum class SelfTestState {
    NotRun,
    Pass,
    Warn,
};

[[nodiscard]] std::string_view SelfTestStateLabel(SelfTestState state) noexcept;

struct SelfTestRow {
    std::string title;
    std::string status_text;
    std::string detail;
    IssueTone tone = IssueTone::Pass;
    // Typed, so nothing downstream of this extraction re-sniffs a detail string to
    // decide whether a check actually ran. The one place that maps the runner's
    // "not executed in this build" sentinel onto this flag is BuildSelfTestReport.
    bool not_run = false;
};

struct SelfTestReport {
    SelfTestState state = SelfTestState::NotRun;
    std::vector<SelfTestRow> rows;
};

// ── Pipeline ────────────────────────────────────────────────────────────────────

enum class StageStatus {
    Planned,
    Ok,
    Hotspot,
    Over,
    Unavailable,
};

[[nodiscard]] std::string_view StageStatusKey(StageStatus status) noexcept;

struct PipelineStage {
    std::string key;
    std::string title;
    std::string lane;  // "CPU" / "GPU" / "GPU (NVENC)" / em dash
    std::string value; // measured number, or an em dash
    std::string tip;
    StageStatus status = StageStatus::Planned;

    // So the Quick model can tell "same six stages, one value moved" from "a
    // different pipeline", and skip publishing when nothing moved at all.
    friend bool operator==(const PipelineStage&, const PipelineStage&) = default;
};

// Builds the six pipeline health cards and carries the ONLY piece of state on the
// diagnostics view side: the frame-drop delta baseline.
//
// s.capture.frames_dropped_problem() is a session-cumulative counter, but the
// capture stage's health verdict needs drops SINCE THE LAST SAMPLE. The baseline is
// therefore carried across snapshots and reset whenever session_generation changes
// (a new recording restarts the counter). Losing this state silently reports every
// drop since session start as "recent", which reads as a permanent bottleneck.
class PipelineCardBuilder {
  public:
    [[nodiscard]] std::vector<PipelineStage> BuildLive(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    // Idle / pre-check readiness. Stages 0-2 are only meaningful while recording;
    // 3-5 report the static encoder / muxer / output-path probes.
    [[nodiscard]] static std::vector<PipelineStage> BuildStatic(bool data_ready, bool encoder_ok, bool muxer_ok,
                                                                bool disk_ok);

    // Called when the pipeline leaves the recording/paused lifecycle so the next
    // session starts from a clean baseline rather than inheriting the old one.
    void Reset() noexcept;

    [[nodiscard]] uint32_t lastRecentDropsForTesting() const noexcept;

  private:
    uint64_t last_generation_ = 0;
    uint64_t last_problem_drops_ = 0;
    uint32_t last_recent_drops_ = 0;
    bool seeded_ = false;
};

// Rate limiter for the live re-check rail. The live snapshot arrives at ~5 Hz but
// re-running the recommendation engine at that cadence is wasted work, so both the
// pipeline cards and the honesty rail refresh at most every 500 ms.
class RefreshThrottle {
  public:
    using Clock = std::chrono::steady_clock;

    explicit RefreshThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(500)) noexcept;

    // True when enough time has elapsed since the last allowed tick (and records it).
    [[nodiscard]] bool Allow(Clock::time_point now);
    void Reset() noexcept;

  private:
    std::chrono::milliseconds interval_;
    Clock::time_point last_{};
};

// ── Pure helpers ────────────────────────────────────────────────────────────────

[[nodiscard]] std::string HumanBytes(uint64_t bytes);

// True for checks whose measurement requires the elevated present-path telemetry
// (PresentMon / DPC-ISR ETW). Drives the "Elev" badge so the user knows the
// diagnosis came from the elevated baseline.
[[nodiscard]] bool NeedsElevation(std::string_view id) noexcept;

// Display names carry a backend suffix ("AV1 (NVENC)"); the Encoder and Audio tiles
// add their own backend token, so both need the bare codec name.
[[nodiscard]] std::string StripBackendSuffix(std::string codec);

[[nodiscard]] int CountAvailableCapabilities(const CapabilitySummary& summary) noexcept;

[[nodiscard]] Verdict ComputeVerdict(const DiagnosticChecklist& recommendations, int cap_passes, bool data_ready);

[[nodiscard]] TopIssues BuildTopIssues(const capability::ResolveResult& profile_validation,
                                       const DiagnosticChecklist& recommendations, bool hotkeys_ok,
                                       const std::string& hotkeys_summary);

[[nodiscard]] std::vector<KeyValueRow> BuildEnvironmentRows(const std::vector<DiagnosticResult>& facts, bool elevated);

[[nodiscard]] std::vector<KeyValueRow> BuildConfigRows(const ConfigSummary& summary);

[[nodiscard]] SelfTestReport BuildSelfTestReport(const DiagnosticChecklist& self_test);

// ── Controller ──────────────────────────────────────────────────────────────────

// Result of one honesty-rail pass: everything the view needs, computed once.
struct DiagnosticsSnapshot {
    Verdict verdict;
    std::vector<ReadinessTile> tiles;
    std::vector<IssueCard> cards;
    std::vector<TipEntry> tips;
    std::vector<KeyValueRow> environment_rows;
};

// Owns the diagnostics inputs and produces the view snapshot. Deliberately free of
// any Qt UI type so the same instance could back either frontend.
//
// The controller never performs blocking I/O itself: disk space, filesystem name,
// output-path writability and the self-test checklist are handed in via
// SetProbeResult() from whatever ran DiagnosticsProbe on a worker thread.
class DiagnosticsController {
  public:
    struct Config {
        capability::CapabilitySet caps;
        capability::AudioUiState audio;
        capability::UserRecorderConfig user_config{};
        capability::ResolveResult profile_validation;
        CapabilitySummary cap_summary;
        ConfigSummary config_summary;
        std::string output_folder; // UTF-8 path string; the probe owns the real path
        std::string hotkeys_summary;
        bool hotkeys_ok = false;
    };

    struct ProbeResult {
        std::optional<uint64_t> free_bytes;
        uint64_t total_bytes = 0;
        std::string filesystem_name;
        bool output_path_writable = true;
        std::string drive_label;
        DiagnosticChecklist self_test;
        bool self_test_valid = false;
    };

    struct DisplayFacts {
        int width = 0;
        int height = 0;
        int refresh_hz = 0;
    };

    void SetConfig(Config config);
    void SetProbeResult(ProbeResult probe);
    void SetDisplayFacts(DisplayFacts facts) noexcept;
    void SetSelectedCaptureTarget(std::optional<recorder_core::CaptureTarget> target);
    void SetCaptureWindowEvidence(std::optional<WindowTargetFacts> facts, const WindowHubEvidence& hub);
    void SetSavedDisplayUnresolved(bool unresolved, std::string label);
    void SetElevated(bool elevated) noexcept;
    void SetHasLastRecording(bool has_last_recording) noexcept;
    void SetCaptureTargetHdrActive(bool active) noexcept;
    void SetDpcLatency(DpcLatencyReading reading);
    void SetPresentSample(std::optional<PresentSample> sample);
    void SetLiveSnapshot(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    // The structured checklist the last Evaluate() produced, and the environment
    // facts alongside it. Retained rather than rebuilt on demand: running the
    // recommendation engine a second time to answer a query would re-read the
    // live snapshot at a different instant, so a consumer could see a verdict the
    // surface never showed. Empty before the first Evaluate().
    [[nodiscard]] const DiagnosticChecklist& lastChecklist() const noexcept;
    [[nodiscard]] const std::vector<DiagnosticResult>& lastEnvironmentFacts() const noexcept;
    // The self-test checklist as the probe produced it, and whether it ran at all
    // ("not executed in this build" is a real answer, not an empty list).
    [[nodiscard]] const DiagnosticChecklist& selfTestChecklist() const noexcept;
    [[nodiscard]] bool selfTestValid() const noexcept;
    // The last live pipeline snapshot fed in by the recording path. This is a
    // pass-through of the engine's own value -- the controller neither smooths
    // nor re-derives it.
    [[nodiscard]] const recorder_core::RecordingDiagnosticsSnapshot& liveSnapshot() const noexcept;

    [[nodiscard]] bool dataReady() const noexcept;
    [[nodiscard]] bool hasLastRecording() const noexcept;
    [[nodiscard]] bool elevated() const noexcept;
    [[nodiscard]] const std::string& outputFolder() const noexcept;
    [[nodiscard]] const SelfTestReport& selfTest() const noexcept;
    [[nodiscard]] const std::vector<KeyValueRow>& configRows() const noexcept;

    // One honesty-rail pass. Safe to call before any data arrives: returns the
    // "not checked yet" snapshot rather than fabricating a verdict.
    [[nodiscard]] DiagnosticsSnapshot Evaluate();

    // Live pipeline stages for the current snapshot. Returns the static readiness
    // set while idle, and carries the frame-drop delta across live samples.
    [[nodiscard]] std::vector<PipelineStage> BuildPipelineStages();

    // True while the last live snapshot is in the recording or paused lifecycle.
    [[nodiscard]] bool liveRecording() const noexcept;

  private:
    Config config_;
    ProbeResult probe_;
    DisplayFacts display_{};
    std::optional<recorder_core::CaptureTarget> selected_target_;
    std::optional<WindowTargetFacts> capture_window_facts_;
    WindowHubEvidence capture_window_hub_;
    bool saved_display_unresolved_ = false;
    std::string saved_display_label_;
    bool elevated_ = false;
    bool has_last_recording_ = false;
    bool capture_target_hdr_active_ = false;
    bool data_ready_ = false;
    std::optional<DpcLatencyReading> dpc_;
    std::optional<PresentSample> present_;
    recorder_core::RecordingDiagnosticsSnapshot live_{};
    SelfTestReport self_test_;
    std::vector<KeyValueRow> config_rows_;
    // What the last Evaluate() computed, kept so the structured surface and the
    // rendered surface answer from one pass.
    DiagnosticChecklist last_checklist_;
    std::vector<DiagnosticResult> last_facts_;
    PipelineCardBuilder pipeline_;
};

} // namespace exosnap::diagnostics
