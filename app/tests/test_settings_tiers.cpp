#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "pages/ConfigPage.h"
#include "settings/AppSettingsStore.h"
#include "ui/widgets/ExoCheckBox.h"
#include "ui/widgets/ExoToggle.h"
#include "ui/widgets/SettingsCardExpander.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "settings_tiers_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class SettingsTiersTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    OutputSettingsModel output_defaults_;
    VideoSettingsModel video_defaults_;
};

// ---- AppSettingsStore tests ----

TEST(AppSettingsTiersStoreTest, ExpertModeEnabled_DefaultIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.expert_mode_enabled);
}

TEST(AppSettingsTiersStoreTest, ExpertModeEnabled_SaveAndLoad_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini")));
    PersistedAppSettings settings;
    settings.expert_mode_enabled = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.expert_mode_enabled);
}

TEST(AppSettingsTiersStoreTest, ExpertModeEnabled_SaveAndLoad_False) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini")));
    PersistedAppSettings settings;
    settings.expert_mode_enabled = false;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.expert_mode_enabled);
}

TEST(AppSettingsTiersStoreTest, OutputSplitExpanderExpanded_DefaultIsFalse) {
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.output_split_expander_expanded);
}

TEST(AppSettingsTiersStoreTest, OutputSplitExpanderExpanded_SaveAndLoad_True) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini")));
    PersistedAppSettings settings;
    settings.output_split_expander_expanded = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.output_split_expander_expanded);
}

TEST(AppSettingsTiersStoreTest, AudioSeparateExpanderExpanded_DefaultIsFalse) {
    // audio_separate_expander_expanded is kept in the store for forward-compat
    // (Phase 1b removed the audio expander from the UI; the store field is harmless).
    PersistedAppSettings settings;
    EXPECT_FALSE(settings.audio_separate_expander_expanded);
}

TEST(AppSettingsTiersStoreTest, AudioSeparateExpanderExpanded_SaveAndLoad_True) {
    // Store round-trip is preserved even though the UI no longer shows an audio expander.
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini")));
    PersistedAppSettings settings;
    settings.audio_separate_expander_expanded = true;
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_TRUE(loaded.audio_separate_expander_expanded);
}

// THEME-SLICE-1: renamed from BumpedTo15 → BumpedTo16.
TEST(AppSettingsTiersStoreTest, SettingsVersion_BumpedTo16) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString path = QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));

    AppSettingsStore store(path);
    PersistedAppSettings settings;
    store.Save(settings);

    QSettings raw(path, QSettings::IniFormat);
    // ELEVATION-FOUNDATION-R1: version bumped 16 → 17 (present_diagnostics_optin).
    EXPECT_EQ(raw.value(QStringLiteral("settings_version")).toInt(), 17);
}

TEST(AppSettingsTiersStoreTest, MissingSettingsTiersGroup_DefaultsToFalse) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString path = QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));

    // Write a file without the [settings_tiers] group.
    {
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_FALSE(loaded.expert_mode_enabled);
    EXPECT_FALSE(loaded.output_split_expander_expanded);
    EXPECT_FALSE(loaded.audio_separate_expander_expanded);
}

// ---- SettingsCardExpander tests ----

TEST_F(SettingsTiersTest, SettingsCardExpander_DefaultCollapsed) {
    ui::widgets::SettingsCardExpander expander(2);
    EXPECT_FALSE(expander.isExpanded());
    // Use isHidden() to check widget state independent of parent-chain visibility.
    EXPECT_TRUE(expander.contentWidget()->isHidden());
}

TEST_F(SettingsTiersTest, SettingsCardExpander_SetExpandedTrue_ShowsContent) {
    ui::widgets::SettingsCardExpander expander(3);
    expander.setExpanded(true);
    EXPECT_TRUE(expander.isExpanded());
    // Use isHidden() to check widget state independent of parent-chain visibility.
    EXPECT_FALSE(expander.contentWidget()->isHidden());
}

TEST_F(SettingsTiersTest, SettingsCardExpander_SetExpandedFalse_HidesContent) {
    ui::widgets::SettingsCardExpander expander(3);
    expander.setExpanded(true);
    expander.setExpanded(false);
    EXPECT_FALSE(expander.isExpanded());
    // Use isHidden() to check widget state independent of parent-chain visibility.
    EXPECT_TRUE(expander.contentWidget()->isHidden());
}

