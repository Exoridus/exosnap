#pragma once
#include <QWidget>

class QPlainTextEdit;
class QPushButton;
class QLabel;
class QBoxLayout;
class QResizeEvent;

namespace exosnap {

class LogsPage : public QWidget {
    Q_OBJECT
  public:
    explicit LogsPage(QWidget* parent = nullptr);

  private:
    void resizeEvent(QResizeEvent* event) override;
    void onRefresh();
    void onOpenFolder();
    void reloadLogContent();

    QPlainTextEdit* log_viewer_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QLabel* status_label_ = nullptr;
    QBoxLayout* action_row_layout_ = nullptr;
};

} // namespace exosnap
