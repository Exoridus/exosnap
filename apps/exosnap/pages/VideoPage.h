#pragma once

#include <QWidget>
#include <vector>

class QButtonGroup;

namespace exosnap::ui::widgets {
class CodecCard;
class ExoCheckBox;
} // namespace exosnap::ui::widgets

namespace exosnap {

class VideoPage : public QWidget {
    Q_OBJECT
  public:
    explicit VideoPage(QWidget* parent = nullptr);

  private:
    void selectCodecCard(ui::widgets::CodecCard* selected_card);

    QButtonGroup* quality_group_ = nullptr;
    QButtonGroup* resolution_group_ = nullptr;
    ui::widgets::ExoCheckBox* cursor_check_ = nullptr;
    QWidget* rail_widget_ = nullptr;
    std::vector<ui::widgets::CodecCard*> codec_cards_;
};

} // namespace exosnap