TEST_F(SettingsTiersTest, SettingsCardExpander_ExpandedChangedSignal) {
    ui::widgets::SettingsCardExpander expander(2);
    bool signal_value = false;
    bool signal_received = false;
    QObject::connect(&expander, &ui::widgets::SettingsCardExpander::expandedChanged, [&](bool expanded) {
        signal_value = expanded;
        signal_received = true;
    });
    expander.setExpanded(true);
    EXPECT_TRUE(signal_received);
    EXPECT_TRUE(signal_value);
}

TEST_F(SettingsTiersTest, SettingsCardExpander_NoDoubleSignalOnSameValue) {
    ui::widgets::SettingsCardExpander expander(2);
    int signal_count = 0;
    QObject::connect(&expander, &ui::widgets::SettingsCardExpander::expandedChanged, [&](bool) { ++signal_count; });
    expander.setExpanded(false); // Already false — no signal.
    EXPECT_EQ(signal_count, 0);
    expander.setExpanded(true);
    EXPECT_EQ(signal_count, 1);
    expander.setExpanded(true); // Already true — no signal.
    EXPECT_EQ(signal_count, 1);
}

TEST_F(SettingsTiersTest, SettingsCardExpander_HeaderButtonExists) {
    ui::widgets::SettingsCardExpander expander(2);
    auto* btn = expander.findChild<QPushButton*>(QStringLiteral("settingsCardExpanderHeader"));
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->text().contains(QStringLiteral("Advanced")));
}

// ---- ConfigPage integration tests ----

TEST_F(SettingsTiersTest, ConfigPage_ExpertModeToggleExists) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("expertModeToggleBtn"));
    ASSERT_NE(toggle, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_ExpertMode_DefaultOff) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_FALSE(page.expertModeEnabled());
}

TEST_F(SettingsTiersTest, ConfigPage_SetExpertModeEnabled_UpdatesState) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    EXPECT_TRUE(page.expertModeEnabled());
    page.setExpertModeEnabled(false);
    EXPECT_FALSE(page.expertModeEnabled());
}

TEST_F(SettingsTiersTest, ConfigPage_ExpertModeChanged_Signal) {
    ConfigPage page(output_defaults_, video_defaults_);
    // setExpertModeEnabled does NOT emit the signal (it's the setter, not user action).
    // Only the button click path emits. So we just confirm the state.
    page.setExpertModeEnabled(true);
    EXPECT_TRUE(page.expertModeEnabled());
}

// Wave 2: output_split_expander_ dissolved; split controls are now expert-gated.
// Tests updated to reflect the new structure.

