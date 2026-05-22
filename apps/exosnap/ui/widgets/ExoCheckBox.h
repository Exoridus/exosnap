#pragma once

#include <QAbstractButton>

namespace exosnap::ui::widgets {

class ExoCheckBox : public QAbstractButton {
    Q_OBJECT
  public:
    explicit ExoCheckBox(const QString& text = QString(), QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool hovered_ = false;
};

} // namespace exosnap::ui::widgets
