#pragma once

#include <QAbstractButton>
#include <QString>

class QPaintEvent;

namespace exosnap::ui::widgets {

// Circular icon toggle used by the Record transport dock for the audio/webcam
// source pills (System / Mic / Webcam / App).
//
// The widget paints itself (icon + circular chrome) so the on/off look is
// honest regardless of the disabled palette: `setOn()` drives the visual state
// and `setInteractive()` controls whether the user can click it. When a toggle
// is non-interactive it becomes a read-only status pill (e.g. while recording,
// or for the webcam source which is configured in Settings).
class AudioSourceToggle : public QAbstractButton {
    Q_OBJECT
  public:
    // icon_key: one of "system", "mic", "webcam", "app".
    // source_key: identifier reported back to the controller via clicked().
    AudioSourceToggle(const QString& icon_key, const QString& source_key, QWidget* parent = nullptr);

    void setOn(bool on);
    [[nodiscard]] bool isOn() const noexcept {
        return on_;
    }

    // Non-interactive toggles act as read-only status pills (no clicks, no hover).
    void setInteractive(bool interactive);
    [[nodiscard]] bool isInteractive() const noexcept {
        return interactive_;
    }

    [[nodiscard]] QString sourceKey() const {
        return source_key_;
    }

    // Compact mono meter strip painted below the icon circle.
    // level01: 0.0 = silence, 1.0 = peak. Only sources with real data should
    // ever receive a non-zero value; webcam has no audio meter.
    void setMeterLevel(float level01);
    [[nodiscard]] float meterLevel() const noexcept {
        return meter_level_;
    }
    void setMeterActive(bool active);
    [[nodiscard]] bool isMeterActive() const noexcept {
        return meter_active_;
    }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString icon_key_;
    QString source_key_;
    bool on_ = false;
    bool interactive_ = true;
    float meter_level_ = 0.0f;
    bool meter_active_ = false;
};

} // namespace exosnap::ui::widgets
