#pragma once

#include <QWidget>

class QTimer;

namespace exosnap::ui::widgets {

class StatusPill : public QWidget {
    Q_OBJECT
  public:
    enum class Tone {
        Ready,
        Recording,
        Warn,
        Blocked,
        Neutral,
        Info, // DF-15: azure accent for pre-recording states (Countdown, Starting)
    };
    Q_ENUM(Tone)

    explicit StatusPill(QWidget* parent = nullptr);

    void setTone(Tone tone);
    Tone tone() const noexcept;

    void setText(const QString& text);
    const QString& text() const;

    void setDotVisible(bool visible);
    bool isDotVisible() const noexcept;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void updateBlinkState();
    void advanceBlinkFrame();

    Tone tone_ = Tone::Neutral;
    QString text_;
    bool dot_visible_ = true;
    QTimer* blink_timer_ = nullptr;
    bool blink_low_phase_ = false;
    qreal dot_opacity_ = 1.0;
};

} // namespace exosnap::ui::widgets
