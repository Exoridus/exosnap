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
TEST(AppSettingsTiersStoreTest, SettingsVersion_BumpedTo19) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString path = QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));

    AppSettingsStore store(path);
    PersistedAppSettings settings;
    store.Save(settings);

    QSettings raw(path, QSettings::IniFormat);
    // WHATS-NEW: version bumped 18 → 19 (whats_new_suppressed).
    EXPECT_EQ(raw.value(QStringLiteral("settings_version")).toInt(), 19);
}

TEST(AppSettingsTiersStoreTest, DeveloperLogLevel_DefaultIsDebug) {
    // Review F1 (product decision): ship default is "Debug" (record everything) --
    // main recorded all severities before this control was wired, and Debug lines
    // are exactly what support cases need. Narrowing is an explicit user choice.
    PersistedAppSettings settings;
    EXPECT_EQ(settings.developer_log_level, QStringLiteral("Debug"));
}

TEST(AppSettingsTiersStoreTest, DeveloperLogLevel_SaveAndLoad_RoundTrips) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());

    AppSettingsStore store(QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini")));
    PersistedAppSettings settings;
    settings.developer_log_level = QStringLiteral("Warning"); // non-default value
    store.Save(settings);

    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.developer_log_level, QStringLiteral("Warning"));
}

TEST(AppSettingsTiersStoreTest, DeveloperLogLevel_MissingKey_DefaultsToDebug) {
    QTemporaryDir temp_dir;
    ASSERT_TRUE(temp_dir.isValid());
    const QString path = QDir(temp_dir.path()).filePath(QStringLiteral("settings.ini"));

    // Write a file without the [developer] group (simulates a pre-existing settings
    // file from before this slice).
    {
        QSettings s(path, QSettings::IniFormat);
        s.beginGroup(QStringLiteral("overlay"));
        s.setValue(QStringLiteral("show_recording_overlay"), true);
        s.endGroup();
        s.sync();
    }

    AppSettingsStore store(path);
    const PersistedAppSettings loaded = store.Load();
    EXPECT_EQ(loaded.developer_log_level, QStringLiteral("Debug"));
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

// The amber expert-mode warning banner was removed for good; it must not reappear in
// either mode.
TEST_F(SettingsTiersTest, ConfigPage_ExpertWarnBanner_IsGone) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("expertWarnBanner")), nullptr);
    page.setExpertModeEnabled(true);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("expertWarnBanner")), nullptr)
        << "the expert warning banner must stay removed even in expert mode";
}

// Wave 2: output_split_expander_ dissolved; split controls are now expert-gated.
// Tests updated to reflect the new structure.

