#pragma once
#include <QAbstractButton>
#include <QColor>
#include <QString>

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Circular glass-scrim button for preview corners.
// Matches the CornerBtn mockup: dark semi-transparent BG, 1px border,
// centered Lucide icon. Hover/pressed/disabled states via paintEvent.
// signal clicked() is inherited from QAbstractButton.
class CornerCaptureButton : public QAbstractButton {
    Q_OBJECT
  public:
    explicit CornerCaptureButton(QWidget* parent = nullptr);

    void setIcon(const QString& lucide_name);
    void setIconColor(const QColor& color);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString icon_name_{"camera"};
    QColor icon_color_;
};

} // namespace exosnap::ui::widgets
