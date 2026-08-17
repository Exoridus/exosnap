#include "WhatsNewAdapter.h"

#include "diagnostics/AppLog.h"

#include <QDesktopServices>
#include <QUrl>
#include <QVariantMap>

namespace exosnap::quick {
namespace {

// The product's own releases page, used when a check has not reported one. Same
// address models::AboutInfo carries; duplicated rather than reached for because
// nothing else in this class knows about About.
constexpr const char* kReleasesUrlFallback = "https://github.com/Exoridus/exosnap/releases";

} // namespace

WhatsNewAdapter::WhatsNewAdapter(QObject* parent) : QObject(parent) {
}

bool WhatsNewAdapter::active() const noexcept {
    return active_;
}

bool WhatsNewAdapter::postUpdateMode() const noexcept {
    return post_update_mode_;
}

const QVariantList& WhatsNewAdapter::notes() const noexcept {
    return notes_;
}

bool WhatsNewAdapter::showAfterUpdates() const noexcept {
    return show_after_updates_;
}

void WhatsNewAdapter::setShowAfterUpdates(bool show) {
    if (show_after_updates_ == show)
        return;
    show_after_updates_ = show;
    emit showAfterUpdatesChanged();
    // Unchecking suppresses future auto-shows; it never hides the card link.
    emit suppressedEdited(!show);
}

void WhatsNewAdapter::setSuppressed(bool suppressed) {
    const bool show = !suppressed;
    if (show_after_updates_ == show)
        return;
    show_after_updates_ = show;
    emit showAfterUpdatesChanged();
}

void WhatsNewAdapter::present(const QVector<WhatsNewNote>& notes, bool post_update_mode, const QString& releases_url) {
    if (notes.isEmpty()) {
        diagnostics::AppLog::info(QStringLiteral("update"),
                                  QStringLiteral("\"What's new\" was requested with no release notes to show; "
                                                 "no overlay was raised."));
        return;
    }

    notes_.clear();
    notes_.reserve(notes.size());
    for (const WhatsNewNote& note : notes) {
        QVariantMap row;
        row[QStringLiteral("version")] = note.version;
        row[QStringLiteral("body")] = note.body;
        row[QStringLiteral("url")] = note.html_url;
        notes_.append(row);
    }
    releases_url_ = releases_url.isEmpty() ? QString::fromLatin1(kReleasesUrlFallback) : releases_url;
    post_update_mode_ = post_update_mode;
    active_ = true;
    emit changed();
}

void WhatsNewAdapter::dismiss() {
    if (!active_)
        return;
    active_ = false;
    emit changed();
}

void WhatsNewAdapter::openAllReleases() {
    openUrl(releases_url_);
}

void WhatsNewAdapter::openUrl(const QString& url) {
    const QString target = url.isEmpty() ? QString::fromLatin1(kReleasesUrlFallback) : url;
    if (!QDesktopServices::openUrl(QUrl(target))) {
        diagnostics::AppLog::warning(QStringLiteral("update"),
                                     QStringLiteral("Could not open %1 in a browser.").arg(target));
    }
}

} // namespace exosnap::quick
