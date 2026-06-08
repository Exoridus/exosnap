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

    {QStringLiteral("diagnostics"), QStringLiteral("Diagnostics"), VisualPage::Diagnostics},
    {QStringLiteral("hotkeys"), QStringLiteral("Hotkeys"), VisualPage::Hotkeys},
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
};

} // namespace

const QVector<VisualScenario>& VisualScenarioRegistry() {
    return kScenarios;
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
