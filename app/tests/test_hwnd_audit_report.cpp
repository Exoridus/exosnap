#include <gtest/gtest.h>

#include "hwnd_audit/HwndAuditReport.h"

#include <QRect>
#include <QString>
#include <QVector>

namespace exosnap::hwnd_audit {
namespace {

// The title bar as the production window lays it out: full width, 40 px tall,
// at the top of the client area (physical pixels, as the harness collects them).
constexpr int kTitleBarHeight = 40;

ProtectedRegion TitleBar() {
    return ProtectedRegion{QStringLiteral("title bar"), QRect(0, 0, 1280, kTitleBarHeight)};
}

NativeWindowNode Child(const QRect& rect, bool visible = true, const QString& description = {}) {
    NativeWindowNode node;
    node.handle = 0x000A0B31;
    node.parent = 0x000A0B2C;
    node.description = description.isEmpty() ? QStringLiteral("QWidget(container)") : description;
    node.screen_rect = rect;
    node.visible = visible;
    return node;
}

TEST(HwndAuditReportTest, NoNativeChildrenLeavesTheRegionHitTestable) {
    const auto violations = FindRegionsCoveredByNativeChildren({TitleBar()}, {});

    EXPECT_TRUE(violations.isEmpty());
}

// The 2026-08-08 case: the container spanning the whole client area is an
// ANCESTOR of the DXGI preview, so Qt makes it native — and it starts at y=0,
// covering the title bar. WM_NCHITTEST then never reaches the top-level there.
TEST(HwndAuditReportTest, ChildSpanningTheClientAreaCoversTheTitleBar) {
    const auto violations = FindRegionsCoveredByNativeChildren({TitleBar()}, {Child(QRect(0, 0, 1280, 1000))});

    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].region_name, QStringLiteral("title bar"));
    EXPECT_EQ(violations[0].covered, QRect(0, 0, 1280, kTitleBarHeight));
}

// The repaired layout: the preview subtree starts below the title bar, so no
// native window owns those pixels and the top-level is asked again.
TEST(HwndAuditReportTest, ChildBelowTheTitleBarIsNotAViolation) {
    const auto violations =
        FindRegionsCoveredByNativeChildren({TitleBar()}, {Child(QRect(0, kTitleBarHeight, 1280, 960))});

    EXPECT_TRUE(violations.isEmpty());
}

// A single overlapping row is still a violation: the region has to be owned by
// the top-level in full, not mostly.
TEST(HwndAuditReportTest, PartialOverlapIsReportedWithTheIntersectionOnly) {
    const auto violations = FindRegionsCoveredByNativeChildren({TitleBar()}, {Child(QRect(400, 39, 200, 500))});

    ASSERT_EQ(violations.size(), 1);
    EXPECT_EQ(violations[0].covered, QRect(400, 39, 200, 1));
}

// A hidden window owns no pixels, so it cannot intercept hit-testing.
TEST(HwndAuditReportTest, HiddenChildrenAreIgnored) {
    const auto violations =
        FindRegionsCoveredByNativeChildren({TitleBar()}, {Child(QRect(0, 0, 1280, 1000), /*visible=*/false)});

    EXPECT_TRUE(violations.isEmpty());
}

TEST(HwndAuditReportTest, EmptyRegionIsSkippedRatherThanMatchingEverything) {
    const ProtectedRegion missing{QStringLiteral("title bar"), QRect()};

    const auto violations = FindRegionsCoveredByNativeChildren({missing}, {Child(QRect(0, 0, 1280, 1000))});

    EXPECT_TRUE(violations.isEmpty());
}

TEST(HwndAuditReportTest, EveryOverlappingChildIsReportedSeparately) {
    const QVector<NativeWindowNode> children = {Child(QRect(0, 0, 1280, 1000), true, QStringLiteral("QWidget(root)")),
                                                Child(QRect(0, 0, 200, 40), true, QStringLiteral("QWidget(nav)"))};

    const auto violations = FindRegionsCoveredByNativeChildren({TitleBar()}, children);

    ASSERT_EQ(violations.size(), 2);
    EXPECT_EQ(violations[0].description, QStringLiteral("QWidget(root)"));
    EXPECT_EQ(violations[1].description, QStringLiteral("QWidget(nav)"));
}

NativeWindowNode TopLevel() {
    NativeWindowNode node;
    node.handle = 0x000A0B2C;
    node.description = QStringLiteral("MainWindow");
    node.screen_rect = QRect(0, 0, 1280, 1040);
    node.visible = true;
    return node;
}

TEST(HwndAuditReportTest, ReportStatesTheVerdictForACleanTree) {
    const QString report = FormatHwndAuditReport(TopLevel(), {}, {TitleBar()}, {});

    EXPECT_TRUE(report.contains(QStringLiteral("OK    title bar")));
    EXPECT_TRUE(report.contains(QStringLiteral("verdict: every protected region is hit-testable")));
}

TEST(HwndAuditReportTest, ReportNamesTheOffendingWindowForACoveredRegion) {
    const QVector<NativeWindowNode> children = {Child(QRect(0, 0, 1280, 1000))};
    const auto violations = FindRegionsCoveredByNativeChildren({TitleBar()}, children);

    const QString report = FormatHwndAuditReport(TopLevel(), children, {TitleBar()}, violations);

    EXPECT_TRUE(report.contains(QStringLiteral("FAIL  title bar")));
    EXPECT_TRUE(report.contains(QStringLiteral("QWidget(container)")));
    EXPECT_TRUE(report.contains(QStringLiteral("0x000a0b31")));
}

TEST(HwndAuditReportTest, ReportMarksHiddenChildrenSoACleanVerdictIsExplainable) {
    const QVector<NativeWindowNode> children = {Child(QRect(0, 0, 1280, 1000), /*visible=*/false)};

    const QString report = FormatHwndAuditReport(TopLevel(), children, {TitleBar()}, {});

    EXPECT_TRUE(report.contains(QStringLiteral("[hidden]")));
}

} // namespace
} // namespace exosnap::hwnd_audit
