// UpdateFeedOverride.cpp -- see header. Pure argv inspection, nothing else.

#include "UpdateFeedOverride.h"

#include <update/update_checker.h>

namespace exosnap::services {

bool IsAcceptableFeedUrl(const QString& url) {
    // https only. FetchReleasesJson refuses anything else anyway; refusing here
    // turns a typo into a refused launch rather than a check that fails later
    // with a network error nobody connects to the command line.
    constexpr QLatin1String kScheme("https://");
    if (!url.startsWith(kScheme))
        return false;
    const QString rest = url.mid(kScheme.size());
    const qsizetype slash = rest.indexOf(QLatin1Char('/'));
    const QString host_port = slash < 0 ? rest : rest.left(slash);
    return !host_port.isEmpty();
}

UpdateFeedOverride ParseUpdateFeedOverride(const QStringList& args) {
    UpdateFeedOverride out;
    const qsizetype index = args.indexOf(QString::fromLatin1(kUpdateFeedOverrideFlag));
    if (index < 0)
        return out;

    out.requested = true;

    if constexpr (exosnap::update::IsUpdateCheckEnabled()) {
        // Official build: refuse, and say why. A shipped artifact whose update
        // source can be redirected from a command line is a different product
        // from the one that was reviewed.
        out.error = QStringLiteral("%1 is a development option and is not accepted in an official build")
                        .arg(QString::fromLatin1(kUpdateFeedOverrideFlag));
        return out;
    }

    if (index + 1 >= args.size()) {
        out.error = QStringLiteral("%1 requires an https:// URL").arg(QString::fromLatin1(kUpdateFeedOverrideFlag));
        return out;
    }

    const QString url = args.at(index + 1);
    if (!IsAcceptableFeedUrl(url)) {
        out.error =
            QStringLiteral("%1 must be an https:// URL with a host").arg(QString::fromLatin1(kUpdateFeedOverrideFlag));
        return out;
    }

    out.base_url = url;
    return out;
}

} // namespace exosnap::services
