#pragma once

#include <QWidget>
#include <vector>

class QButtonGroup;
class QCheckBox;

namespace exosnap::ui::widgets {
class CodecCard;
}

namespace exosnap {

class VideoPage : public QWidget {
    Q_OBJECT
  public:
    explicit VideoPage(QWidget* parent = nullptr);

  private:
    void selectCodecCard(ui::widgets::CodecCard* selected_card);

    QButtonGroup* quality_group_ = nullptr;
    QButtonGroup* resolution_group_ = nullptr;
    QCheckBox* cursor_check_ = nullptr;
    QWidget* rail_widget_ = nullptr;
    std::vector<ui::widgets::CodecCard*> codec_cards_;
};

} // namespace exosnap
