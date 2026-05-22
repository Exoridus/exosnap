#pragma once
#include <QWidget>

class QComboBox;

namespace exosnap::ui::widgets {
class ExoCheckBox;
}

namespace exosnap {

class AdvancedPage : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPage(QWidget* parent = nullptr);

  private:
    void onReset();

    QComboBox* log_level_combo_ = nullptr;
    ui::widgets::ExoCheckBox* nvtx_check_ = nullptr;
};

} // namespace exosnap
