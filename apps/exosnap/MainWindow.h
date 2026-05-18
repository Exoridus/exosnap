#pragma once
#include <QListWidget>
#include <QMainWindow>
#include <QStackedWidget>

namespace exosnap {

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QWidget* parent = nullptr);

  private slots:
    void onNavChanged(QListWidgetItem* current, QListWidgetItem* previous);

  private:
    QListWidget* nav_;
    QStackedWidget* stack_;
};

} // namespace exosnap
