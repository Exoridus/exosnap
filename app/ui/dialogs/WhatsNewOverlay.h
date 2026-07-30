#pragma once
// WhatsNewOverlay.h -- in-window "What's new" surface listing release notes.
// Pre-update mode shows the full channel history (every non-draft release);
// post-update mode shows only the gap (installed, target].
//
// Follows the RecoveryOverlay pattern exactly:
//   - Plain QWidget (never a QDialog / OS window).
//   - Parent = central widget; covers the full app window.
//   - Scrim via paintEvent (theme bg at ~0.62 alpha).
//   - Escape / backdrop-click closes.
//   - Geometry tracked via a parent event-filter.
//
// Two entry points share this one overlay:
//   - Pre-update (post_update_mode = false): opened from the Settings update
//     card "See what's new in vX.Y" link. No suppress checkbox.
//   - Post-update (post_update_mode = true): shown once on the first launch of a
//     freshly-updated build. Shows the "Show release notes after updates" checkbox.
//
// All notes are concatenated into a single always-expanded QTextBrowser, newest
// first, each preceded by a plain version heading. Bodies are GitHub release
// Markdown, rendered via QTextDocument::setMarkdown.

#include <QVector>
#include <QWidget>

#include "services/WhatsNewPayload.h" // WhatsNewNote

class QEvent;
class QFrame;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QLabel;
class QTextBrowser;

namespace exosnap::ui::dialogs {

class WhatsNewOverlay : public QWidget {
    Q_OBJECT
  public:
    // `notes` are newest-first (as produced by CollectReleaseNotes). `releases_url`
    // is opened by the footer "All releases" link.
    explicit WhatsNewOverlay(const QVector<WhatsNewNote>& notes, bool post_update_mode, const QString& releases_url,
                             QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    [[nodiscard]] bool isOpen() const noexcept;

  signals:
    void closed();
    // Emitted when the "Show release notes after updates" checkbox toggles
    // (post-update mode only; checked by default). Carries the inverse of the
    // checkbox's own checked state. MainWindow persists whats_new_suppressed.
    void suppressToggled(bool suppressed);

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    QFrame* buildCard();

    QVector<WhatsNewNote> notes_;
    bool post_update_mode_ = false;
    QString releases_url_;
    QFrame* card_ = nullptr;
};

} // namespace exosnap::ui::dialogs
