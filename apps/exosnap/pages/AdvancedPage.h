#pragma once
#include <QWidget>

class QCheckBox;
class QComboBox;

namespace exosnap {

class AdvancedPage : public QWidget {
    Q_OBJECT
  public:
    explicit AdvancedPage(QWidget* parent = nullptr);

  private:
    void onReset();

    QComboBox* log_level_combo_ = nullptr;
    QCheckBox* nvtx_check_ = nullptr;
};

} // namespace exosnap
