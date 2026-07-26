#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QFrame>

#include "models/OutputSettingsModel.h"
#include "models/SettingsHintText.h"
#include "models/VideoSettingsModel.h"
#include "pages/ConfigPage.h"
#include "ui/widgets/CompareHint.h"
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

    // Count CompareHint widgets with a specific compare key in the page.
    static int CountCompareHintsWithKey(const ConfigPage& page, const QString& key) {
        int count = 0;
        for (const auto* h : page.findChildren<ui::widgets::CompareHint*>()) {
            if (h->compareKey() == key)
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

// D6: Container, VideoCodec, AudioCodec, Quality rows now use CompareHint instead of InfoHintIcon.
TEST_F(InfoHintsConfigTest, ConfigPage_Container_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("container"));
    EXPECT_GE(count, 1) << "Container setting must have a CompareHint (key='container')";
}

TEST_F(InfoHintsConfigTest, ConfigPage_VideoCodec_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("videoCodec"));
    EXPECT_GE(count, 1) << "Video codec setting must have a CompareHint (key='videoCodec')";
}

TEST_F(InfoHintsConfigTest, ConfigPage_AudioCodec_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("audioCodec"));
    EXPECT_GE(count, 1) << "Audio codec setting must have a CompareHint (key='audioCodec')";
}

TEST_F(InfoHintsConfigTest, ConfigPage_Quality_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("quality"));
    EXPECT_GE(count, 1) << "Quality setting must have a CompareHint (key='quality')";
}

// ---- D6: Additional CompareHint tests ----

TEST_F(InfoHintsConfigTest, ConfigPage_Container_HasCompareHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("container"));
    EXPECT_GE(count, 1) << "Container row must contain a CompareHint widget";
}

TEST_F(InfoHintsConfigTest, ConfigPage_VideoCodec_HasCompareHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("videoCodec"));
    EXPECT_GE(count, 1) << "Video codec row must contain a CompareHint widget";
}

TEST_F(InfoHintsConfigTest, ConfigPage_AudioCodec_HasCompareHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountCompareHintsWithKey(page, QStringLiteral("audioCodec"));
    EXPECT_GE(count, 1) << "Audio codec row must contain a CompareHint widget";
}

TEST_F(InfoHintsConfigTest, ConfigPage_WebMWithH264_ShowsCompatCallout) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Simulate selecting WebM container + H.264 video codec (incompatible combo).
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::WebM;
    settings.video_codec = capability::VideoCodec::H264Nvenc;
    settings.audio_codec = capability::AudioCodec::Opus;
    page.setOutputSettings(settings);

    // The compat callout should not be explicitly hidden (setVisible(true) was called).
    const auto* callout = page.findChild<QFrame*>(QStringLiteral("compatCalloutWidget"));
    ASSERT_NE(callout, nullptr) << "compatCalloutWidget must exist";
    EXPECT_FALSE(callout->isHidden()) << "compatCalloutWidget must not be hidden for WebM+H.264";
}

TEST_F(InfoHintsConfigTest, ConfigPage_FrameRate_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kFrameRate);
    EXPECT_GE(count, 1) << "Frame rate setting must have an InfoHint";
}

TEST_F(InfoHintsConfigTest, ConfigPage_CaptureCursor_HasNoInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    // v0.9 polish: "Capture cursor" is a self-explanatory boolean, not an A/B tradeoff,
    // so it deliberately carries no info-i (icon reserved for rows with a real choice).
    const int count = CountHintsWithText(page, ui::hints::kCaptureCursor);
    EXPECT_EQ(count, 0) << "Capture cursor is a plain boolean and must not carry an info-i";
}

TEST_F(InfoHintsConfigTest, ConfigPage_OutputResolution_HasCompareHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    // D6: Output resolution is a multi-option control and uses a CompareHint
    // (key "resolution"), not the single-line InfoHint it had before.
    const int count = CountCompareHintsWithKey(page, QStringLiteral("resolution"));
    EXPECT_GE(count, 1) << "Output resolution row must contain a CompareHint widget";
}

TEST_F(InfoHintsConfigTest, ConfigPage_AudioSourceEnable_HasNoInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    // v0.9 polish: a source row carries at most one info-i. The plain enable toggle drops
    // its icon; the genuine tradeoff (merge vs separate track) keeps it — see SeparateTrack.
    const int count = CountHintsWithText(page, ui::hints::kAudioSourceEnable);
    EXPECT_EQ(count, 0) << "The enable toggle must not carry an info-i (one per source row)";
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
    // Task 7: split-by-time / split-by-size is Default tier, built eagerly from the
    // constructor — no expert-mode-enable needed for the hint to exist.
    ConfigPage page(output_defaults_, video_defaults_);

    const int count = CountHintsWithText(page, ui::hints::kSplitRecording);
    EXPECT_GE(count, 1) << "Split by time setting must have an InfoHint";
}

// Colour range (fmt_expert_section_) is constructed eagerly (unlike the lazily-built
// Advanced/Audio expert subtrees), but the row lives behind Expert mode; enable it
// explicitly so the assertion never depends on incidental build-order behaviour.
TEST_F(InfoHintsConfigTest, ConfigPage_VideoColorRange_HasInfoHint) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    const int count = CountHintsWithText(page, ui::hints::kVideoColorRange);
    EXPECT_GE(count, 1) << "Colour range setting must have an InfoHint (inside the Expert Video section)";
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
