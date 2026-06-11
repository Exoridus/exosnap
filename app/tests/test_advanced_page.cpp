#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "pages/AdvancedPage.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "advanced_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class AdvancedPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static bool HasLabelText(const AdvancedPage& page, const QString& text) {
        const auto labels = page.findChildren<QLabel*>();
        for (const auto* l : labels) {
            if (l->text() == text)
                return true;
        }
        return false;
    }
};

TEST_F(AdvancedPageTest, Baseline_DefaultsAreNeutral_NoStaleQualityClaim) {
    AdvancedPage page;

    // Before any baseline is supplied, the panel must not assert a concrete
    // (and possibly wrong) quality such as "Balanced".
    const auto labels = page.findChildren<QLabel*>();
    for (const auto* l : labels) {
        EXPECT_FALSE(l->text().contains(QStringLiteral("Balanced")))
            << "Stale baseline value found: " << l->text().toStdString();
        EXPECT_FALSE(l->text().contains(QStringLiteral("AV1 (NVENC)")))
            << "Stale baseline value found: " << l->text().toStdString();
    }
}

TEST_F(AdvancedPageTest, SetBaseline_ReflectsHighQualityProfile) {
    AdvancedPage page;

    OutputSettingsModel output;
    output.container = capability::Container::Matroska;
    output.video_codec = capability::VideoCodec::H264Nvenc;
    output.audio_codec = capability::AudioCodec::AacMf;

    VideoSettingsModel video;
    video.quality = recorder_core::NvencQualityPreset::High;
    video.cfr = true;
    video.frame_rate_num = 60;
    video.capture_cursor = true;

    page.setBaseline(output, video, QStringLiteral("High Quality"));

    EXPECT_TRUE(HasLabelText(page, QStringLiteral("High Quality")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("MKV")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("H.264 (NVENC)")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("High  \302\267  CQ 19")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("CFR 60 fps")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("AAC")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Captured")));
}

TEST_F(AdvancedPageTest, SetBaseline_DoesNotShowBalancedWhenActiveIsHigh) {
    AdvancedPage page;

    OutputSettingsModel output;
    VideoSettingsModel video;
    video.quality = recorder_core::NvencQualityPreset::High;

    page.setBaseline(output, video, QStringLiteral("High"));

    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Balanced  \302\267  CQ 24")));
}

} // namespace
} // namespace exosnap
