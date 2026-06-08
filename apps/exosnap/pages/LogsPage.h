#pragma once
#include <QWidget>

class QPlainTextEdit;
class QPushButton;
class QLabel;

namespace exosnap {

class LogsPage : public QWidget {
    Q_OBJECT
  public:
    explicit LogsPage(QWidget* parent = nullptr);

  private:
    void onRefresh();
    void onOpenFolder();
    void onCopy();
    void reloadLogContent();

    QPlainTextEdit* log_viewer_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QPushButton* copy_btn_ = nullptr;
    QLabel* status_label_ = nullptr;
};

} // namespace exosnap
