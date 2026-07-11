#pragma once

#include <QVector>
#include <QWidget>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"

class QEvent;
class QFrame;
class QKeyEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QPushButton;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window Recovery surface shown at startup when interrupted recordings are
// found. Follows the AboutOverlay pattern exactly:
//   - Plain QWidget (never a QDialog / OS window).
//   - Parent = central widget; covers the full app window.
//   - Scrim via paintEvent rgba(8,8,10,0.62).
//   - Escape / backdrop-click = "Decide later" (entries remain in manifest).
//   - Geometry tracked via parent event-filter.
//
// ADR-0015: per candidate the card shows:
//   filename, recording size, timestamp, target container
//   → "Finish" | "Continue" (non-finalized only) | "Delete" (inline two-step confirm)
//   → "Decide later" text button (bottom of card)
//   → progress bar + Cancel while a remux is running
//
// "Continue" signal: emitted when the user arms a candidate for continuation.
// The overlay closes; the coordinator enters ArmedFromRecovery state.
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
    // Emitted when the user chooses "Continue" for a candidate.
    // The coordinator (wired by MainWindow) calls ArmFromRecovery().
    void continueRequested(const RecoveryManifestEntry& entry);

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    QFrame* buildCard();
    // Re-applies every theme-coloured inline stylesheet (card surface, chrome bar,
    // title/hint text, separator rules, the "Decide later" button) and repaints the
    // custom backdrop from the active theme. Runs once at construction and again on
    // every ReapplyTheme(). Per-candidate rows restyle themselves independently.
    void applyTheme();

    RecoveryService& service_;
    QVector<RecoveryCandidate> candidates_;
    QFrame* card_ = nullptr;

    // Theme-coloured widgets restyled by applyTheme() (promoted from buildCard locals).
    QWidget* chrome_bar_ = nullptr;
    QLabel* brand_label_ = nullptr;
    QFrame* chrome_sep_ = nullptr;
    QLabel* chrome_sub_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* hint_label_ = nullptr;
    QPushButton* decide_later_btn_ = nullptr;
    QVector<QFrame*> rules_;
};

} // namespace exosnap::ui::dialogs
