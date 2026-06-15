#pragma once

#include "../models/VideoSettingsModel.h"

#include <QWidget>
#include <vector>

class QButtonGroup;
class QLabel;
class QSpinBox;
class QStackedWidget;

namespace exosnap::ui::widgets {
class CodecCard;
class ExoCheckBox;
} // namespace exosnap::ui::widgets

namespace exosnap {

class VideoPage : public QWidget {
    Q_OBJECT
  public:
    explicit VideoPage(const VideoSettingsModel& initial_settings, QWidget* parent = nullptr);
    void setVideoSettings(const VideoSettingsModel& settings);

  signals:
    void videoSettingsChanged(VideoSettingsModel settings);

  private:
    void selectCodecCard(ui::widgets::CodecCard* selected_card);
    void onQualityChanged(int id);
    void onCfrVfrChanged(int id);
    void onCursorToggled(bool checked);
    void onRateControlChanged(int id);
    void onBitrateChanged(int value);

    // Collect current state from all controls and emit videoSettingsChanged.
    VideoSettingsModel collectSettings() const;

    QButtonGroup* quality_group_ = nullptr;
    QButtonGroup* resolution_group_ = nullptr;
    QButtonGroup* cfr_vfr_group_ = nullptr;
    QButtonGroup* rate_control_group_ = nullptr;
    QSpinBox* bitrate_spinbox_ = nullptr;
    QStackedWidget* quality_stack_ = nullptr; // page 0 = quality radios, page 1 = bitrate input
    ui::widgets::ExoCheckBox* cursor_check_ = nullptr;
    QWidget* rail_widget_ = nullptr;
    std::vector<ui::widgets::CodecCard*> codec_cards_;

    QLabel* fps_note_label_ = nullptr;
    QLabel* rail_rc_label_ = nullptr; // "RATE CTRL" value label in rail
    QLabel* rail_cq_label_ = nullptr;
    QLabel* rail_bitrate_label_ = nullptr;
    QLabel* rail_size_label_ = nullptr;
    QLabel* rail_cursor_label_ = nullptr;
};

} // namespace exosnap
