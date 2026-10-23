#include "CaptureExclusion.h"

#include <QGuiApplication>
#include <QImage>
#include <QScopeGuard>

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

TEST(CaptureExclusionPreview, ExplicitOffscreenScreenshotAllowsComposition) {
    EXPECT_TRUE(CaptureExclusion::RenderOnlyPreviewAllowed(
        QStringLiteral("offscreen"),
        {QStringLiteral("exosnap"), QStringLiteral("--visual-test"), QStringLiteral("review.png")}));
}

TEST(CaptureExclusionPreview, NativePlatformsNeverAllowCompositionWithoutExclusion) {
    const QStringList arguments{QStringLiteral("exosnap"), QStringLiteral("--visual-test"),
                                QStringLiteral("review.png")};
    for (const auto& platform :
         {QStringLiteral("windows"), QStringLiteral("minimal"), QStringLiteral("xcb"), QStringLiteral("wayland"),
          QStringLiteral("vnc"), QStringLiteral("Offscreen"), QStringLiteral("offscreen:windows")})
        EXPECT_FALSE(CaptureExclusion::RenderOnlyPreviewAllowed(platform, arguments)) << platform.toStdString();
}

TEST(CaptureExclusionPreview, NormalOffscreenAndIncompleteRequestsRemainFailClosed) {
    for (const QStringList& arguments :
         {QStringList{QStringLiteral("exosnap")},
          QStringList{QStringLiteral("exosnap"), QStringLiteral("--visual-test")},
          QStringList{QStringLiteral("exosnap"), QStringLiteral("--visual-test"), QString()},
          QStringList{QStringLiteral("exosnap"), QStringLiteral("--visual-test"), QStringLiteral("--visual-page")},
          QStringList{QStringLiteral("exosnap"), QStringLiteral("--overlay-visual-state"),
                      QStringLiteral("hud-recording")}})
        EXPECT_FALSE(CaptureExclusion::RenderOnlyPreviewAllowed(QStringLiteral("offscreen"), arguments));
}

TEST(CaptureExclusionPreview, CompositionPermissionDoesNotGrantPlatformAuthority) {
    ASSERT_EQ(QGuiApplication::instance(), nullptr);
    const auto original_platform = qgetenv("QT_QPA_PLATFORM");
    const auto original_backend = qgetenv("QT_QUICK_BACKEND");
    const auto restore_platform = qScopeGuard([&] {
        if (original_platform.isNull())
            qunsetenv("QT_QPA_PLATFORM");
        else
            qputenv("QT_QPA_PLATFORM", original_platform);
        if (original_backend.isNull())
            qunsetenv("QT_QUICK_BACKEND");
        else
            qputenv("QT_QUICK_BACKEND", original_backend);
    });
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    int argc = 3;
    char name[] = "capture-preview";
    char flag[] = "--visual-test";
    char output[] = "review.png";
    char* argv[] = {name, flag, output, nullptr};
    QGuiApplication application(argc, argv);
    CaptureExclusion exclusion;
    ASSERT_TRUE(exclusion.renderOnlyPreview());
    int affinity_calls = 0;
    CaptureExclusion::SetAffinityFunctionForTest([&](void*, quint32) {
        ++affinity_calls;
        return true;
    });
    const auto restore_affinity = qScopeGuard(CaptureExclusion::ResetAffinityFunctionForTest);
    QQuickWindow window;
    exclusion.setTarget(&window);
    window.resize(64, 64);
    window.show();
    window.requestUpdate();
    QCoreApplication::processEvents();
    EXPECT_FALSE(window.grabWindow().isNull());
    window.hide();
    window.destroy();
    QCoreApplication::processEvents();
    EXPECT_EQ(affinity_calls, 0);
    EXPECT_FALSE(exclusion.granted());
    EXPECT_FALSE(exclusion.resolved());
    EXPECT_EQ(exclusion.target(), &window);
}

} // namespace
} // namespace exosnap::quick
