#include "HwndAuditReport.h"

#include <QStringList>

namespace exosnap::hwnd_audit {
namespace {

QString FormatHandle(quintptr handle) {
    return QStringLiteral("0x%1").arg(handle, 8, 16, QLatin1Char('0'));
}

QString FormatRect(const QRect& rect) {
    return QStringLiteral("%1x%2 @ %3,%4").arg(rect.width()).arg(rect.height()).arg(rect.x()).arg(rect.y());
}

} // namespace

QVector<HwndAuditViolation> FindRegionsCoveredByNativeChildren(const QVector<ProtectedRegion>& regions,
                                                               const QVector<NativeWindowNode>& children) {
    QVector<HwndAuditViolation> violations;
    for (const ProtectedRegion& region : regions) {
        if (region.screen_rect.isEmpty())
            continue;
        for (const NativeWindowNode& child : children) {
            if (!child.visible)
                continue;
            const QRect covered = child.screen_rect.intersected(region.screen_rect);
            if (covered.isEmpty())
                continue;
            violations.push_back(
                HwndAuditViolation{region.name, region.screen_rect, child.handle, child.description, covered});
        }
    }
    return violations;
}

QString FormatHwndAuditReport(const NativeWindowNode& top_level, const QVector<NativeWindowNode>& children,
                              const QVector<ProtectedRegion>& regions, const QVector<HwndAuditViolation>& violations) {
    QStringList lines;
    lines << QStringLiteral("native window tree");
    lines << QStringLiteral("  top-level  %1  %2  %3")
                 .arg(FormatHandle(top_level.handle), top_level.description, FormatRect(top_level.screen_rect));
    if (children.isEmpty()) {
        lines << QStringLiteral("  (no native children)");
    } else {
        for (const NativeWindowNode& child : children) {
            lines << QStringLiteral("  child      %1  %2  %3%4")
                         .arg(FormatHandle(child.handle), child.description, FormatRect(child.screen_rect),
                              child.visible ? QString() : QStringLiteral("  [hidden]"));
        }
    }

    lines << QString();
    lines << QStringLiteral("protected regions");
    for (const ProtectedRegion& region : regions) {
        QVector<HwndAuditViolation> region_violations;
        for (const HwndAuditViolation& violation : violations) {
            if (violation.region_name == region.name)
                region_violations.push_back(violation);
        }

        if (region_violations.isEmpty()) {
            lines << QStringLiteral("  OK    %1 (%2) is owned by the top-level window")
                         .arg(region.name, FormatRect(region.screen_rect));
            continue;
        }
        for (const HwndAuditViolation& violation : region_violations) {
            lines << QStringLiteral("  FAIL  %1 (%2) is covered by native child %3 (%4) over %5")
                         .arg(region.name, FormatRect(region.screen_rect), FormatHandle(violation.handle),
                              violation.description, FormatRect(violation.covered));
        }
    }

    lines << QString();
    if (violations.isEmpty()) {
        lines << QStringLiteral("verdict: every protected region is hit-testable by the top-level window.");
    } else {
        lines << QStringLiteral("verdict: %1 region/child overlap(s). Windows routes WM_NCHITTEST only to the")
                     .arg(violations.size());
        lines << QStringLiteral("window owning the pixel, so the top-level is never asked in those areas —");
        lines << QStringLiteral("native window chrome cannot work there regardless of the handler.");
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace exosnap::hwnd_audit
