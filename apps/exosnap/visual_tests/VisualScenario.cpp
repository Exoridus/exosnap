#include "VisualScenario.h"

#include <recorder_core/webcam_placement.h>

#include <algorithm>
#include <cmath>

namespace exosnap::visual {
namespace {

const QVector<VisualScenario> kScenarios = {
    {QStringLiteral("record-ready"),
     QStringLiteral("Record / Ready"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
      {QStringLiteral("recordDockTimer"), QStringLiteral("timer may be masked by visual diff runners")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}}},
    {QStringLiteral("record-ready-countdown-off"),
     QStringLiteral("Record / Ready / Countdown Off"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     0},
    {QStringLiteral("record-ready-countdown-3s"),
     QStringLiteral("Record / Ready / Countdown 3s"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3},
    {QStringLiteral("record-countdown-3"),
     QStringLiteral("Record / Countdown / 3"),
     VisualPage::Record,
     VisualRecordState::Countdown,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview remains dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3,
     3},
    {QStringLiteral("record-countdown-2"),
     QStringLiteral("Record / Countdown / 2"),
     VisualPage::Record,
     VisualRecordState::Countdown,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview remains dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3,
     2},
    {QStringLiteral("record-countdown-1"),
     QStringLiteral("Record / Countdown / 1"),
     VisualPage::Record,
     VisualRecordState::Countdown,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview remains dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3,
     1},
    {QStringLiteral("record-countdown-cancelled"),
     QStringLiteral("Record / Countdown Cancelled"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3,
     0},
    {QStringLiteral("record-recording-after-countdown"),
     QStringLiteral("Record / Recording After Countdown"),
     VisualPage::Record,
     VisualRecordState::Recording,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("technical preview surface")},
      {QStringLiteral("recordDockTimer"), QStringLiteral("timer starts from zero after countdown")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     3,
     0},
    {QStringLiteral("record-recording"),
     QStringLiteral("Record / Recording"),
     VisualPage::Record,
     VisualRecordState::Recording,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("technical preview surface")},
      {QStringLiteral("recordDockTimer"), QStringLiteral("fixed visual-test timer")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}}},
    {QStringLiteral("record-paused"),
     QStringLiteral("Record / Paused"),
     VisualPage::Record,
     VisualRecordState::Paused,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("technical preview surface")},
      {QStringLiteral("recordDockTimer"), QStringLiteral("fixed visual-test timer")},
      {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}}},
    {QStringLiteral("record-completed"),
     QStringLiteral("Record / Completed"),
     VisualPage::Record,
     VisualRecordState::Completed,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::None,
     {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
      {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}}},
    {QStringLiteral("settings-display"), QStringLiteral("Settings / Display"), VisualPage::Settings,
     VisualRecordState::None, VisualSettingsTarget::Display},
    {QStringLiteral("settings-window"), QStringLiteral("Settings / Window"), VisualPage::Settings,
     VisualRecordState::None, VisualSettingsTarget::Window},
    {QStringLiteral("settings-region"), QStringLiteral("Settings / Region"), VisualPage::Settings,
     VisualRecordState::None, VisualSettingsTarget::Region},
    {QStringLiteral("source-picker-screens"),
     QStringLiteral("Source Picker / Screens"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Screens,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("thumbnail cards may be masked")}}},
    {QStringLiteral("source-picker-windows"),
     QStringLiteral("Source Picker / Windows"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Windows,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("thumbnail cards may be masked")}}},
    {QStringLiteral("source-picker-region"),
     QStringLiteral("Source Picker / Region"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}}},
    {QStringLiteral("source-region-empty"),
     QStringLiteral("Source Picker / Region Empty"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Empty,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0},
    {QStringLiteral("source-region-selected"),
     QStringLiteral("Source Picker / Region Selected"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Selected,
     VisualRegionEditMode::None,
     640,
     360,
     1280,
     720},
    {QStringLiteral("source-region-editing"),
     QStringLiteral("Source Picker / Region Editing"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Editing,
     VisualRegionEditMode::Move,
     640,
     360,
     1280,
     720},
    {QStringLiteral("source-region-preset-16x9"),
     QStringLiteral("Source Picker / Region Preset 16x9"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Preset16x9,
     VisualRegionEditMode::None,
     320,
     180,
     1920,
     1080},
    {QStringLiteral("source-region-preset-9x16"),
     QStringLiteral("Source Picker / Region Preset 9x16"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Preset9x16,
     VisualRegionEditMode::None,
     875,
     0,
     810,
     1440},
    {QStringLiteral("source-region-invalid"),
     QStringLiteral("Source Picker / Region Invalid"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Region,
     VisualWebcamState::None,
     {{QStringLiteral("sourcePickerDialog"), QStringLiteral("region preview controls may be masked")}},
     0,
     0,
     VisualRegionState::Invalid,
     VisualRegionEditMode::None,
     100,
     100,
     32,
     32},
    {QStringLiteral("webcam-active"),
     QStringLiteral("Webcam / Active"),
     VisualPage::Webcam,
     VisualRecordState::None,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::None,
     VisualWebcamState::Active,
     {{QStringLiteral("webcamCameraPreview"), QStringLiteral("synthetic test camera frame")}}},
    {QStringLiteral("webcam-unavailable"), QStringLiteral("Webcam / Unavailable"), VisualPage::Webcam,
     VisualRecordState::None, VisualSettingsTarget::None, VisualSourcePickerTab::None, VisualWebcamState::Unavailable},

    // --- Webcam PiP placement + mirror (WEBCAM-PIP-MIRROR-R1) -------------------
    // Record-page PiP scenarios run with a deterministic synthetic preview + camera
    // frame (DXGI stopped), so the Qt paint path is exercised reproducibly.
    {.id = QStringLiteral("record-webcam-disabled"),
     .title = QStringLiteral("Record / Webcam Disabled"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::None,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = false},
    {.id = QStringLiteral("record-webcam-default-pip"),
     .title = QStringLiteral("Record / Webcam Default PiP"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_x = 0.75f,
     .webcam_y = 0.75f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("record-webcam-selected"),
     .title = QStringLiteral("Record / Webcam Selected"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_x = 0.75f,
     .webcam_y = 0.75f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("record-webcam-dragging"),
     .title = QStringLiteral("Record / Webcam Dragging"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_handle = VisualWebcamHandle::Move,
     .webcam_x = 0.40f,
     .webcam_y = 0.40f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("record-webcam-resize-top-left"),
     .title = QStringLiteral("Record / Webcam Resize Top-Left"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_handle = VisualWebcamHandle::ResizeTopLeft,
     .webcam_x = 0.55f,
     .webcam_y = 0.55f,
     .webcam_w = 0.30f,
     .webcam_h = 0.30f},
    {.id = QStringLiteral("record-webcam-resize-bottom-right"),
     .title = QStringLiteral("Record / Webcam Resize Bottom-Right"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_handle = VisualWebcamHandle::ResizeBottomRight,
     .webcam_x = 0.40f,
     .webcam_y = 0.40f,
     .webcam_w = 0.40f,
     .webcam_h = 0.40f},
    {.id = QStringLiteral("record-webcam-min-size"),
     .title = QStringLiteral("Record / Webcam Minimum Size"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_x = 0.90f,
     .webcam_y = 0.90f,
     .webcam_w = 0.05f,
     .webcam_h = 0.05f},
    {.id = QStringLiteral("record-webcam-max-size"),
     .title = QStringLiteral("Record / Webcam Maximum Size"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_pip_selected = true,
     .webcam_x = 0.05f,
     .webcam_y = 0.05f,
     .webcam_w = 0.90f,
     .webcam_h = 0.90f},
    {.id = QStringLiteral("record-webcam-mirrored"),
     .title = QStringLiteral("Record / Webcam Mirrored"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_mirror = true,
     .webcam_x = 0.75f,
     .webcam_y = 0.75f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("record-webcam-countdown-locked"),
     .title = QStringLiteral("Record / Webcam Countdown Locked"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Countdown,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .countdown_seconds = 3,
     .countdown_remaining = 3,
     .webcam_pip_enabled = true,
     .webcam_pip_edit_locked = true,
     .webcam_x = 0.75f,
     .webcam_y = 0.75f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("record-webcam-recording-locked"),
     .title = QStringLiteral("Record / Webcam Recording Locked"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("technical preview surface")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed visual-test timer")}},
     .webcam_pip_enabled = true,
     .webcam_pip_edit_locked = true,
     .webcam_x = 0.75f,
     .webcam_y = 0.75f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},
    {.id = QStringLiteral("settings-webcam-mirror-off"),
     .title = QStringLiteral("Settings / Webcam Mirror Off"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("webcamCameraPreview"), QStringLiteral("synthetic test camera frame")}},
     .webcam_mirror = false},
    {.id = QStringLiteral("settings-webcam-mirror-on"),
     .title = QStringLiteral("Settings / Webcam Mirror On"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("webcamCameraPreview"), QStringLiteral("synthetic test camera frame")}},
     .webcam_mirror = true},
    {.id = QStringLiteral("settings-webcam-unavailable"),
     .title = QStringLiteral("Settings / Webcam Unavailable"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Unavailable},
    {.id = QStringLiteral("settings-webcam-chroma-disabled"),
     .title = QStringLiteral("Webcam / Chroma Disabled"),
     .page = VisualPage::Webcam,
     .webcam_state = VisualWebcamState::Active,
     .webcam_chroma_enabled = false,
     .webcam_chroma_color_mode = QStringLiteral("green")},
    {.id = QStringLiteral("settings-webcam-chroma-green"),
     .title = QStringLiteral("Webcam / Chroma Green"),
     .page = VisualPage::Webcam,
     .webcam_state = VisualWebcamState::Active,
     .webcam_chroma_enabled = true,
     .webcam_chroma_color_mode = QStringLiteral("green")},
    {.id = QStringLiteral("settings-webcam-chroma-blue"),
     .title = QStringLiteral("Webcam / Chroma Blue"),
     .page = VisualPage::Webcam,
     .webcam_state = VisualWebcamState::Active,
     .webcam_chroma_enabled = true,
     .webcam_chroma_color_mode = QStringLiteral("blue")},
    {.id = QStringLiteral("settings-webcam-chroma-custom"),
     .title = QStringLiteral("Webcam / Chroma Custom"),
     .page = VisualPage::Webcam,
     .webcam_state = VisualWebcamState::Active,
     .webcam_chroma_enabled = true,
     .webcam_chroma_color_mode = QStringLiteral("custom")},

    // --- Live chroma key during recording/paused (WEBCAM-EFFECTS-R1) -----------
    {.id = QStringLiteral("record-webcam-chroma-active"),
     .title = QStringLiteral("Webcam / Chroma Active (Recording)"),
     .page = VisualPage::Webcam,
     .record_state = VisualRecordState::Recording,
     .webcam_state = VisualWebcamState::Active,
     .controls_locked = true,
     .webcam_chroma_enabled = true,
     .webcam_chroma_color_mode = QStringLiteral("green")},
    {.id = QStringLiteral("paused-webcam-chroma-active"),
     .title = QStringLiteral("Webcam / Chroma Active (Paused)"),
     .page = VisualPage::Webcam,
     .record_state = VisualRecordState::Paused,
     .webcam_state = VisualWebcamState::Active,
     .controls_locked = true,
     .webcam_chroma_enabled = true,
     .webcam_chroma_color_mode = QStringLiteral("green")},

    {QStringLiteral("diagnostics"), QStringLiteral("Diagnostics"), VisualPage::Diagnostics},
    {QStringLiteral("hotkeys"), QStringLiteral("Hotkeys"), VisualPage::Hotkeys},
    {.id = QStringLiteral("hotkeys-default"),
     .title = QStringLiteral("Hotkeys / Default Bindings"),
     .page = VisualPage::Hotkeys},
    {.id = QStringLiteral("hotkeys-capture"),
     .title = QStringLiteral("Hotkeys / Capture Mode"),
     .page = VisualPage::Hotkeys,
     .hk_capture_active = true,
     .hk_capture_action = 1},
    {.id = QStringLiteral("hotkeys-conflict"),
     .title = QStringLiteral("Hotkeys / Conflict Message"),
     .page = VisualPage::Hotkeys,
     .hk_conflict_shown = true,
     .hk_conflict_action = 1,
     .hk_conflict_message = QStringLiteral("Alt+F9 is already assigned to Start / Stop recording.")},
    {.id = QStringLiteral("hotkeys-custom-bindings"),
     .title = QStringLiteral("Hotkeys / Custom Bindings"),
     .page = VisualPage::Hotkeys,
     .hk_custom_binding_0 = QStringLiteral("Ctrl+Shift+R"),
     .hk_custom_binding_1 = QStringLiteral("Alt+F10")},
    {QStringLiteral("logs"), QStringLiteral("Logs"), VisualPage::Logs},
    {.id = QStringLiteral("logs-empty"),
     .title = QStringLiteral("Logs / Empty"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::All,
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-all-levels"),
     .title = QStringLiteral("Logs / All Levels"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::All,
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-info-filter"),
     .title = QStringLiteral("Logs / Info Filter"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::Info,
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-issues-filter"),
     .title = QStringLiteral("Logs / Issues Filter"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::Issues,
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-search-results"),
     .title = QStringLiteral("Logs / Search Results"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::All,
     .log_search_query = QStringLiteral("webcam"),
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-long-message"),
     .title = QStringLiteral("Logs / Long Message"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::All,
     .log_auto_scroll = true},
    {.id = QStringLiteral("logs-buffer-truncated"),
     .title = QStringLiteral("Logs / Buffer Truncated"),
     .page = VisualPage::Logs,
     .log_filter = VisualLogFilter::All,
     .log_auto_scroll = false},
    {QStringLiteral("about"), QStringLiteral("About"), VisualPage::About},

    // ---- Preset card scenarios (COMPLETE-PRESET-R1) -------------------------
    // All Settings / preset scenarios use synthetic ConfigPage::ProfileOption
    // data injected by applyVisualSettingsScenario — no RecordingPresetStore or
    // registry is touched.  preset_count > 0 triggers the injection path.

    // settings-preset-default: selected == default, clean state (badge visible,
    // Save hidden).
    {.id = QStringLiteral("settings-preset-default"),
     .title = QStringLiteral("Settings / Preset Default"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Default"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false},

    // settings-preset-modified: dirty state — presetDirtyIndicator visible,
    // Save enabled.
    {.id = QStringLiteral("settings-preset-modified"),
     .title = QStringLiteral("Settings / Preset Modified"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Gaming"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = true},

    // settings-preset-saved: clean state after a save on a non-default preset.
    {.id = QStringLiteral("settings-preset-saved"),
     .title = QStringLiteral("Settings / Preset Saved"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Tutorial"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false},

    // settings-preset-menu: Manage overflow menu shown with all actions.
    {.id = QStringLiteral("settings-preset-menu"),
     .title = QStringLiteral("Settings / Preset Menu Open"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Gaming"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false,
     .preset_menu_open = true},

    // settings-preset-multiple: 4+ presets in the combo.
    {.id = QStringLiteral("settings-preset-multiple"),
     .title = QStringLiteral("Settings / Preset Multiple"),
     .page = VisualPage::Settings,
     .preset_count = 5,
     .preset_selected_name = QStringLiteral("Streaming"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false},

    // settings-preset-default-badge: selected == default so presetDefaultBadge is
    // visible; "Set as default" action would be disabled.
    {.id = QStringLiteral("settings-preset-default-badge"),
     .title = QStringLiteral("Settings / Preset Default Badge"),
     .page = VisualPage::Settings,
     .preset_count = 4,
     .preset_selected_name = QStringLiteral("Default"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false},

    // settings-preset-delete-confirm: represents the delete-confirmation intent
    // deterministically by showing the Manage menu (no real blocking QMessageBox).
    // The harness opens the menu and the Delete action is the highlighted entry.
    // Screenshots capture the full menu open state before any confirmation dialog
    // would appear.  This is the same approach used for settings-preset-menu.
    {.id = QStringLiteral("settings-preset-delete-confirm"),
     .title = QStringLiteral("Settings / Preset Delete (menu open, no modal)"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Gaming"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = false,
     .preset_menu_open = true},

    // settings-preset-save-error: preset_save_error=true — inline name-conflict
    // error affordance, deterministic, no modal.
    {.id = QStringLiteral("settings-preset-save-error"),
     .title = QStringLiteral("Settings / Preset Save Error"),
     .page = VisualPage::Settings,
     .preset_count = 3,
     .preset_selected_name = QStringLiteral("Gaming"),
     .preset_default_name = QStringLiteral("Default"),
     .preset_dirty = true,
     .preset_save_error = true},

    // ---- Record page preset scenarios ----------------------------------------

    // record-preset-display: Display capture kind, representative preset.
    {.id = QStringLiteral("record-preset-display"),
     .title = QStringLiteral("Record / Preset / Display"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .settings_target = VisualSettingsTarget::Display,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}}},

    // record-preset-window: Window capture kind.
    {.id = QStringLiteral("record-preset-window"),
     .title = QStringLiteral("Record / Preset / Window"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .settings_target = VisualSettingsTarget::Window,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}}},

    // record-preset-region: Region capture with a representative region rect.
    {.id = QStringLiteral("record-preset-region"),
     .title = QStringLiteral("Record / Preset / Region"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .settings_target = VisualSettingsTarget::Region,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     .region_state = VisualRegionState::Selected,
     .region_x = 640,
     .region_y = 360,
     .region_width = 1280,
     .region_height = 720},

    // record-preset-webcam: webcam PiP enabled with representative placement.
    {.id = QStringLiteral("record-preset-webcam"),
     .title = QStringLiteral("Record / Preset / Webcam PiP"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .webcam_pip_enabled = true,
     .webcam_mirror = false,
     .webcam_x = 0.72f,
     .webcam_y = 0.72f,
     .webcam_w = 0.25f,
     .webcam_h = 0.25f},

    // record-preset-countdown: countdown_seconds=3, Ready state.
    {.id = QStringLiteral("record-preset-countdown"),
     .title = QStringLiteral("Record / Preset / Countdown 3s"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     .countdown_seconds = 3},

    {.id = QStringLiteral("settings-output-native"),
     .title = QStringLiteral("Settings / Output / Native"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::Native,
     .effective_width = 2560,
     .effective_height = 1440},
    {.id = QStringLiteral("settings-output-4k"),
     .title = QStringLiteral("Settings / Output / 4K"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::UHD2160,
     .requested_width = 3840,
     .requested_height = 2160,
     .effective_width = 3840,
     .effective_height = 2160,
     .content_width = 3840,
     .content_height = 2160},
    {.id = QStringLiteral("settings-output-1440p"),
     .title = QStringLiteral("Settings / Output / 1440p"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::QHD1440,
     .requested_width = 2560,
     .requested_height = 1440,
     .effective_width = 2560,
     .effective_height = 1440},
    {.id = QStringLiteral("settings-output-1080p"),
     .title = QStringLiteral("Settings / Output / 1080p"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080},
    {.id = QStringLiteral("settings-output-720p"),
     .title = QStringLiteral("Settings / Output / 720p"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::HD720,
     .requested_width = 1280,
     .requested_height = 720,
     .effective_width = 1280,
     .effective_height = 720,
     .content_width = 1280,
     .content_height = 720},

    // Custom output resolution scenarios (CUSTOM-OUTPUT-RESOLUTION-R1).
    {.id = QStringLiteral("settings-output-custom-resolution"),
     .title = QStringLiteral("Settings / Output / Custom Resolution"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::Custom,
     .requested_width = 2560,
     .requested_height = 1440,
     .effective_width = 2560,
     .effective_height = 1440,
     .content_width = 2560,
     .content_height = 1440},
    {.id = QStringLiteral("settings-output-custom-resolution-invalid"),
     .title = QStringLiteral("Settings / Output / Custom Resolution Invalid"),
     .page = VisualPage::Settings,
     .output_resolution_mode = OutputResolutionMode::Custom,
     .requested_width = 100,
     .requested_height = 100,
     .effective_width = 0,
     .effective_height = 0,
     .content_width = 2560,
     .content_height = 1440},
    {.id = QStringLiteral("completed-output-custom-resolution"),
     .title = QStringLiteral("Completed / Output / Custom Resolution"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .output_resolution_mode = OutputResolutionMode::Custom,
     .requested_width = 2560,
     .requested_height = 1440,
     .effective_width = 2560,
     .effective_height = 1440,
     .content_width = 2560,
     .content_height = 1440},

    {.id = QStringLiteral("settings-format-24-cfr"),
     .title = QStringLiteral("Settings / Format / 24 CFR"),
     .page = VisualPage::Settings,
     .frame_rate_num = 24,
     .frame_rate_den = 1,
     .cfr = true},
    {.id = QStringLiteral("settings-format-30-cfr"),
     .title = QStringLiteral("Settings / Format / 30 CFR"),
     .page = VisualPage::Settings,
     .frame_rate_num = 30,
     .frame_rate_den = 1,
     .cfr = true},
    {.id = QStringLiteral("settings-format-60-cfr"),
     .title = QStringLiteral("Settings / Format / 60 CFR"),
     .page = VisualPage::Settings,
     .frame_rate_num = 60,
     .frame_rate_den = 1,
     .cfr = true},
    {.id = QStringLiteral("settings-format-120-unavailable"),
     .title = QStringLiteral("Settings / Format / 120 Unavailable"),
     .page = VisualPage::Settings,
     .frame_rate_num = 60,
     .frame_rate_den = 1,
     .cfr = true,
     .reconciliation_warning = QStringLiteral("requested 120 fps unavailable; effective 60 fps")},
    {.id = QStringLiteral("settings-format-vfr"),
     .title = QStringLiteral("Settings / Format / VFR"),
     .page = VisualPage::Settings,
     .frame_rate_num = 60,
     .frame_rate_den = 1,
     .cfr = false,
     .container = capability::Container::Matroska},
    {.id = QStringLiteral("settings-format-container-mkv"),
     .title = QStringLiteral("Settings / Format / MKV"),
     .page = VisualPage::Settings,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::Av1Nvenc,
     .audio_codec = capability::AudioCodec::Opus},
    {.id = QStringLiteral("settings-format-container-mp4"),
     .title = QStringLiteral("Settings / Format / MP4"),
     .page = VisualPage::Settings,
     .container = capability::Container::Mp4,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf},
    {.id = QStringLiteral("settings-format-incompatible"),
     .title = QStringLiteral("Settings / Format / Incompatible Reconciled"),
     .page = VisualPage::Settings,
     .container = capability::Container::Mp4,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .reconciliation_warning = QStringLiteral("MP4 reconciles to H.264 + AAC + CFR")},
    {.id = QStringLiteral("settings-format-recording-locked"),
     .title = QStringLiteral("Settings / Format / Recording Locked"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::Recording,
     .controls_locked = true},

    {.id = QStringLiteral("record-output-native"),
     .title = QStringLiteral("Record / Output / Native"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .output_resolution_mode = OutputResolutionMode::Native,
     .effective_width = 2560,
     .effective_height = 1440},
    {.id = QStringLiteral("record-output-1080p"),
     .title = QStringLiteral("Record / Output / 1080p"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080},
    {.id = QStringLiteral("record-output-letterbox"),
     .title = QStringLiteral("Record / Output / Letterbox"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("letterbox fixture is synthetic")}},
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .source_width = 1024,
     .source_height = 768,
     .content_x = 240,
     .content_y = 0,
     .content_width = 1440,
     .content_height = 1080},
    {.id = QStringLiteral("record-output-region"),
     .title = QStringLiteral("Record / Output / Region"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .settings_target = VisualSettingsTarget::Region,
     .region_state = VisualRegionState::Selected,
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .source_width = 1280,
     .source_height = 720,
     .content_width = 1920,
     .content_height = 1080},
    {.id = QStringLiteral("record-output-webcam"),
     .title = QStringLiteral("Record / Output / Webcam"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .webcam_state = VisualWebcamState::Active,
     .webcam_pip_enabled = true,
     .webcam_x = 0.70f,
     .webcam_y = 0.68f,
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080},
    {.id = QStringLiteral("record-output-summary"),
     .title = QStringLiteral("Record / Output / Summary"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080},

    {.id = QStringLiteral("completed-output-1080p"),
     .title = QStringLiteral("Completed / Output / 1080p"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .requested_width = 1920,
     .requested_height = 1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080},
    {.id = QStringLiteral("completed-output-fallback"),
     .title = QStringLiteral("Completed / Output / Fallback"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .output_resolution_mode = OutputResolutionMode::UHD2160,
     .requested_width = 3840,
     .requested_height = 2160,
     .effective_width = 1920,
     .effective_height = 1080,
     .content_width = 1920,
     .content_height = 1080,
     .reconciliation_warning = QStringLiteral("Requested 4K exceeded capability; effective output is 1080p")},

    // --- Reactive Source Discovery scenarios (SOURCE-DISCOVERY-R1) ---
    {QStringLiteral("source-picker-windows-initial"),
     QStringLiteral("Source Picker / Windows / Initial Load"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Windows,
     VisualWebcamState::None,
     {},
     0,
     0,
     VisualRegionState::None,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0,
     false,
     false,
     false,
     false,
     VisualWebcamHandle::None,
     0,
     0,
     0,
     0,
     VisualLogFilter::All,
     QString(),
     true,
     0,
     QString(),
     QString(),
     false,
     false,
     false,
     OutputResolutionMode::Native,
     2560,
     1440,
     2560,
     1440,
     2560,
     1440,
     0,
     0,
     2560,
     1440,
     60,
     1,
     true,
     capability::Container::WebM,
     capability::VideoCodec::Av1Nvenc,
     capability::AudioCodec::Opus,
     QString(),
     false,
     -1,
     -1,
     QString(),
     true,
     true,
     0},

    {QStringLiteral("source-picker-windows-added"),
     QStringLiteral("Source Picker / Windows / New Window Added"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Windows,
     VisualWebcamState::None,
     {},
     0,
     0,
     VisualRegionState::None,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0,
     false,
     false,
     false,
     false,
     VisualWebcamHandle::None,
     0,
     0,
     0,
     0,
     VisualLogFilter::All,
     QString(),
     true,
     0,
     QString(),
     QString(),
     false,
     false,
     false,
     OutputResolutionMode::Native,
     2560,
     1440,
     2560,
     1440,
     2560,
     1440,
     0,
     0,
     2560,
     1440,
     60,
     1,
     true,
     capability::Container::WebM,
     capability::VideoCodec::Av1Nvenc,
     capability::AudioCodec::Opus,
     QString(),
     false,
     -1,
     -1,
     QString(),
     true,
     true,
     0},

    {QStringLiteral("source-picker-window-disappeared"),
     QStringLiteral("Source Picker / Windows / Window Disappeared"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Windows,
     VisualWebcamState::None,
     {},
     0,
     0,
     VisualRegionState::None,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0,
     false,
     false,
     false,
     false,
     VisualWebcamHandle::None,
     0,
     0,
     0,
     0,
     VisualLogFilter::All,
     QString(),
     true,
     0,
     QString(),
     QString(),
     false,
     false,
     false,
     OutputResolutionMode::Native,
     2560,
     1440,
     2560,
     1440,
     2560,
     1440,
     0,
     0,
     2560,
     1440,
     60,
     1,
     true,
     capability::Container::WebM,
     capability::VideoCodec::Av1Nvenc,
     capability::AudioCodec::Opus,
     QString(),
     false,
     -1,
     -1,
     QString(),
     false,
     true,
     0},

    {QStringLiteral("source-picker-selection-preserved"),
     QStringLiteral("Source Picker / Selection Preserved"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Windows,
     VisualWebcamState::None,
     {},
     0,
     0,
     VisualRegionState::None,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0,
     false,
     false,
     false,
     false,
     VisualWebcamHandle::None,
     0,
     0,
     0,
     0,
     VisualLogFilter::All,
     QString(),
     true,
     0,
     QString(),
     QString(),
     false,
     false,
     false,
     OutputResolutionMode::Native,
     2560,
     1440,
     2560,
     1440,
     2560,
     1440,
     0,
     0,
     2560,
     1440,
     60,
     1,
     true,
     capability::Container::WebM,
     capability::VideoCodec::Av1Nvenc,
     capability::AudioCodec::Opus,
     QString(),
     false,
     -1,
     -1,
     QString(),
     true,
     true,
     0},

    {QStringLiteral("source-picker-displays-updated"),
     QStringLiteral("Source Picker / Displays Updated"),
     VisualPage::Record,
     VisualRecordState::Ready,
     VisualSettingsTarget::None,
     VisualSourcePickerTab::Screens,
     VisualWebcamState::None,
     {},
     0,
     0,
     VisualRegionState::None,
     VisualRegionEditMode::None,
     0,
     0,
     0,
     0,
     false,
     false,
     false,
     false,
     VisualWebcamHandle::None,
     0,
     0,
     0,
     0,
     VisualLogFilter::All,
     QString(),
     true,
     0,
     QString(),
     QString(),
     false,
     false,
     false,
     OutputResolutionMode::Native,
     2560,
     1440,
     2560,
     1440,
     2560,
     1440,
     0,
     0,
     2560,
     1440,
     60,
     1,
     true,
     capability::Container::WebM,
     capability::VideoCodec::Av1Nvenc,
     capability::AudioCodec::Opus,
     QString(),
     false,
     -1,
     -1,
     QString(),
     true,
     true,
     0},
};

// --- Completed result scenarios (OUTPUT-RESULTS-R1) ---
// These are appended via a second initializer list since they reference
// fields that were added after the original positional entries.
// (scenarios are added here for build ordering convenience)
} // namespace

namespace {
const QVector<VisualScenario> kCompletedScenarios = {
    {.id = QStringLiteral("record-completed-details"),
     .title = QStringLiteral("Record / Completed / Details"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .output_resolution_mode = OutputResolutionMode::FHD1080,
     .effective_width = 1920,
     .effective_height = 1080,
     .source_width = 1920,
     .source_height = 1080,
     .content_x = 0,
     .content_y = 0,
     .content_width = 1920,
     .content_height = 1080,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .result_file_name = QStringLiteral("completed-detail.mkv"),
     .result_file_size_bytes = 128ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 120.0},

    {.id = QStringLiteral("record-completed-long-filename"),
     .title = QStringLiteral("Record / Completed / Long Filename"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::WebM,
     .video_codec = capability::VideoCodec::Av1Nvenc,
     .audio_codec = capability::AudioCodec::Opus,
     .result_file_name = QStringLiteral("2024-12-25_very-long-recording-name-with-many-details-and-metadata.webm"),
     .result_file_size_bytes = 256ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 300.0},

    {.id = QStringLiteral("record-completed-missing-file"),
     .title = QStringLiteral("Record / Completed / Missing File"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .result_file_name = QStringLiteral("deleted-recording.mkv"),
     .result_file_size_bytes = 64ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 45.0,
     .result_file_exists = false},

    {.id = QStringLiteral("record-completed-recent-history"),
     .title = QStringLiteral("Record / Completed / Recent History"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .result_file_name = QStringLiteral("latest-recording.mkv"),
     .result_file_size_bytes = 32ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 60.0,
     .recent_result_count = 5},

    {.id = QStringLiteral("record-completed-delete-confirm"),
     .title = QStringLiteral("Record / Completed / Delete Confirm"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .result_file_name = QStringLiteral("to-delete.mkv"),
     .result_file_size_bytes = 10ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 12.0,
     .show_delete_confirm = true},

    {.id = QStringLiteral("completed-history-restored"),
     .title = QStringLiteral("Record / Completed / History Restored"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::Av1Nvenc,
     .audio_codec = capability::AudioCodec::Opus,
     .result_file_name = QStringLiteral("restored-latest.mkv"),
     .result_file_size_bytes = 64ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 90.0,
     .recent_result_count = 3},

    {.id = QStringLiteral("completed-history-restored-missing"),
     .title = QStringLiteral("Record / Completed / History Restored (Missing)"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("completed preview may be masked")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("fixed completed duration")}},
     .effective_width = 2560,
     .effective_height = 1440,
     .source_width = 2560,
     .source_height = 1440,
     .content_x = 0,
     .content_y = 0,
     .content_width = 2560,
     .content_height = 1440,
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::H264Nvenc,
     .audio_codec = capability::AudioCodec::AacMf,
     .result_file_name = QStringLiteral("restored-missing.mkv"),
     .result_file_size_bytes = 0,
     .result_duration_seconds = 0.0,
     .result_file_exists = false,
     .recent_result_count = 1},
};
} // namespace

// --- Device Discovery scenarios (DEVICE-DISCOVERY-R1) ---
// Each scenario represents a specific device state encountered during reactive
// device discovery.  Fields are deterministic and non-persistent: they do NOT
// write to AppSettingsStore or RecordingPresetStore.
const QVector<VisualScenario> kDeviceDiscoveryScenarios = {
    // 1. Settings/Audio — normal state: mic list present, a mic selected & available.
    {.id = QStringLiteral("settings-audio-devices-normal"),
     .title = QStringLiteral("Settings / Audio / Devices Normal"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .settings_target = VisualSettingsTarget::None,
     .dd_audio_input_count = 2,
     .dd_audio_output_count = 1,
     .dd_selected_mic_stable_id = QStringLiteral("dev-mic-001"),
     .dd_selected_mic_available = true,
     .dd_selected_output_semantic_default = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("Startup")},

    // 2. Settings/Audio — selected mic unavailable (placeholder shown), id preserved.
    {.id = QStringLiteral("settings-audio-selected-missing"),
     .title = QStringLiteral("Settings / Audio / Selected Mic Missing"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .settings_target = VisualSettingsTarget::None,
     .dd_audio_input_count = 1,
     .dd_audio_output_count = 1,
     .dd_selected_mic_stable_id = QStringLiteral("dev-mic-001"),
     .dd_selected_mic_available = false,
     .dd_selected_output_semantic_default = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceRemoved")},

    // 3. Settings/Audio — semantic Default mic, last_discovery_reason=DefaultChanged.
    {.id = QStringLiteral("settings-audio-default-changed"),
     .title = QStringLiteral("Settings / Audio / Default Changed"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .settings_target = VisualSettingsTarget::None,
     .dd_audio_input_count = 2,
     .dd_audio_output_count = 1,
     .dd_selected_mic_stable_id = QString(),
     .dd_selected_mic_available = true,
     .dd_selected_output_semantic_default = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DefaultChanged")},

    // 4. Settings/Webcam — camera present & available.
    {.id = QStringLiteral("settings-webcam-devices-normal"),
     .title = QStringLiteral("Settings / Webcam / Devices Normal"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("webcamCameraPreview"), QStringLiteral("synthetic test camera frame")}},
     .dd_webcam_count = 1,
     .dd_selected_webcam_stable_id = QStringLiteral("cam-vis-001"),
     .dd_selected_webcam_available = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("Startup")},

    // 5. Settings/Webcam — selected camera unavailable (honest no-stale-frame state).
    {.id = QStringLiteral("settings-webcam-selected-missing"),
     .title = QStringLiteral("Settings / Webcam / Selected Camera Missing"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Unavailable,
     .dd_webcam_count = 0,
     .dd_selected_webcam_stable_id = QStringLiteral("cam-vis-001"),
     .dd_selected_webcam_available = false,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceRemoved")},

    // 6. Settings/Webcam — camera available again after a loss (reconnect path).
    {.id = QStringLiteral("settings-webcam-reconnected"),
     .title = QStringLiteral("Settings / Webcam / Camera Reconnected"),
     .page = VisualPage::Settings,
     .record_state = VisualRecordState::None,
     .webcam_state = VisualWebcamState::Active,
     .masks = {{QStringLiteral("webcamCameraPreview"), QStringLiteral("synthetic test camera frame")}},
     .dd_webcam_count = 1,
     .dd_selected_webcam_stable_id = QStringLiteral("cam-vis-001"),
     .dd_selected_webcam_available = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceAdded")},

    // 7. Source Picker open, displays listed, one selected & available.
    {.id = QStringLiteral("source-displays-normal"),
     .title = QStringLiteral("Source Picker / Displays Normal"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .source_picker_tab = VisualSourcePickerTab::Screens,
     .masks = {{QStringLiteral("sourcePickerDialog"), QStringLiteral("thumbnail cards may be masked")}},
     .dd_display_count = 2,
     .dd_selected_display_stable_id = QStringLiteral("\\\\.\\DISPLAY1"),
     .dd_selected_display_available = true,
     .dd_current_target_resolved = true,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("Startup")},

    // 8. Source Picker open, selected display unavailable (honest unresolved).
    {.id = QStringLiteral("source-display-selected-missing"),
     .title = QStringLiteral("Source Picker / Display Selected Missing"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .source_picker_tab = VisualSourcePickerTab::Screens,
     .masks = {{QStringLiteral("sourcePickerDialog"), QStringLiteral("thumbnail cards may be masked")}},
     .dd_display_count = 1,
     .dd_selected_display_stable_id = QStringLiteral("\\\\.\\DISPLAY2"),
     .dd_selected_display_available = false,
     .dd_current_target_resolved = false,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceRemoved")},

    // 9. Record page, configured display unresolved (no stale preview, no silent switch).
    {.id = QStringLiteral("record-display-unavailable"),
     .title = QStringLiteral("Record / Display Unavailable"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     .dd_display_count = 1,
     .dd_selected_display_stable_id = QStringLiteral("\\\\.\\DISPLAY2"),
     .dd_selected_display_available = false,
     .dd_current_target_resolved = false,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceRemoved")},

    // 10. Record page, region invalidated because hosting monitor gone.
    {.id = QStringLiteral("record-region-monitor-missing"),
     .title = QStringLiteral("Record / Region / Monitor Missing"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .settings_target = VisualSettingsTarget::Region,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordTransportDock"), QStringLiteral("meter values are deterministic test levels")}},
     .region_state = VisualRegionState::Invalid,
     .region_x = 100,
     .region_y = 100,
     .region_width = 64,
     .region_height = 64,
     .dd_display_count = 1,
     .dd_selected_display_stable_id = QStringLiteral("\\\\.\\DISPLAY2"),
     .dd_selected_display_available = false,
     .dd_current_target_resolved = false,
     .dd_rescan_enabled = true,
     .dd_last_discovery_reason = QStringLiteral("DeviceRemoved")},
};

const QVector<VisualScenario> kCaptureFrameScenarios = {
    // Ready state — action visible and enabled, no pending request.
    {.id = QStringLiteral("record-capture-frame-ready"),
     .title = QStringLiteral("Record / Capture Frame / Ready"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .capture_frame_action_visible = true,
     .capture_frame_action_enabled = true,
     .capture_frame_pending = false},

    // Recording state — action visible, one request in flight.
    {.id = QStringLiteral("record-capture-frame-recording"),
     .title = QStringLiteral("Record / Capture Frame / Recording"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .capture_frame_action_visible = true,
     .capture_frame_action_enabled = true,
     .capture_frame_pending = true},

    // Saved feedback — action visible, last save succeeded.
    {.id = QStringLiteral("record-capture-frame-saved"),
     .title = QStringLiteral("Record / Capture Frame / Saved"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .capture_frame_action_visible = true,
     .capture_frame_action_enabled = true,
     .capture_frame_success = true,
     .capture_frame_last_saved = QStringLiteral("2026-06-09_12-00-00_frame.png")},

    // Unavailable — no preview frame in ready state.
    {.id = QStringLiteral("record-capture-frame-unavailable"),
     .title = QStringLiteral("Record / Capture Frame / Unavailable"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Ready,
     .capture_frame_action_visible = true,
     .capture_frame_action_enabled = false,
     .capture_frame_success = false,
     .capture_frame_last_saved = {}},
};

const QVector<VisualScenario> kMarkerScenarios = {
    {.id = QStringLiteral("record-recording-with-markers"),
     .title = QStringLiteral("Record / Recording / With Markers"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .marker_action_visible = true,
     .marker_action_enabled = true,
     .marker_count = 3,
     .marker_latest_time_ms = 370200,
     .marker_latest_type = QStringLiteral("cut"),
     .marker_recording_state = QStringLiteral("Recording"),
     .hk_marker_active = true},

    {.id = QStringLiteral("record-paused-with-marker"),
     .title = QStringLiteral("Record / Paused / With Marker"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Paused,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .marker_action_visible = true,
     .marker_action_enabled = true,
     .marker_count = 1,
     .marker_latest_time_ms = 207450,
     .marker_latest_type = QStringLiteral("general"),
     .marker_recording_state = QStringLiteral("Paused"),
     .hk_marker_active = true},

    {.id = QStringLiteral("record-completed-markers"),
     .title = QStringLiteral("Record / Completed / Markers"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .result_file_name = QStringLiteral("recording.webm"),
     .result_file_size_bytes = 52ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 83.0,
     .result_file_exists = true,
     .marker_count = 3,
     .marker_latest_time_ms = 370200,
     .marker_latest_type = QStringLiteral("cut"),
     .marker_sidecar_file = QStringLiteral("recording.markers.json")},
};

const QVector<VisualScenario> kSplitRecordingScenarios = {
    // --- Recording state: split available (SPLIT-RECORDING-R1) ---
    {.id = QStringLiteral("record-split-available"),
     .title = QStringLiteral("Record / Split / Available During Recording"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Recording,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")},
               {QStringLiteral("recordDockTimer"), QStringLiteral("timer is dynamic during recording")}},
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::Av1Nvenc,
     .split_action_visible = true,
     .split_action_enabled = true},

    {.id = QStringLiteral("paused-split-available"),
     .title = QStringLiteral("Record / Split / Available While Paused"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Paused,
     .masks = {{QStringLiteral("previewSurface"), QStringLiteral("live preview is dynamic")}},
     .container = capability::Container::Matroska,
     .video_codec = capability::VideoCodec::Av1Nvenc,
     .split_action_visible = true,
     .split_action_enabled = true},

    // --- Completed result: multi-segment (SPLIT-RECORDING-R1) ---
    {.id = QStringLiteral("completed-recording-segments"),
     .title = QStringLiteral("Record / Completed / Multi-Segment"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .container = capability::Container::Matroska,
     .result_file_name = QStringLiteral("recording_part_003.mkv"),
     .result_file_size_bytes = 156ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 605.0,
     .result_file_exists = true,
     .completed_segment_count = 3},

    {.id = QStringLiteral("completed-recording-segment-missing"),
     .title = QStringLiteral("Record / Completed / Segment Missing"),
     .page = VisualPage::Record,
     .record_state = VisualRecordState::Completed,
     .container = capability::Container::Matroska,
     .result_file_name = QStringLiteral("recording_part_002.mkv"),
     .result_file_size_bytes = 52ULL * 1024ULL * 1024ULL,
     .result_duration_seconds = 205.0,
     .result_file_exists = true,
     .completed_segment_count = 3,
     .completed_segment_missing = true},
};

const QVector<VisualScenario>& VisualScenarioRegistry() {
    static QVector<VisualScenario> merged;
    if (merged.isEmpty()) {
        merged = kScenarios;
        merged.append(kCompletedScenarios);
        merged.append(kDeviceDiscoveryScenarios);
        merged.append(kCaptureFrameScenarios);
        merged.append(kMarkerScenarios);
        merged.append(kSplitRecordingScenarios);
    }
    return merged;
}

const VisualScenario* FindVisualScenario(QStringView id) {
    const auto& scenarios = VisualScenarioRegistry();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(),
                                 [id](const VisualScenario& scenario) { return scenario.id == id; });
    return it == scenarios.end() ? nullptr : &(*it);
}

QStringList VisualScenarioIds() {
    QStringList ids;
    ids.reserve(VisualScenarioRegistry().size());
    for (const VisualScenario& scenario : VisualScenarioRegistry())
        ids.push_back(scenario.id);
    return ids;
}

bool VisualHarnessEnabledForBuildConfig(QStringView build_config) {
    return build_config.compare(QStringLiteral("Release"), Qt::CaseInsensitive) != 0;
}

int VisualRunnerExitCode(bool scenario_found, bool manifest_written, bool screenshot_written, bool requested_manifest,
                         bool requested_screenshot) {
    if (!scenario_found)
        return 2;
    if (requested_manifest && !manifest_written)
        return 3;
    if (requested_screenshot && !screenshot_written)
        return 4;
    return 0;
}

bool ValidateVisualScenario(const VisualScenario& scenario, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    if (scenario.countdown_seconds != 0 && scenario.countdown_seconds != 3 && scenario.countdown_seconds != 5 &&
        scenario.countdown_seconds != 10) {
        return fail(QStringLiteral("Invalid visual-test countdown duration"));
    }
    if (scenario.countdown_remaining < 0 || scenario.countdown_remaining > scenario.countdown_seconds) {
        return fail(QStringLiteral("Invalid visual-test countdown remaining value"));
    }
    if (scenario.record_state == VisualRecordState::Countdown && scenario.countdown_remaining <= 0) {
        return fail(QStringLiteral("Countdown visual state requires a positive remaining value"));
    }
    const auto valid_size_or_zero = [](int width, int height) {
        return (width == 0 && height == 0) || (width > 0 && height > 0);
    };
    if (!valid_size_or_zero(scenario.requested_width, scenario.requested_height) ||
        !valid_size_or_zero(scenario.effective_width, scenario.effective_height) ||
        !valid_size_or_zero(scenario.source_width, scenario.source_height) ||
        !valid_size_or_zero(scenario.content_width, scenario.content_height)) {
        return fail(QStringLiteral("Invalid visual-test output dimensions"));
    }
    if (scenario.frame_rate_num == 0 || scenario.frame_rate_den == 0) {
        return fail(QStringLiteral("Invalid visual-test frame rate"));
    }
    if (scenario.content_width > 0 && scenario.content_height > 0 && scenario.effective_width > 0 &&
        scenario.effective_height > 0) {
        if (scenario.content_x < 0 || scenario.content_y < 0 ||
            scenario.content_x + scenario.content_width > scenario.effective_width ||
            scenario.content_y + scenario.content_height > scenario.effective_height) {
            return fail(QStringLiteral("Visual-test content rectangle must fit inside output"));
        }
    }

    const bool region_visual = scenario.region_state != VisualRegionState::None &&
                               scenario.region_state != VisualRegionState::Empty &&
                               scenario.region_state != VisualRegionState::Invalid;
    if (region_visual && (scenario.region_width < 64 || scenario.region_height < 64)) {
        return fail(QStringLiteral("Invalid visual-test region geometry"));
    }
    if (scenario.region_state == VisualRegionState::Empty &&
        (scenario.region_width != 0 || scenario.region_height != 0)) {
        return fail(QStringLiteral("Empty visual-test region must not carry geometry"));
    }

    // Webcam PiP placement must be a valid, bounds-safe normalized rectangle.
    if (scenario.webcam_pip_enabled) {
        constexpr float kMin = recorder_core::WebcamPlacement::kMinSize;
        constexpr float kMax = recorder_core::WebcamPlacement::kMaxSize;
        const float x = scenario.webcam_x;
        const float y = scenario.webcam_y;
        const float w = scenario.webcam_w;
        const float h = scenario.webcam_h;
        const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h);
        if (!finite)
            return fail(QStringLiteral("Webcam PiP placement must be finite"));
        if (w < kMin || h < kMin || w > kMax || h > kMax)
            return fail(QStringLiteral("Webcam PiP size out of bounds"));
        if (x < 0.0f || y < 0.0f || x + w > 1.0f + 1e-4f || y + h > 1.0f + 1e-4f)
            return fail(QStringLiteral("Webcam PiP rectangle escapes the content frame"));
    }

    return true;
}

QString ToString(VisualPage page) {
    switch (page) {
    case VisualPage::Record:
        return QStringLiteral("record");
    case VisualPage::Settings:
        return QStringLiteral("settings");
    case VisualPage::Webcam:
        return QStringLiteral("webcam");
    case VisualPage::Hotkeys:
        return QStringLiteral("hotkeys");
    case VisualPage::Diagnostics:
        return QStringLiteral("diagnostics");
    case VisualPage::Logs:
        return QStringLiteral("logs");
    case VisualPage::About:
        return QStringLiteral("about");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualRecordState state) {
    switch (state) {
    case VisualRecordState::None:
        return QStringLiteral("none");
    case VisualRecordState::Ready:
        return QStringLiteral("ready");
    case VisualRecordState::Countdown:
        return QStringLiteral("countdown");
    case VisualRecordState::Recording:
        return QStringLiteral("recording");
    case VisualRecordState::Paused:
        return QStringLiteral("paused");
    case VisualRecordState::Completed:
        return QStringLiteral("completed");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualSettingsTarget target) {
    switch (target) {
    case VisualSettingsTarget::None:
        return QStringLiteral("none");
    case VisualSettingsTarget::Display:
        return QStringLiteral("display");
    case VisualSettingsTarget::Window:
        return QStringLiteral("window");
    case VisualSettingsTarget::Region:
        return QStringLiteral("region");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualSourcePickerTab tab) {
    switch (tab) {
    case VisualSourcePickerTab::None:
        return QStringLiteral("none");
    case VisualSourcePickerTab::Screens:
        return QStringLiteral("screens");
    case VisualSourcePickerTab::Windows:
        return QStringLiteral("windows");
    case VisualSourcePickerTab::Region:
        return QStringLiteral("region");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualWebcamState state) {
    switch (state) {
    case VisualWebcamState::None:
        return QStringLiteral("none");
    case VisualWebcamState::Active:
        return QStringLiteral("active");
    case VisualWebcamState::Unavailable:
        return QStringLiteral("unavailable");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualRegionState state) {
    switch (state) {
    case VisualRegionState::None:
        return QStringLiteral("none");
    case VisualRegionState::Empty:
        return QStringLiteral("empty");
    case VisualRegionState::Selected:
        return QStringLiteral("selected");
    case VisualRegionState::Editing:
        return QStringLiteral("editing");
    case VisualRegionState::Preset16x9:
        return QStringLiteral("preset-16x9");
    case VisualRegionState::Preset9x16:
        return QStringLiteral("preset-9x16");
    case VisualRegionState::Invalid:
        return QStringLiteral("invalid");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualRegionEditMode mode) {
    switch (mode) {
    case VisualRegionEditMode::None:
        return QStringLiteral("none");
    case VisualRegionEditMode::Move:
        return QStringLiteral("move");
    case VisualRegionEditMode::ResizeTopLeft:
        return QStringLiteral("resize-top-left");
    case VisualRegionEditMode::ResizeTopRight:
        return QStringLiteral("resize-top-right");
    case VisualRegionEditMode::ResizeBottomLeft:
        return QStringLiteral("resize-bottom-left");
    case VisualRegionEditMode::ResizeBottomRight:
        return QStringLiteral("resize-bottom-right");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualWebcamHandle handle) {
    switch (handle) {
    case VisualWebcamHandle::None:
        return QStringLiteral("none");
    case VisualWebcamHandle::Move:
        return QStringLiteral("move");
    case VisualWebcamHandle::ResizeTopLeft:
        return QStringLiteral("tl");
    case VisualWebcamHandle::ResizeTopRight:
        return QStringLiteral("tr");
    case VisualWebcamHandle::ResizeBottomLeft:
        return QStringLiteral("bl");
    case VisualWebcamHandle::ResizeBottomRight:
        return QStringLiteral("br");
    }
    return QStringLiteral("unknown");
}

QString ToString(VisualLogFilter filter) {
    switch (filter) {
    case VisualLogFilter::All:
        return QStringLiteral("all");
    case VisualLogFilter::Info:
        return QStringLiteral("info");
    case VisualLogFilter::Issues:
        return QStringLiteral("issues");
    }
    return QStringLiteral("unknown");
}

} // namespace exosnap::visual
