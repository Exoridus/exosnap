#pragma once

#include "services/WhatsNewPayload.h" // WhatsNewNote

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// Narrow QML boundary for the "What's new" release-notes overlay.
//
// One surface, two entry points, and the ONLY thing that differs between them is
// which notes it shows (product-spec, "What's new (shipped)"):
//
//   - Pre-update: the Settings update card's "See what's new in vX.Y" link, with
//     the full reference list for the active channel. No suppress checkbox, and
//     never gated by the suppress setting.
//   - Post-update: the first launch of a freshly-updated build, with the gap
//     notes the update persisted. Carries the "Show release notes after updates"
//     checkbox, checked by default.
//
// Deliberately holds no policy of its own. WHETHER the post-update overlay may
// appear is ShouldShowWhatsNew() over the persisted payload; WHEN it may appear
// is the composition root's, which waits for the blocking surfaces to clear; and
// the suppress setting is persisted by the composition root off
// suppressedEdited(). This class carries the notes and the two user inputs.
class WhatsNewAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("WhatsNewAdapter is provided by the application")

    Q_PROPERTY(bool active READ active NOTIFY changed FINAL)
    Q_PROPERTY(bool postUpdateMode READ postUpdateMode NOTIFY changed FINAL)
    // One entry per release, newest first: { version, body, url }. `body` is the
    // GitHub release body verbatim, so the surface renders it as Markdown.
    Q_PROPERTY(QVariantList notes READ notes NOTIFY changed FINAL)
    // The checkbox, in the direction the user reads it: checked means notes are
    // shown after updates. The persisted key is its inverse.
    Q_PROPERTY(
        bool showAfterUpdates READ showAfterUpdates WRITE setShowAfterUpdates NOTIFY showAfterUpdatesChanged FINAL)

  public:
    explicit WhatsNewAdapter(QObject* parent = nullptr);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool postUpdateMode() const noexcept;
    [[nodiscard]] const QVariantList& notes() const noexcept;
    [[nodiscard]] bool showAfterUpdates() const noexcept;

    void setShowAfterUpdates(bool show);

    // Seeds the checkbox from the persisted setting without reporting an edit —
    // publishing a stored value is not the user changing it.
    void setSuppressed(bool suppressed);

    // Raises the surface. An empty `notes` list is refused: an overlay whose whole
    // content is missing is worse than no overlay, and both entry points can
    // legitimately have nothing to show (a channel with no releases yet, a payload
    // that carried none).
    void present(const QVector<WhatsNewNote>& notes, bool post_update_mode, const QString& releases_url);

    // Escape, the backdrop, and the primary Close/Got it action all mean the same
    // thing here: this surface reports, it does not ask.
    Q_INVOKABLE void dismiss();
    // The footer "All releases" link.
    Q_INVOKABLE void openAllReleases();
    // A link inside a rendered release body.
    Q_INVOKABLE void openUrl(const QString& url);

  signals:
    void changed();
    void showAfterUpdatesChanged();
    // The user changed the checkbox. Carries the value of the persisted key
    // (`whats_new_suppressed`), not the checkbox's own state.
    void suppressedEdited(bool suppressed);

  private:
    QVariantList notes_;
    QString releases_url_;
    bool active_ = false;
    bool post_update_mode_ = false;
    bool show_after_updates_ = true;
};

} // namespace exosnap::quick
