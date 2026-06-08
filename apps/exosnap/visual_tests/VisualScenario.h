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

} // namespace exosnap::visual