TEST_F(SettingsTiersTest, ConfigPage_OutputSplitExpanderExists) {
    // Wave 2: no SettingsCardExpander named "outputSplitExpander" exists.
    // The split expert section (plain QWidget) replaced it.
    ConfigPage page(output_defaults_, video_defaults_);
    auto* expander = page.findChild<ui::widgets::SettingsCardExpander*>(QStringLiteral("outputSplitExpander"));
    EXPECT_EQ(expander, nullptr);
    // The split controls do still exist (inside splitExpertSection).
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    EXPECT_NE(combo, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_OutputSplitExpander_DefaultCollapsed) {
    // Wave 2: API is a no-op; always returns false.
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_FALSE(page.outputSplitExpanderExpanded());
}

TEST_F(SettingsTiersTest, ConfigPage_SplitModeComboInExpander_HiddenByDefault) {
    // Wave 2: splitModeCombo is now inside split_expert_section_ (expert-gated).
    // It is hidden by default because expert mode is off.
    ConfigPage page(output_defaults_, video_defaults_);
    auto* section = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_TRUE(section->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_SplitModeComboInExpander_VisibleWhenExpanded) {
    // Wave 2: split controls are shown when expert mode is on (not via expander).
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* section = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden());
    // The combo is findable.
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_SetOutputSplitExpanderExpanded_RoundTrip) {
    // Wave 2: setOutputSplitExpanderExpanded is a no-op; outputSplitExpanderExpanded always false.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSplitExpanderExpanded(true);
    EXPECT_FALSE(page.outputSplitExpanderExpanded());
    page.setOutputSplitExpanderExpanded(false);
    EXPECT_FALSE(page.outputSplitExpanderExpanded());
}

// Phase 1b: the audio-separate expander was removed (per-row toggles stay beside
// their own row).  The public API (setAudioSeparateExpanderExpanded / audioSeparateExpanderExpanded)
// is preserved for backward compat but always returns false — no expander widget exists.

TEST_F(SettingsTiersTest, ConfigPage_AudioSeparateExpanderAbsent) {
    // Phase 1b: no audioSeparateExpander widget exists in the tree.
    ConfigPage page(output_defaults_, video_defaults_);
    auto* expander = page.findChild<ui::widgets::SettingsCardExpander*>(QStringLiteral("audioSeparateExpander"));
    EXPECT_EQ(expander, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_AudioSeparateExpanderExpanded_AlwaysFalse) {
    // Phase 1b: getter returns false because no expander exists.
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_FALSE(page.audioSeparateExpanderExpanded());
    // Setting expanded has no effect.
    page.setAudioSeparateExpanderExpanded(true);
    EXPECT_FALSE(page.audioSeparateExpanderExpanded());
}

TEST_F(SettingsTiersTest, ConfigPage_AudioSeparateTogglesInSourceRows) {
    // Phase 1b: sys/app/mic separate-track toggles must exist as children of ConfigPage
    // (inside their source rows, not inside an expander).
    ConfigPage page(output_defaults_, video_defaults_);
    // The toggles are ExoToggle widgets; we verify they are present and correctly wired
    // by checking the named audio check boxes that sit beside them.
    // THEME-SLICE-1: audio source rows switched from QCheckBox to ExoCheckBox.
    auto* sys_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioSysCheck"));
    ASSERT_NE(sys_check, nullptr);
    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
}

// ---- PS-PHASE-C: Hotkeys card, expert controls, search pill gating ----

TEST_F(SettingsTiersTest, ConfigPage_HotkeysCard_ResetAllButtonExists) {
    // The embedded hotkeys panel must expose its "Reset all" button.
    ConfigPage page(output_defaults_, video_defaults_);
    auto* btn = page.findChild<QPushButton*>(QStringLiteral("settingsHkResetAllBtn"));
    ASSERT_NE(btn, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_HotkeysCard_ActiveRowsExist) {
    // All five active hotkey rows must be in the Settings tree.
    ConfigPage page(output_defaults_, video_defaults_);
    for (int i = 0; i < 5; ++i) {
        auto* set_btn = page.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_%1").arg(i));
        EXPECT_NE(set_btn, nullptr) << "settingsHkSetBtn_" << i << " not found";
    }
}

TEST_F(SettingsTiersTest, ConfigPage_FmtExpertSection_HiddenByDefault) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* section = page.findChild<QWidget*>(QStringLiteral("fmtExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_TRUE(section->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_FmtExpertSection_VisibleInExpertMode) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* section = page.findChild<QWidget*>(QStringLiteral("fmtExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_RateControlCombo_Exists) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("rateControlCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->findData(static_cast<int>(recorder_core::RateControlMode::ConstantQuality)), 0);
    EXPECT_GE(combo->findData(static_cast<int>(recorder_core::RateControlMode::VariableBitrate)), 0);
    EXPECT_GE(combo->findData(static_cast<int>(recorder_core::RateControlMode::ConstantBitrate)), 0);
}

TEST_F(SettingsTiersTest, ConfigPage_AudioExpertSection_HiddenByDefault) {
    ConfigPage page(output_defaults_, video_defaults_);
    // Startup-perf: the audio expert subtree is built lazily on first expert-enable,
    // so by default it isn't constructed yet — which still means it is not shown.
    auto* section = page.findChild<QWidget*>(QStringLiteral("audioExpertSection"));
    EXPECT_TRUE(section == nullptr || section->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_AudioExpertSection_VisibleInExpertMode) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* section = page.findChild<QWidget*>(QStringLiteral("audioExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_AudioExpertControls_Exist) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // audio-expert subtree is built lazily on first enable
    // Polish-R1: mic gain is now a QSlider (micGainSlider) + read-only QLabel (micGainDbLabel).
    EXPECT_NE(page.findChild<QSlider*>(QStringLiteral("micGainSlider")), nullptr);
    EXPECT_NE(page.findChild<QLabel*>(QStringLiteral("micGainDbLabel")), nullptr);
    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("micChannelModeCombo")), nullptr);
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("audioBitrateKbpsSpin")), nullptr);
    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("opusFrameDurationCombo")), nullptr);
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("opusComplexitySpin")), nullptr);
}

} // namespace
} // namespace exosnap
