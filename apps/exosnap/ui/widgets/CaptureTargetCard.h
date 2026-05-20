#pragma once

#include <QFrame>

class QLabel;

namespace exosnap::ui::widgets {

class CaptureTargetCard : public QFrame {
    Q_OBJECT
  public:
    explicit CaptureTargetCard(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    QString title() const;

    void setSubtitle(const QString& subtitle);
    QString subtitle() const;

    void setStatusText(const QString& status);
    QString statusText() const;

    void setSelected(bool selected);
    bool isSelected() const noexcept;

  signals:
    void clicked();

  protected:
    void mousePressEvent(QMouseEvent* event) override;

  private:
    QLabel* title_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    bool selected_ = false;
};

} // namespace exosnap::ui::widgets
