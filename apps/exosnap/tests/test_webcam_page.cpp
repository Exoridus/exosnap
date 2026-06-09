#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
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

TEST_F(WebcamPageTest, NoVisibleOverlayOrMvpWording) {
    WebcamPage page;

    // Overlay placement and aspect-lock controls are off-layout (hidden) for
    // MVP. Chroma controls are now visible in the main layout.
    for (auto* label : page.findChildren<QLabel*>()) {
        if (!label->isVisibleTo(&page))
            continue;
        const QString text = label->text();
        EXPECT_FALSE(text.contains(QStringLiteral("Overlay"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("Lock aspect"), Qt::CaseInsensitive)) << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("not available in this MVP"), Qt::CaseInsensitive))
            << text.toStdString();
        EXPECT_FALSE(text.contains(QStringLiteral("Target preview"), Qt::CaseInsensitive)) << text.toStdString();
    }
}

TEST_F(WebcamPageTest, ChromaKey_ControlsVisible) {
    WebcamPage page;

    bool chroma_label_found = false;
    for (auto* label : page.findChildren<QLabel*>()) {
        if (!label->isVisibleTo(&page))
            continue;
        if (label->text() == QStringLiteral("Chroma Key")) {
            chroma_label_found = true;
            break;
        }
    }
    EXPECT_TRUE(chroma_label_found);

    // All four color mode buttons must be present and accessible
    auto buttons = page.findChildren<QPushButton*>();
    const auto has_btn = [&](const QString& text) {
        for (auto* b : buttons)
            if (b->text() == text)
                return true;
        return false;
    };
    EXPECT_TRUE(has_btn(QStringLiteral("Green")));
    EXPECT_TRUE(has_btn(QStringLiteral("Blue")));
    EXPECT_TRUE(has_btn(QStringLiteral("Magenta")));
    EXPECT_TRUE(has_btn(QStringLiteral("Custom...")));
}

TEST_F(WebcamPageTest, ChromaControls_RemainEnabled_WhenRecordingLocked) {
    WebcamPage page;
    page.setRecordingControlsLocked(true);

    const QStringList chroma_btns = {QStringLiteral("Green"), QStringLiteral("Blue"), QStringLiteral("Magenta"),
                                     QStringLiteral("Custom...")};
    for (auto* b : page.findChildren<QPushButton*>()) {
        if (chroma_btns.contains(b->text()))
            EXPECT_TRUE(b->isEnabled()) << b->text().toStdString() << " must stay enabled when recording";
    }

    for (auto* slider : page.findChildren<QSlider*>())
        EXPECT_TRUE(slider->isEnabled()) << "chroma slider must stay enabled when recording";
}

TEST_F(WebcamPageTest, DeviceCombo_DisabledWhenRecordingLocked) {
    WebcamPage page;
    page.setRecordingControlsLocked(true);

    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled());
}

TEST_F(WebcamPageTest, ChromaControls_EnabledWhenLockReleased) {
    WebcamPage page;
    page.setRecordingControlsLocked(true);
    page.setRecordingControlsLocked(false);

    const QStringList chroma_btns = {QStringLiteral("Green"), QStringLiteral("Blue"), QStringLiteral("Magenta"),
                                     QStringLiteral("Custom...")};
    for (auto* b : page.findChildren<QPushButton*>()) {
        if (chroma_btns.contains(b->text()))
            EXPECT_TRUE(b->isEnabled()) << b->text().toStdString() << " must re-enable after unlock";
    }

    auto* combo = page.findChild<QComboBox*>();
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled());
}

TEST_F(WebcamPageTest, SettingsChanged_EmittedOnColorModeButtonClick) {
    WebcamPage page;

    int count = 0;
    WebcamChromaKeyColorMode last_mode = WebcamChromaKeyColorMode::Green;
    QObject::connect(&page, &WebcamPage::settingsChanged, [&](const WebcamSettings& s) {
        ++count;
        last_mode = s.chroma_key.color_mode;
    });

    for (auto* b : page.findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("Blue")) {
            b->click();
            break;
        }
    }

    EXPECT_GE(count, 1);
    EXPECT_EQ(last_mode, WebcamChromaKeyColorMode::Blue);
}

TEST_F(WebcamPageTest, SettingsChanged_EmittedOnToleranceSliderChange) {
    WebcamPage page;

    WebcamSettings init;
    init.chroma_key.enabled = true;
    page.applySettings(init);

    int count = 0;
    QObject::connect(&page, &WebcamPage::settingsChanged, [&](const WebcamSettings&) { ++count; });

    auto sliders = page.findChildren<QSlider*>();
    ASSERT_FALSE(sliders.isEmpty());
    const int orig = sliders.front()->value();
    sliders.front()->setValue(orig + 1);

    EXPECT_GE(count, 1);
}

} // namespace
} // namespace exosnap
