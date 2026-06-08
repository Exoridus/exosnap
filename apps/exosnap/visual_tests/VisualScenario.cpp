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

} // namespace exosnap::visual
