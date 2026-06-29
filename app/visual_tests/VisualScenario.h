#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include "models/OutputSettingsModel.h"
#include <capability/config_types.h>

namespace exosnap::visual {

enum class VisualPage {
    Record,
    Settings,
    Webcam,
    Hotkeys,
    Diagnostics,
    Logs,
    About,
    EditExport,
};

enum class VisualRecordState {
    None,
    Ready,
    Countdown,
    Recording,
    Paused,
    Completed,
};

enum class VisualSettingsTarget {
    None,
    Display,
    Window,
    Region,
};

enum class VisualSourcePickerTab {
    None,
    Screens,
    Windows,
    Region,
};

enum class VisualWebcamState {
    None,
    Active,
    Unavailable,
};

enum class VisualRegionState {
    None,
    Empty,
    Selected,
    Editing,
    Preset16x9,
    Preset9x16,
    Invalid,
};

enum class VisualRegionEditMode {
    None,
    Move,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight,
};

// Active webcam PiP drag handle (for deterministic interaction scenarios).
enum class VisualWebcamHandle {
    None,
    Move,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomLeft,
    ResizeBottomRight,
};

enum class VisualLogFilter {
    All,
    Info,
    Issues,
};

struct VisualMask {
    QString object_name;
    QString reason;
};

struct VisualScenario {
    QString id;
    QString title;
    VisualPage page = VisualPage::Record;
    VisualRecordState record_state = VisualRecordState::None;
    VisualSettingsTarget settings_target = VisualSettingsTarget::None;
    VisualSourcePickerTab source_picker_tab = VisualSourcePickerTab::None;
    VisualWebcamState webcam_state = VisualWebcamState::None;
    QVector<VisualMask> masks;
    int countdown_seconds = 0;
    int countdown_remaining = 0;
    VisualRegionState region_state = VisualRegionState::None;
    VisualRegionEditMode region_edit_mode = VisualRegionEditMode::None;
    int region_x = 640;
    int region_y = 360;
    int region_width = 1280;
    int region_height = 720;

    // --- Webcam PiP / mirror (Record preview + Settings webcam card) ---
    // Trailing fields so existing positional initializers stay valid.
    bool webcam_pip_enabled = false;
    bool webcam_mirror = false;
    bool webcam_pip_selected = false;
    bool webcam_pip_edit_locked = false;
    VisualWebcamHandle webcam_handle = VisualWebcamHandle::None;
    float webcam_x = 0.70f;
    float webcam_y = 0.70f;
    float webcam_w = 0.25f;
    float webcam_h = 0.25f;

    VisualLogFilter log_filter = VisualLogFilter::All;
    QString log_search_query;
    bool log_auto_scroll = true;

    // Preset card (Settings) — synthetic state for deterministic scenarios.
    // preset_count == 0 means "leave the card untouched".
    int preset_count = 0;
    QString preset_selected_name;   // selected preset display name
    QString preset_default_name;    // startup-default preset name (for the badge)
    bool preset_dirty = false;      // unsaved-changes indicator
    bool preset_menu_open = false;  // open the Manage overflow menu
    bool preset_save_error = false; // render an inline save/name error affordance

    // Output scaling / format scenarios (OUTPUT-SCALING-R1 + FORMAT-CONTROLS-R1).
    OutputResolutionMode output_resolution_mode = OutputResolutionMode::Native;
    int requested_width = 2560;
    int requested_height = 1440;
    int effective_width = 2560;
    int effective_height = 1440;
    int source_width = 2560;
    int source_height = 1440;
    int content_x = 0;
    int content_y = 0;
    int content_width = 2560;
    int content_height = 1440;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    bool cfr = true;
    capability::Container container = capability::Container::WebM;
    capability::VideoCodec video_codec = capability::VideoCodec::Av1Nvenc;
    capability::AudioCodec audio_codec = capability::AudioCodec::Opus;
    QString reconciliation_warning;
    bool controls_locked = false;

    // Source picker reactive refresh scenarios
    int source_display_count = -1;    // synthetic: count of displays in picker
    int source_window_count = -1;     // synthetic: count of windows in picker
    QString source_selected_identity; // title of selected source
    bool source_selected_available = true;
    bool source_refresh_active = false; // refresh timer is running
    int source_refresh_generation = 0;  // monotonic generation counter

    // --- Completed result scenarios (OUTPUT-RESULTS-R1) ---
    QString result_file_name = QStringLiteral("visual-test-recording.webm");
    uint64_t result_file_size_bytes = 52ULL * 1024ULL * 1024ULL;
    double result_duration_seconds = 83.0;
    bool result_file_exists = true;
    int recent_result_count = 0;
    bool show_delete_confirm = false;
    bool show_rename_overlay = false;

    // --- Device discovery scenarios (DEVICE-DISCOVERY-R1) ---
    // Sentinels: audio_input_count / audio_output_count / webcam_count /
    // display_count == -1 means "not applicable" (scenario does not drive discovery).
    // selected_*_stable_id == QString()  means not applicable / not set.
    int dd_audio_input_count = -1;                    // total mic/input endpoints in snapshot
    int dd_audio_output_count = -1;                   // total render/output endpoints in snapshot
    QString dd_selected_mic_stable_id;                // stable id of configured mic (empty = not applicable)
    bool dd_selected_mic_available = true;            // true when configured mic is present in snapshot
    bool dd_selected_output_semantic_default = false; // true when output is semantic Default (nullopt)
    int dd_webcam_count = -1;                         // total webcam devices in snapshot
    QString dd_selected_webcam_stable_id;             // stable id of configured webcam (empty = not applicable)
    bool dd_selected_webcam_available = true;         // true when configured webcam is present
    int dd_display_count = -1;                        // total displays in snapshot
    QString dd_selected_display_stable_id;            // stable id of selected display (empty = not applicable)
    bool dd_selected_display_available = true;        // true when selected display is present
    bool dd_current_target_resolved = true;           // false when the capture target is unresolved
    bool dd_rescan_enabled = true;                    // true when Rescan button is active/enabled
    QString dd_last_discovery_reason;                 // e.g. "DeviceRemoved", "DefaultChanged", "Startup"

