// CrashIssueReport.cpp -- Stage-0 GitHub crash-issue builder implementation.

#include "services/CrashIssueReport.h"

#include <QUrl>
#include <QUrlQuery>

namespace exosnap::services {

namespace {

// Repo slug and issue-form template wiring (Part C: .github/ISSUE_TEMPLATE/crash.yml).
constexpr char kIssuesNewPath[] = "https://github.com/Exoridus/exosnap/issues/new";
constexpr char kTemplateFile[] = "crash.yml";
constexpr char kLabel[] = "crash";
// Issue-form field id of the auto-filled metadata textarea in crash.yml.
constexpr char kMetadataFieldId[] = "metadata";

// Total encoded-URL budget. GitHub tolerates long URLs; we stay well under to
// avoid truncation by browsers/proxies. If exceeded we fall back to the bare
// template URL and rely on the clipboard copy for the metadata.
constexpr int kUrlBudget = 6000;

QString DisplayOrDash(const QString& v) {
    return v.isEmpty() ? QStringLiteral("—") : v;
}

} // namespace

QString BuildCrashMetadataBlock(const CrashIssueData& data) {
    // Plain key/value lines — easy to read in the issue body and trivial to
    // scan when triaging. Allowlisted fields only.
    QString block;
    block += QStringLiteral("Version: ") + DisplayOrDash(data.app_version) + QLatin1Char('\n');
    block += QStringLiteral("OS: ") + DisplayOrDash(data.os) + QLatin1Char('\n');
    block += QStringLiteral("GPU: ") + DisplayOrDash(data.gpu) + QLatin1Char('\n');
    block += QStringLiteral("Encoder: ") + DisplayOrDash(data.encoder) + QLatin1Char('\n');
    block += QStringLiteral("Exception: ") + DisplayOrDash(data.exception) + QLatin1Char('\n');
    block += QStringLiteral("Correlation ID: ") + DisplayOrDash(data.correlation_id);
    return block;
}

QString BuildCrashIssueUrl(const CrashIssueData& data) {
    // Bare template URL — also the fallback when the prefilled URL is too long.
    QUrl base(QString::fromLatin1(kIssuesNewPath));
    QUrlQuery bare;
    bare.addQueryItem(QStringLiteral("template"), QString::fromLatin1(kTemplateFile));
    bare.addQueryItem(QStringLiteral("labels"), QString::fromLatin1(kLabel));
    base.setQuery(bare);
    const QString bare_url = base.toString(QUrl::FullyEncoded);

    // Prefilled URL: template + labels + a title + the auto-filled metadata
    // textarea (matched by id in crash.yml).
    QUrl full(QString::fromLatin1(kIssuesNewPath));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("template"), QString::fromLatin1(kTemplateFile));
    q.addQueryItem(QStringLiteral("labels"), QString::fromLatin1(kLabel));

    // The crash.yml form sets title: "[crash] "; we append a short suffix so the
    // issue list is scannable. No paths/usernames — version + exception only.
    QString title = QStringLiteral("[crash] ");
    if (!data.app_version.isEmpty())
        title += data.app_version;
    if (!data.exception.isEmpty())
        title += QStringLiteral(" · ") + data.exception;
    q.addQueryItem(QStringLiteral("title"), title);

    q.addQueryItem(QString::fromLatin1(kMetadataFieldId), BuildCrashMetadataBlock(data));

    full.setQuery(q);
    const QString full_url = full.toString(QUrl::FullyEncoded);

    return full_url.size() <= kUrlBudget ? full_url : bare_url;
}

} // namespace exosnap::services
