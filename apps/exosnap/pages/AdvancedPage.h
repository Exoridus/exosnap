#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"

class QComboBox;
class QLabel;

namespace exosnap::ui::widgets {
class ExoCheckBox;
}

namespace exosnap {

class AdvancedPage : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPage(QWidget* parent = nullptr);

    // Reflects the active profile and resolved capture settings in the read-only
    // baseline panel. Called by MainWindow whenever the active profile changes.
    void setBaseline(const OutputSettingsModel& output, const VideoSettingsModel& video, const QString& profile_name);

  signals:
    void backToSettingsRequested();

  private:
    void onReset();

    QComboBox* log_level_combo_ = nullptr;
    ui::widgets::ExoCheckBox* nvtx_check_ = nullptr;

    QLabel* baseline_profile_label_ = nullptr;
    QLabel* baseline_container_label_ = nullptr;
    QLabel* baseline_video_label_ = nullptr;
    QLabel* baseline_quality_label_ = nullptr;
    QLabel* baseline_framerate_label_ = nullptr;
    QLabel* baseline_audio_label_ = nullptr;
    QLabel* baseline_cursor_label_ = nullptr;
};

} // namespace exosnap