    // --- Capture frame state (capture-frame-* scenarios) ---
    bool capture_frame_action_visible = false;
    bool capture_frame_action_enabled = false;
    bool capture_frame_pending = false; // a snapshot request is in flight
    bool capture_frame_success = false; // last capture succeeded
    QString capture_frame_last_saved;   // filename of last saved PNG

    // --- Hotkeys visual state (hotkeys-* scenarios) ---
    // hk_capture_action == -1 means "not in capture mode".
    bool hk_capture_active = false; // show one active row in capture mode
    int hk_capture_action = -1;     // 0=ToggleRecording, 1=TogglePause
    bool hk_conflict_shown = false; // show inline conflict error on a row
    int hk_conflict_action = -1;    // which row shows the conflict message
    QString hk_conflict_message;    // text of the conflict message
    // Custom bindings: indexed by HotkeyAction (0=ToggleRecording, 1=TogglePause).
    // Empty string = leave default.
    QString hk_custom_binding_0;    // portable key sequence string for ToggleRecording
    QString hk_custom_binding_1;    // portable key sequence string for TogglePause
    bool hk_editing_locked = false; // editing locked (recording-in-progress state)

    // --- Webcam chroma key (WEBCAM-EFFECTS-R1) ---
    bool webcam_chroma_enabled = false;
    QString webcam_chroma_color_mode; // "green" / "blue" / "magenta" / "custom" / "" (not set)

    // --- Recording marker scenarios (RECORDING-MARKERS-R1) ---
    bool marker_action_visible = false;
    bool marker_action_enabled = false;
    int marker_count = 0;
    uint64_t marker_latest_time_ms = 0;
    QString marker_latest_type; // "general" / "cut" / "highlight"
    QString marker_sidecar_file;
    QString marker_recording_state; // "Recording" / "Paused"
    bool hk_marker_active = false;  // hotkey action active on HotkeysPage

    // --- Split recording scenarios (SPLIT-RECORDING-R1) ---
    bool split_action_visible = false;
    bool split_action_enabled = false;
    int completed_segment_count = 1;        // number of segments in completed result
    bool completed_segment_missing = false; // at least one segment file is missing

    // --- Live pipeline diagnostics (DIAGNOSTICS-LIVE-PIPELINE-R1) ---
    // Selects a deterministic synthetic RecordingDiagnosticsSnapshot injected into the
    // Diagnostics page's live telemetry panel. Empty = static page only. Recognized:
    // "idle", "healthy", "encoder", "disk", "paused", "split".
    QString diag_live;

    // --- Keyboard focus (VISUAL-REVIEW-AND-PRODUCT-POLISH-R1 / VR-004) ---
    // objectName of a widget that receives keyboard focus (Qt::TabFocusReason)
    // after the scenario is applied, so :focus styling renders deterministically.
    QString focused_object;

    // --- Settings tiers (SETTINGS-TIERS-R1) ---
    // Drive the deterministic Settings progressive-disclosure states that the
    // live-app capture script cannot reach (it only renders defaults).
    bool settings_expert_mode = false;       // Expert mode ON → Developer card revealed
    bool settings_advanced_expanded = false; // Output card "Advanced" expander open

    // Scroll the active page's scroll area to a named section before capture, so
    // below-the-fold cards (e.g. the Updates card) are visible in the grab.
    // Same vocabulary as ConfigPage::scrollToSection ("settings/updates" etc.).
    QString scroll_target;

    // Drive the Settings Updates card state (ADR 0034): "uptodate" | "checking" |
    // "available" | "error". With "available", settings_update_version fills the
    // "Update to vX.Y" action. Empty = leave the card at its default.
    QString settings_update_state;
    QString settings_update_version;

    // --- EditExport scenarios (PHASE-G-EDIT-EXPORT-R1) ---
    QString edit_export_phase;
    QString edit_export_file_path;
    QString edit_export_duration;
    QString edit_export_size;
    QString edit_export_resolution;
    QString edit_export_fps;
    QString edit_export_video_codec;
    QString edit_export_audio_codec;
    QString edit_export_container;
};

const QVector<VisualScenario>& VisualScenarioRegistry();
const VisualScenario* FindVisualScenario(QStringView id);
QStringList VisualScenarioIds();

bool VisualHarnessEnabledForBuildConfig(QStringView build_config);
int VisualRunnerExitCode(bool scenario_found, bool manifest_written, bool screenshot_written, bool requested_manifest,
                         bool requested_screenshot);
bool ValidateVisualScenario(const VisualScenario& scenario, QString* error = nullptr);

QString ToString(VisualPage page);
QString ToString(VisualRecordState state);
QString ToString(VisualSettingsTarget target);
QString ToString(VisualSourcePickerTab tab);
QString ToString(VisualWebcamState state);
QString ToString(VisualRegionState state);
QString ToString(VisualRegionEditMode mode);
QString ToString(VisualWebcamHandle handle);
QString ToString(VisualLogFilter filter);

} // namespace exosnap::visual
