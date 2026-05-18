#pragma once
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QPushButton;

namespace exosnap {

class VideoPage : public QWidget {
    Q_OBJECT
  public:
    explicit VideoPage(QWidget* parent = nullptr);

  private slots:
    void onExpandAdvanced();

  private:
    QButtonGroup* codec_group_ = nullptr;
    QButtonGroup* quality_group_ = nullptr;
    QButtonGroup* resolution_group_ = nullptr;
    QCheckBox* cursor_check_ = nullptr;
    QWidget* advanced_panel_ = nullptr;
    QPushButton* expand_btn_ = nullptr;
    bool advanced_expanded_ = false;
};

} // namespace exosnap
