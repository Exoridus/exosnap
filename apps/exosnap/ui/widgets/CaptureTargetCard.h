#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QObject;
class QEvent;
class QKeyEvent;

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
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    void updateStatusLabel();

    QLabel* title_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QString status_text_;
    bool selected_ = false;
    bool click_armed_ = false;
};

} // namespace exosnap::ui::widgets
