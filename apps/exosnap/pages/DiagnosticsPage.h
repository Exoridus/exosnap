#pragma once
#include <QWidget>

class QLabel;

namespace exosnap {

class DiagnosticsPage : public QWidget {
    Q_OBJECT
  public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

  private:
    void onRunCheck();

    QLabel* status_label_ = nullptr;
    QLabel* last_check_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
};

} // namespace exosnap
