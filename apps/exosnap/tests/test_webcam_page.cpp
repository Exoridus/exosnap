#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QWidget>

#include "pages/WebcamPage.h"
#include "ui/widgets/CameraPreview.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "webcam_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

// Constructing WebcamPage enumerates capture devices (a robust static MF call
// that returns an empty list when no camera/MF is present) but never opens a
// camera — preview capture only starts on showEvent(). These tests therefore
// never touch real webcam hardware.
class WebcamPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(WebcamPageTest, Constructs_CameraPreviewWidgetPresent) {
    WebcamPage page;

    auto* preview = page.findChild<ui::widgets::CameraPreview*>();
    EXPECT_NE(preview, nullptr);
    EXPECT_NE(page.findChild<QWidget*>(QStringLiteral("webcamCameraPreview")), nullptr);
}

TEST_F(WebcamPageTest, CameraPreviewLabelExists) {
    WebcamPage page;

    bool found = false;
    for (auto* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Camera preview")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(WebcamPageTest, IncludeWebcamToggleLabelExists) {
    WebcamPage page;

    bool found = false;
    for (auto* label : page.findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Include webcam in recording")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(WebcamPageTest, NoVisibleOverlayChromaOrMvpWording) {
    WebcamPage page;

    // Only inspect labels that would be shown when the page is visible. The
    // chroma/overlay widgets are intentionally retained off-layout (hidden) for
    // settings round-trip, so they must be excluded from the stale-copy check.
    for (auto* label : page.findChildren<QLabel*>()) {
        if (!label->isVisibleTo(&page))
            continue;
        const QString text = label->text();
        EXPECT_FALSE(text.contains(QStringLiteral("Overlay"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("Chroma"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("Lock aspect"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("not available in this MVP"), Qt::CaseInsensitive))
            << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("Target preview"), Qt::CaseInsensitive)) << text.toStdString();
    }
}

} // namespace
} // namespace exosnap
