#pragma once

#include "../models/VideoSettingsModel.h"

#include <QWidget>
#include <vector>

class QButtonGroup;
class QLabel;

namespace exosnap::ui::widgets {
class CodecCard;
class ExoCheckBox;
} // namespace exosnap::ui::widgets

namespace exosnap {

class VideoPage : public QWidget {
    Q_OBJECT
  public:
    explicit VideoPage(const VideoSettingsModel& initial_settings, QWidget* parent = nullptr);

  signals:
    void videoSettingsChanged(VideoSettingsModel settings);

  private:
    void selectCodecCard(ui::widgets::CodecCard* selected_card);
    void onQualityChanged(int id);
    void onCfrVfrChanged(int id);

    QButtonGroup* quality_group_ = nullptr;
    QButtonGroup* resolution_group_ = nullptr;
    QButtonGroup* cfr_vfr_group_ = nullptr;
    ui::widgets::ExoCheckBox* cursor_check_ = nullptr;
    QWidget* rail_widget_ = nullptr;
    std::vector<ui::widgets::CodecCard*> codec_cards_;

    QLabel* fps_note_label_ = nullptr;
    QLabel* rail_cq_label_ = nullptr;
    QLabel* rail_bitrate_label_ = nullptr;
    QLabel* rail_size_label_ = nullptr;
};

} // namespace exosnap
