#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

namespace exosnap::visual {

enum class VisualPage {
    Record,
    Settings,
    Webcam,
    Hotkeys,
    Diagnostics,
    Logs,
    About,
};

enum class VisualRecordState {
    None,
    Ready,
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
};

const QVector<VisualScenario>& VisualScenarioRegistry();
const VisualScenario* FindVisualScenario(QStringView id);
QStringList VisualScenarioIds();

bool VisualHarnessEnabledForBuildConfig(QStringView build_config);
int VisualRunnerExitCode(bool scenario_found, bool manifest_written, bool screenshot_written, bool requested_manifest,
                         bool requested_screenshot);

QString ToString(VisualPage page);
QString ToString(VisualRecordState state);
QString ToString(VisualSettingsTarget target);
QString ToString(VisualSourcePickerTab tab);
QString ToString(VisualWebcamState state);

} // namespace exosnap::visual
