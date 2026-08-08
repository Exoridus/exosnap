#pragma once

#include <QRect>
#include <QString>
#include <QVector>

namespace exosnap::hwnd_audit {

// One native window (HWND) in the audited tree. Populated from Win32 by the
// harness; kept free of Win32 types so the evaluation below stays testable
// without a real window.
struct NativeWindowNode {
    quintptr handle = 0;
    quintptr parent = 0;
    QString description; // Qt class + objectName when the HWND maps to a QWidget
    QRect screen_rect;   // screen coordinates, matching Win32 GetWindowRect
    bool visible = false;
};

// A screen region the top-level window must be able to answer WM_NCHITTEST for.
struct ProtectedRegion {
    QString name;
    QRect screen_rect;
};

// A native child overlapping a protected region. Windows routes WM_NCHITTEST
// only to the window that owns the pixel under the cursor, so any overlap here
// means the top-level is never asked at those pixels — however correct its
// handler is.
struct HwndAuditViolation {
    QString region_name;
    QRect region_rect;
    quintptr handle = 0;
    QString description;
    QRect covered; // intersection of the child with the region
};

// PURE. Returns every (region, native child) overlap, in the order the regions
// and children were given. Invisible children are ignored: they own no pixels
// and so never intercept hit-testing.
QVector<HwndAuditViolation> FindRegionsCoveredByNativeChildren(const QVector<ProtectedRegion>& regions,
                                                               const QVector<NativeWindowNode>& children);

// PURE. Human-readable report: the window tree, then the verdict per region.
QString FormatHwndAuditReport(const NativeWindowNode& top_level, const QVector<NativeWindowNode>& children,
                              const QVector<ProtectedRegion>& regions, const QVector<HwndAuditViolation>& violations);

} // namespace exosnap::hwnd_audit
