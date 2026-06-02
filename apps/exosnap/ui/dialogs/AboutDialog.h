#pragma once
#include <QDialog>

namespace exosnap::ui::dialogs {

class AboutDialog : public QDialog {
    Q_OBJECT
  public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

} // namespace exosnap::ui::dialogs
