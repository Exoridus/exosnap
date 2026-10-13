#include "models/HiddenCaptureTargets.h"

#include <gtest/gtest.h>

using namespace exosnap;

namespace {

HiddenCaptureWindow window(int generation, QString title = QStringLiteral("Editor"),
                           QString app = QStringLiteral("C:/Apps/editor.exe")) {
    return {QStringLiteral("window:%1").arg(generation), title, title, QStringLiteral("Editor"), app};
}

TEST(HiddenCaptureTargets, EmptyAndLargeSnapshotsKeepAllEligibleWindows) {
    HiddenCaptureTargets targets;
    EXPECT_TRUE(targets.view({}, false).windows.isEmpty());
    for (int count : {3, 100}) {
        QVector<HiddenCaptureWindow> windows;
        for (int i = 0; i < count; ++i)
            windows.push_back(window(i));
        targets.setWindows(std::move(windows));
        EXPECT_EQ(targets.view({}, false).windows.size(), count);
    }
}

TEST(HiddenCaptureTargets, SessionHideTracksLifetimeIdentityAcrossReorderAndDisappearance) {
    HiddenCaptureTargets targets;
    targets.setWindows({window(1), window(2), window(3)});
    ASSERT_TRUE(targets.hideSession(QStringLiteral("window:2")));
    targets.setWindows({window(3), window(2), window(1)});
    EXPECT_EQ(targets.view({}, false).hidden_count, 1);
    EXPECT_EQ(targets.view({}, false).windows.size(), 2);
    targets.setWindows({window(3), window(1)});
    EXPECT_FALSE(targets.resolve(QStringLiteral("window:2")).has_value());
    targets.setWindows({window(2), window(3), window(1)});
    EXPECT_EQ(targets.view({}, false).hidden_count, 1);
    targets.setWindows({window(3), window(4), window(1)});
    EXPECT_EQ(targets.view({}, false).hidden_count, 0);
    EXPECT_EQ(targets.view({}, false).windows.size(), 3);
    HiddenCaptureTargets restarted;
    restarted.setWindows({window(2)});
    EXPECT_EQ(restarted.view({}, false).hidden_count, 0);
}

TEST(HiddenCaptureTargets, DuplicateEnumerationDoesNotInflateHiddenCount) {
    HiddenCaptureTargets targets;
    targets.setWindows({window(1), window(1), window(2)});
    ASSERT_TRUE(targets.hideSession(QStringLiteral("window:1")));
    EXPECT_EQ(targets.view({}, false).hidden_count, 1);
    EXPECT_EQ(targets.view({}, true).windows.size(), 2);
}

TEST(HiddenCaptureTargets, RecycledHandleOrPidDoesNotInheritSessionHide) {
    HiddenCaptureTargets targets;
    auto old_window = window(1);
    old_window.identity = QStringLiteral("hwnd:73:pid:9:created:100:lifetime:1");
    targets.setWindows({old_window});
    ASSERT_TRUE(targets.hideSession(old_window.identity));

    auto recycled_handle = window(2);
    recycled_handle.identity = QStringLiteral("hwnd:73:pid:9:created:100:lifetime:2");
    targets.setWindows({recycled_handle});
    EXPECT_EQ(targets.view({}, false).hidden_count, 0);

    auto recycled_pid = window(3);
    recycled_pid.identity = QStringLiteral("hwnd:73:pid:9:created:200:lifetime:3");
    targets.setWindows({recycled_pid});
    EXPECT_EQ(targets.view({}, false).hidden_count, 0);
    EXPECT_FALSE(targets.resolve(old_window.identity).has_value());
}

TEST(HiddenCaptureTargets, TitleRulesAreScopedAndOverlapsCountOnceBeforeSearch) {
    HiddenCaptureTargets targets;
    targets.setWindows({window(1, QStringLiteral("Report A")), window(2, QStringLiteral("Report B")),
                        window(3, QStringLiteral("Report C"), QStringLiteral("C:/Apps/other.exe"))});
    const QString scoped = targets.addTitleRule(QStringLiteral("report"), HiddenCaptureTitleMatch::Contains, false,
                                                QStringLiteral("C:/Apps/editor.exe"));
    const QString exact = targets.addTitleRule(QStringLiteral("Report A"), HiddenCaptureTitleMatch::Equals, true, {});
    ASSERT_FALSE(scoped.isEmpty());
    ASSERT_FALSE(exact.isEmpty());
    EXPECT_EQ(targets.affectedCount(scoped), 2);
    EXPECT_EQ(targets.affectedCount(exact), 1);
    const auto result = targets.view(QStringLiteral("C"), false);
    EXPECT_EQ(result.hidden_count, 2);
    EXPECT_EQ(result.search_match_count, 1);
    ASSERT_EQ(result.windows.size(), 1);
    EXPECT_EQ(result.windows[0].identity, QStringLiteral("window:3"));
    EXPECT_EQ(targets.view({}, true).windows.size(), 3);
    ASSERT_TRUE(targets.setRuleEnabled(scoped, false));
    EXPECT_EQ(targets.view({}, false).hidden_count, 1);
    ASSERT_TRUE(targets.removeRule(exact));
    EXPECT_EQ(targets.view({}, false).hidden_count, 0);
}

TEST(HiddenCaptureTargets, ApplicationHideRequiresConfirmationAndExactIdentity) {
    HiddenCaptureTargets targets;
    targets.setWindows({window(1), window(2), window(3, {}, QStringLiteral("C:/Apps/editor-helper.exe"))});
    EXPECT_TRUE(targets.addApplicationRule(QStringLiteral("C:/Apps/editor.exe"), false).isEmpty());
    EXPECT_EQ(targets.view({}, false).hidden_count, 0);
    const QString id = targets.addApplicationRule(QStringLiteral("C:/Apps/editor.exe"), true);
    ASSERT_FALSE(id.isEmpty());
    EXPECT_EQ(targets.affectedCount(id), 2);
    EXPECT_EQ(targets.view({}, false).hidden_count, 2);
}

TEST(HiddenCaptureTargets, SavedRulesDoNotRetainSessionDecisions) {
    HiddenCaptureTargets first;
    first.setWindows({window(1), window(2)});
    ASSERT_TRUE(first.hideSession(QStringLiteral("window:1")));
    ASSERT_FALSE(first
                     .addTitleRule(QStringLiteral("Editor"), HiddenCaptureTitleMatch::Equals, true,
                                   QStringLiteral("C:/Apps/editor.exe"))
                     .isEmpty());
    HiddenCaptureTargets restarted;
    restarted.setRules(first.rules());
    restarted.setWindows({window(3), window(4)});
    EXPECT_EQ(restarted.view({}, false).hidden_count, 2);
    ASSERT_TRUE(restarted.removeRule(first.rules()[0].id));
    EXPECT_EQ(restarted.view({}, false).hidden_count, 0);
}

} // namespace
