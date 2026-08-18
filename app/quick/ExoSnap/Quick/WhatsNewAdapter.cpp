#include "WhatsNewAdapter.h"

#include "diagnostics/AppLog.h"

#include <QDesktopServices>
#include <QRegularExpression>
#include <QStringView>
#include <QUrl>
#include <QVariantMap>

namespace exosnap::quick {
namespace {

// The product's own releases page, used when a check has not reported one. Same
// address models::AboutInfo carries; duplicated rather than reached for because
// nothing else in this class knows about About.
constexpr const char* kReleasesUrlFallback = "https://github.com/Exoridus/exosnap/releases";

// Release bodies are third-party text: GitHub renders whatever the author wrote,
// and authors put screenshots in release notes. Qt's Text loads the images a
// MarkdownText or RichText document references, over the network, the moment the
// document is set -- so simply showing the body would make opening this overlay
// fetch remote URLs, from a product whose stated rule is that every URL it opens
// goes through one place. It would also be the only network request in the app
// that no setting governs.
//
// The alt text is kept rather than the whole reference dropped: a note that reads
// "see below" above a removed screenshot is worse than one that says what the
// screenshot was. Links are untouched -- they are inert until clicked, and the
// click already goes through openUrl().
QString WithoutRemoteImages(const QString& markdown) {
    // Raw string literals, so the patterns read as the regexes they are. Written
    // with C escapes they need a doubled backslash for every bracket, and `\b`
    // then compiles cleanly as a BACKSPACE rather than as a word boundary --
    // silently matching nothing.
    //
    // `![alt](url)` and `![alt][ref]`, alt captured.
    static const QRegularExpression kImage(QStringLiteral(R"(!\[([^\]]*)\](?:\([^)]*\)|\[[^\]]*\]))"));
    // Markdown permits inline HTML, and <img> is the same request by another name.
    static const QRegularExpression kHtmlImage(QStringLiteral(R"(<img\b[^>]*>)"),
                                               QRegularExpression::CaseInsensitiveOption);

    // Rebuilt from the matches rather than replaced with a `\1` backreference: the
    // backreference form produced an EMPTY alt text, which is precisely the result
    // this function exists to avoid -- a note reading "see below" above a
    // screenshot that is no longer there.
    QString out;
    out.reserve(markdown.size());
    qsizetype cursor = 0;
    QRegularExpressionMatchIterator it = kImage.globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out.append(QStringView{markdown}.sliced(cursor, match.capturedStart() - cursor));
        out.append(match.captured(1));
        cursor = match.capturedEnd();
    }
    out.append(QStringView{markdown}.sliced(cursor));

    // No alt text to keep for the HTML form: there it is an attribute, not the
    // element's content.
    out.remove(kHtmlImage);
    return out;
}

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
        row[QStringLiteral("body")] = WithoutRemoteImages(note.body);
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
