#include "VisualScenario.h"

#include <algorithm>

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
    {QStringLiteral("diagnostics"), QStringLiteral("Diagnostics"), VisualPage::Diagnostics},
    {QStringLiteral("hotkeys"), QStringLiteral("Hotkeys"), VisualPage::Hotkeys},
    {QStringLiteral("logs"), QStringLiteral("Logs"), VisualPage::Logs},
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

} // namespace exosnap::visual
