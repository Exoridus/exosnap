#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>

#include "models/OutputSettingsModel.h"
#include "models/SettingsHintText.h"
#include "models/VideoSettingsModel.h"
#include "pages/ConfigPage.h"
#include "ui/widgets/InfoHintIcon.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "info_hints_config_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class InfoHintsConfigTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    // Count InfoHintIcon widgets with a specific hint text in the page.
    static int CountHintsWithText(const ConfigPage& page, const QString& text) {
        int count = 0;
        for (const auto* h : page.findChildren<ui::widgets::InfoHintIcon*>()) {
            if (h->hintText() == text)
                ++count;
        }
        return count;
    }

    OutputSettingsModel output_defaults_;
    VideoSettingsModel video_defaults_;
};

// ---- InfoHint presence tests for ConfigPage ----

TEST_F(InfoHintsConfigTest, ConfigPage_HasAtLeastOneInfoHintIcon) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto hints = page.findChildren<ui::widgets::InfoHintIcon*>();
    EXPECT_GT(static_cast<int>(hints.size()), 0) << "ConfigPage must contain at least one InfoHintIcon";
}

TEST_F(InfoHintsConfigTest, ConfigPage_Container_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kContainer);
    EXPECT_GE(count, 1) << "Container setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_VideoCodec_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kVideoCodecAv1);
    EXPECT_GE(count, 1) << "Video codec setting must have an InfoHint (AV1 hint)";
}

TEST_F(InfoHintsConfigTest, ConfigPage_AudioCodec_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kAudioCodecOpus);
    EXPECT_GE(count, 1) << "Audio codec setting must have an InfoHint (Opus hint)";
}

TEST_F(InfoHintsConfigTest, ConfigPage_Quality_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kQualityPreset);
    EXPECT_GE(count, 1) << "Quality setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_FrameRate_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kFrameRate);
    EXPECT_GE(count, 1) << "Frame rate setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_CaptureCursor_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kCaptureCursor);
    EXPECT_GE(count, 1) << "Capture cursor setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_OutputResolution_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kOutputResolution);
    EXPECT_GE(count, 1) << "Output resolution setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_AudioSourceEnable_HasInfoHints) {
    ConfigPage page(output_defaults_, video_defaults_);

    // At least sys + mic source enable hints (app row may be hidden for Display target).
    const int count = CountHintsWithText(page, ui::hints::kAudioSourceEnable);
    EXPECT_GE(count, 2) << "Audio source enable hints must be present for at least sys and mic sources";
}

TEST_F(InfoHintsConfigTest, ConfigPage_SeparateTrack_HasInfoHints) {
    ConfigPage page(output_defaults_, video_defaults_);

    // At least sys + mic separate-track hints.
    const int count = CountHintsWithText(page, ui::hints::kSeparateTrack);
    EXPECT_GE(count, 2) << "Separate track hints must be present for at least sys and mic sources";
}

TEST_F(InfoHintsConfigTest, ConfigPage_OutputFolder_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kOutputFolder);
    EXPECT_GE(count, 1) << "Output folder setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_FilenamePattern_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kFilenamePattern);
    EXPECT_GE(count, 1) << "Filename pattern setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_SplitRecording_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kSplitRecording);
    EXPECT_GE(count, 1) << "Split recording setting must have an InfoHint (inside the Advanced expander)";
}

TEST_F(InfoHintsConfigTest, ConfigPage_InfoHints_AreAllFocusable) {
    ConfigPage page(output_defaults_, video_defaults_);

    for (const auto* hint : page.findChildren<ui::widgets::InfoHintIcon*>()) {
        EXPECT_EQ(hint->focusPolicy(), Qt::TabFocus)
            << "Every InfoHintIcon must be keyboard-focusable via Tab; hintText: " << hint->hintText().toStdString();
    }
}

TEST_F(InfoHintsConfigTest, ConfigPage_InfoHints_HaveNonEmptyToolTip) {
    ConfigPage page(output_defaults_, video_defaults_);

    for (const auto* hint : page.findChildren<ui::widgets::InfoHintIcon*>()) {
        EXPECT_FALSE(hint->toolTip().isEmpty())
            << "Every InfoHintIcon must have a non-empty tooltip; hintText: " << hint->hintText().toStdString();
    }
}

TEST_F(InfoHintsConfigTest, ConfigPage_InfoHints_HaveAccessibleName) {
    ConfigPage page(output_defaults_, video_defaults_);

    for (const auto* hint : page.findChildren<ui::widgets::InfoHintIcon*>()) {
        EXPECT_FALSE(hint->accessibleName().isEmpty())
            << "Every InfoHintIcon must have an accessible name for screen readers";
    }
}

} // namespace
} // namespace exosnap