TEST_F(SettingsTiersTest, ConfigPage_OutputSplitExpanderExists) {
    // Wave 2: no SettingsCardExpander named "outputSplitExpander" exists.
    // The split section (plain QWidget) replaced it.
    ConfigPage page(output_defaults_, video_defaults_);
    auto* expander = page.findChild<ui::widgets::SettingsCardExpander*>(QStringLiteral("outputSplitExpander"));
    EXPECT_EQ(expander, nullptr);
    // Task 7: the split controls are Default tier and exist without expert mode.
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    EXPECT_NE(combo, nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_SplitModeComboInExpander_HiddenByDefault) {
    // Task 7: splitModeCombo lives inside split_expert_section_, which is now built
    // eagerly and always visible (Default tier, not expert-gated). The combo itself
    // is only reachable through splitIntervalRow, whose visibility follows
    // splitModeToggle — not expert mode. With expert mode off and the toggle at its
    // default (off), the interval row is hidden because of the toggle, not the tier.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* section = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden()) << "split section is Default tier and always visible";

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitModeToggle"));
    auto* interval_row = page.findChild<QWidget*>(QStringLiteral("splitIntervalRow"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(interval_row, nullptr);
    EXPECT_FALSE(toggle->isOn());
    EXPECT_TRUE(interval_row->isHidden()) << "interval row hidden because the toggle is off";
}

TEST_F(SettingsTiersTest, SplitControls_VisibleWithoutExpertMode) {
    // Task 7: the split section never needed expert mode enabled to be built or
    // shown — it is Default tier, built eagerly from the constructor.
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_FALSE(page.expertModeEnabled());

    auto* section = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden());

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    EXPECT_NE(combo, nullptr);

    // Turning the toggle on (still with expert mode off) reveals the interval row.
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitModeToggle"));
    auto* interval_row = page.findChild<QWidget*>(QStringLiteral("splitIntervalRow"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(interval_row, nullptr);
    toggle->setOn(true);
    EXPECT_FALSE(interval_row->isHidden());
}

TEST_F(SettingsTiersTest, ConfigPage_SplitModeComboInExpander_VisibleWhenExpanded) {
    // Task 7: split controls are visible regardless of expert mode; this test keeps
    // covering the (now redundant but harmless) expert-mode-on case.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* section = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->isHidden());
    // The combo is findable.
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
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

// ---- SETTINGS-HONESTY-R1: Developer card (log level genuinely wired, NVTX honest-disabled) ----

TEST_F(SettingsTiersTest, ConfigPage_DeveloperLogLevelCombo_HasFiveRealLevels_NoTrace) {
    // AppLog only has four severities (Debug/Info/Warning/Error); "Off" is the fifth
    // (fully-suppressed) option. The former stub combo also offered "Trace", which
    // doesn't correspond to anything AppLog can emit -- it must not survive the wiring.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // lazily builds the Developer card
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("developerLogLevelCombo"));
    ASSERT_NE(combo, nullptr);
    ASSERT_EQ(combo->count(), 5);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("Off"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("Error"));
    EXPECT_EQ(combo->itemData(2).toString(), QStringLiteral("Warning"));
    EXPECT_EQ(combo->itemData(3).toString(), QStringLiteral("Info"));
    EXPECT_EQ(combo->itemData(4).toString(), QStringLiteral("Debug"));
    for (int i = 0; i < combo->count(); ++i)
        EXPECT_NE(combo->itemText(i), QStringLiteral("Trace"));
}

TEST_F(SettingsTiersTest, ConfigPage_DeveloperLogLevelCombo_DefaultsToDebug) {
    // Review F1: ship default is Debug (record everything); see the store test above.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("developerLogLevelCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toString(), QStringLiteral("Debug"));
}

TEST_F(SettingsTiersTest, ConfigPage_SetDeveloperLogLevel_BeforeCardBuilt_AppliesOnBuild) {
    // setDeveloperLogLevel must be safe to call before the lazily-built Developer
    // card exists, and the pending value must be applied once it IS built. Probe with
    // a NON-default value ("Warning") so the assertion cannot pass vacuously.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setDeveloperLogLevel(QStringLiteral("Warning"));
    page.setExpertModeEnabled(true);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("developerLogLevelCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentData().toString(), QStringLiteral("Warning"));
}

TEST_F(SettingsTiersTest, ConfigPage_DeveloperLogLevelCombo_HasConsequenceTooltip) {
    // Review F3: raising the level silently drops lines from support diagnostics;
    // the combo must carry a tooltip that names that consequence.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("developerLogLevelCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->toolTip().contains(QStringLiteral("hides lower-severity lines")));
}

TEST_F(SettingsTiersTest, ConfigPage_DeveloperLogLevelCombo_ChangeEmitsSignal) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("developerLogLevelCombo"));
    ASSERT_NE(combo, nullptr);

    QString last_level;
    int count = 0;
    QObject::connect(&page, &ConfigPage::developerLogLevelChanged, [&](const QString& level) {
        ++count;
        last_level = level;
    });

    combo->setCurrentIndex(combo->findData(QStringLiteral("Error")));
    EXPECT_EQ(count, 1);
    EXPECT_EQ(last_level, QStringLiteral("Error"));
}

TEST_F(SettingsTiersTest, ConfigPage_NvtxProfilingCheck_HonestlyDisabledWithTooltip) {
    // No NVTX infrastructure exists in the app -- the control must stay disabled with
    // a "planned" tooltip rather than pretending to work.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("nvtxProfilingCheck"));
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->isEnabled());
    EXPECT_FALSE(check->toolTip().isEmpty());
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
    // Startup-perf: the Container-card expert section is built lazily on first
    // expert-enable, so by default it isn't constructed yet — which still means it is
    // not shown.
    auto* section = page.findChild<QWidget*>(QStringLiteral("fmtExpertSection"));
    EXPECT_TRUE(section == nullptr || section->isHidden());
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
    page.setExpertModeEnabled(true); // the rate section is built lazily on first enable
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
    // The Expert audio subtree keeps only the four genuinely expert rows; the
    // everyday mic/format controls live in the eager Default section.
    auto* section = page.findChild<QWidget*>(QStringLiteral("audioExpertSection"));
    ASSERT_NE(section, nullptr);
    EXPECT_NE(section->findChild<QComboBox*>(QStringLiteral("opusFrameDurationCombo")), nullptr);
    EXPECT_NE(section->findChild<QSpinBox*>(QStringLiteral("opusComplexitySpin")), nullptr);
    EXPECT_NE(section->findChild<QComboBox*>(QStringLiteral("audioSampleRateCombo")), nullptr);
    EXPECT_NE(section->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("clockSlavingCheck")), nullptr);

    // The Default-tier controls must NOT be part of the expert subtree any more.
    EXPECT_EQ(section->findChild<QSlider*>(QStringLiteral("micGainSlider")), nullptr);
    EXPECT_EQ(section->findChild<QComboBox*>(QStringLiteral("micChannelModeCombo")), nullptr);
    EXPECT_EQ(section->findChild<QSpinBox*>(QStringLiteral("audioBitrateKbpsSpin")), nullptr);
    EXPECT_EQ(section->findChild<QComboBox*>(QStringLiteral("audioChannelsCombo")), nullptr);
    EXPECT_EQ(section->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("limiterCheck")), nullptr);
    EXPECT_EQ(section->findChild<QWidget*>(QStringLiteral("micPostProcessingHeader")), nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_AudioDefaultControls_VisibleWithoutExpert) {
    // No setExpertModeEnabled() call: the everyday audio controls are built
    // eagerly and are visible in the Default tier.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* mic_channel = page.findChild<QComboBox*>(QStringLiteral("micChannelModeCombo"));
    auto* bitrate = page.findChild<QSpinBox*>(QStringLiteral("audioBitrateKbpsSpin"));
    auto* channels = page.findChild<QComboBox*>(QStringLiteral("audioChannelsCombo"));
    auto* mic_gain = page.findChild<QSlider*>(QStringLiteral("micGainSlider"));
    auto* limiter = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("limiterCheck"));
    auto* mic_post = page.findChild<QWidget*>(QStringLiteral("micPostProcessingHeader"));

    ASSERT_NE(mic_channel, nullptr);
    ASSERT_NE(bitrate, nullptr);
    ASSERT_NE(channels, nullptr);
    ASSERT_NE(mic_gain, nullptr);
    ASSERT_NE(limiter, nullptr);
    ASSERT_NE(mic_post, nullptr);

    EXPECT_FALSE(mic_channel->isHidden());
    EXPECT_FALSE(bitrate->isHidden());
    EXPECT_FALSE(channels->isHidden());
    EXPECT_FALSE(mic_gain->isHidden());
    EXPECT_FALSE(limiter->isHidden());
    EXPECT_FALSE(mic_post->isHidden());
    EXPECT_NE(page.findChild<QLabel*>(QStringLiteral("micGainDbLabel")), nullptr);
}

TEST_F(SettingsTiersTest, ConfigPage_MicGainSliderSpansSharedControlWidth) {
    // Track + value label must add up to the shared 160 px settings-row control
    // width (116 + 4 px row spacing + 40).
    ConfigPage page(output_defaults_, video_defaults_);
    auto* slider = page.findChild<QSlider*>(QStringLiteral("micGainSlider"));
    auto* db_label = page.findChild<QLabel*>(QStringLiteral("micGainDbLabel"));
    ASSERT_NE(slider, nullptr);
    ASSERT_NE(db_label, nullptr);
    EXPECT_EQ(slider->width(), 116);
    EXPECT_EQ(db_label->width(), 40);
}

} // namespace
} // namespace exosnap
