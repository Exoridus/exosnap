#pragma once

#include <QVector>
#include <QWidget>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"

class QEvent;
class QFrame;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window Recovery surface shown at startup when interrupted recordings are
// found. Follows the AboutOverlay pattern exactly:
//   - Plain QWidget (never a QDialog / OS window).
//   - Parent = central widget; covers the full app window.
//   - Scrim via paintEvent rgba(8,8,10,0.62).
//   - Escape / backdrop-click = "dismiss" (decide later).
//   - Geometry tracked via parent event-filter.
//
// For each candidate the card shows:
//   filename, recording size, timestamp, target container
//   → "Keep as MKV" | "Export as MP4" | "Discard" (inline two-step confirm)
//   → progress bar + Cancel while a remux is running
//
// When all entries are resolved the overlay auto-closes.
class RecoveryOverlay : public QWidget {
    Q_OBJECT
  public:
    // `service` must outlive this widget.
    explicit RecoveryOverlay(RecoveryService& service, const QVector<RecoveryCandidate>& candidates,
                             QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

  signals:
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    QFrame* buildCard();

    RecoveryService& service_;
    QVector<RecoveryCandidate> candidates_;
    QFrame* card_ = nullptr;
};

} // namespace exosnap::ui::dialogs
