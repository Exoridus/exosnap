#pragma once

#include <QFrame>
#include <QString>

class QLabel;
class QKeyEvent;
class QMouseEvent;

namespace exosnap::ui::widgets {

// Selectable adapter card for the Device page's encoder-device grid (one card
// per DXGI adapter). Mirrors the suite-device.jsx DeviceSelector row: avatar +
// vendor/name + iGPU/dGPU kind badge + backend line, with a selected state
// (mint-tinted, matches RegionPresetCard's selection language) and an
// independent "Active encoder" badge for the adapter that actually backs the
// current recording session.
class DeviceAdapterCard : public QFrame {
    Q_OBJECT
  public:
    explicit DeviceAdapterCard(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    const QString& title() const;

    // e.g. "dGPU" / "iGPU"
    void setKindBadge(const QString& text);

    // e.g. "NVENC" or "" (roadmap vendors show no backend line).
    void setBackendLine(const QString& text);

    void setSelected(bool selected);
    bool isSelected() const noexcept;

    // The adapter currently backing the recording session's encoder (at most
    // one adapter should carry this at a time). Independent of setSelected —
    // selection only controls which capability matrix is shown below.
    void setActive(bool active);
    bool isActive() const noexcept;

  signals:
    void clicked();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    QLabel* avatar_icon_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* kind_badge_ = nullptr;
    QLabel* backend_label_ = nullptr;
    QLabel* active_badge_ = nullptr;

    QString title_text_;
    bool selected_ = false;
    bool active_ = false;
    bool click_armed_ = false;

    void updateAvatarTint();
};

} // namespace exosnap::ui::widgets
