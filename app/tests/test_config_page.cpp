#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "models/WebcamSettings.h"
#include "pages/ConfigPage.h"
#include "ui/widgets/CameraPreview.h"
#include "ui/widgets/ExoCheckBox.h"
#include "ui/widgets/ExoToggle.h"
#include "ui/widgets/SettingsPopoverRow.h"
#include "ui/widgets/VUMeterWidget.h"
#include "ui/widgets/WebcamSetupPanel.h"

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "config_page_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class ConfigPageTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static bool HasLabelText(const ConfigPage& page, const QString& text) {
        const auto labels = page.findChildren<QLabel*>();
        for (const auto* label : labels) {
            if (label->text() == text)
                return true;
        }
        return false;
    }

    // Check ExoCheckBox (or QCheckBox) text anywhere in the widget tree.
    // THEME-SLICE-1: audio source rows are now ExoCheckBox, so search both.
    static bool HasCheckText(const ConfigPage& page, const QString& text) {
        for (const auto* cb : page.findChildren<ui::widgets::ExoCheckBox*>()) {
            if (cb->text() == text)
                return true;
        }
        for (const auto* cb : page.findChildren<QCheckBox*>()) {
            if (cb->text() == text)
                return true;
        }
        return false;
    }

    // Check ExoCheckBox text anywhere in the widget tree.
    static bool HasExoCheckText(const ConfigPage& page, const QString& text) {
        for (const auto* cb : page.findChildren<ui::widgets::ExoCheckBox*>()) {
            if (cb->text() == text)
                return true;
        }
        return false;
    }

    // Returns the first ExoCheckBox with matching text, or nullptr.
    static ui::widgets::ExoCheckBox* FindExoCheck(const ConfigPage& page, const QString& text) {
        for (auto* cb : page.findChildren<ui::widgets::ExoCheckBox*>()) {
            if (cb->text() == text)
                return cb;
        }
        return nullptr;
    }

    // Returns true if any visible QLabel (isVisible() == true) has the given text.
    static bool HasVisibleLabelText(const ConfigPage& page, const QString& text) {
        for (const auto* l : page.findChildren<QLabel*>()) {
            if (l->text() == text && l->isVisible())
                return true;
        }
        return false;
    }

    // Returns true when the settingsAudioAppSection container is not explicitly hidden.
    static bool AppSectionVisible(const ConfigPage& page) {
        const auto* section = page.findChild<QWidget*>(QStringLiteral("settingsAudioAppSection"));
        return (section != nullptr) && !section->isHidden();
    }

    OutputSettingsModel output_defaults_;
    VideoSettingsModel video_defaults_;
};

TEST_F(ConfigPageTest, Constructs_AllKeyControlsExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_NE(page.findChild<QComboBox*>("", Qt::FindChildrenRecursively), nullptr);
}

TEST_F(ConfigPageTest, ProfileComboExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* combo = page.findChild<QComboBox*>();
    EXPECT_NE(combo, nullptr);
}

TEST_F(ConfigPageTest, ContainerSegmentedControlExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* segmented = page.findChild<QWidget*>(QStringLiteral("containerSegmented"));
    ASSERT_NE(segmented, nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("containerMkvButton")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("containerWebmButton")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("containerMp4Button")), nullptr);
}

TEST_F(ConfigPageTest, VideoQualityComboExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto combos = page.findChildren<QComboBox*>();
    EXPECT_GE(combos.size(), 2);
}

TEST_F(ConfigPageTest, QualitySegmentsExist_WithStableObjectNames) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualitySegmentBalanced")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh")), nullptr);
}

TEST_F(ConfigPageTest, LegacyQualityCards_AreRemoved) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The 2x2 card selector (and its disabled Custom placeholder) are gone.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("qualityCardBalanced")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("qualityCardSmall")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("qualityCardCustom")), nullptr);
    EXPECT_EQ(page.findChild<QWidget*>(QStringLiteral("qualityCardsGrid")), nullptr);
}

TEST_F(ConfigPageTest, AudioSourceCheckboxesExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    // 3 source-enable ExoCheckBoxes (sys / app / mic).
    // THEME-SLICE-1: audio source rows switched from QCheckBox to ExoCheckBox.
    // The "Separate track" controls are ExoToggle pill-toggles (DF-12 — QCheckBox replaced).
    const auto checks = page.findChildren<ui::widgets::ExoCheckBox*>();
    EXPECT_GE(checks.size(), 3);
}

TEST_F(ConfigPageTest, WebcamControlsExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto combos = page.findChildren<QComboBox*>();
    EXPECT_GE(combos.size(), 2);
}

TEST_F(ConfigPageTest, OutputFolderEditExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto edits = page.findChildren<QLineEdit*>();
    EXPECT_GE(edits.size(), 2);
}

TEST_F(ConfigPageTest, ReadinessDefaults_HidesDiagnosticsAction) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Default state: "CHECKING" is set at construction time.
    // The Open Diagnostics button exists but should be hidden.
    const auto buttons = page.findChildren<QPushButton*>();
    for (const auto* b : buttons) {
        if (b->text() == QStringLiteral("Open Diagnostics...")) {
            EXPECT_FALSE(b->isVisible());
            return;
        }
    }
    FAIL() << "Open Diagnostics... button not found";
}

TEST_F(ConfigPageTest, PresetManagementButtonExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto buttons = page.findChildren<QToolButton*>();
    bool found = false;
    for (const auto* b : buttons) {
        if (b->text() == QStringLiteral("Manage presets")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Preset management overflow button not found";
}

TEST_F(ConfigPageTest, HybridCardTitles_AreVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The hybrid Settings IA splits the old combined card into discrete compact cards.
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Preset")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Format & encoding")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Audio")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Webcam")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Output")));

    // The old combined "Preset & Format" card title is gone after the hybrid IA split.
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Preset & Format")));
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Profile & Format")));
}

TEST_F(ConfigPageTest, OutputResolution_IsFunctional) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* segmented = page.findChild<QWidget*>(QStringLiteral("outputResSegmented"));
    ASSERT_NE(segmented, nullptr);

    const auto segments = segmented->findChildren<QPushButton*>();
    ASSERT_GE(segments.size(), 5);
    for (const auto* seg : segments)
        EXPECT_TRUE(seg->isEnabled()) << "Output resolution segment must be interactive: " << seg->text().toStdString();

    OutputSettingsModel changed;
    bool emitted = false;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, [&](const OutputSettingsModel& settings) {
        emitted = true;
        changed = settings;
    });

    auto* res1080 = page.findChild<QPushButton*>(QStringLiteral("outputRes1080Button"));
    ASSERT_NE(res1080, nullptr);
    res1080->click();
    EXPECT_TRUE(emitted);
    EXPECT_EQ(changed.resolution.mode, OutputResolutionMode::FHD1080);
}

TEST_F(ConfigPageTest, FrameRateControl_UsesRealValues) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* frame_rate = page.findChild<QComboBox*>(QStringLiteral("frameRateCombo"));
    ASSERT_NE(frame_rate, nullptr);
    EXPECT_TRUE(frame_rate->isEnabled());
    EXPECT_GE(frame_rate->count(), 6);

    VideoSettingsModel changed;
    bool emitted = false;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged, [&](const VideoSettingsModel& settings) {
        emitted = true;
        changed = settings;
    });

    const int idx30 = frame_rate->findData(30);
    ASSERT_GE(idx30, 0);
    frame_rate->setCurrentIndex(idx30);
    EXPECT_TRUE(emitted);
    EXPECT_EQ(changed.frame_rate_num, 30u);
    EXPECT_EQ(changed.frame_rate_den, 1u);
}

TEST_F(ConfigPageTest, TimingSegments_MapToVideoSettingsAndMp4DisablesVfr) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* vfr = page.findChild<QPushButton*>(QStringLiteral("timingVfrButton"));
    auto* cfr = page.findChild<QPushButton*>(QStringLiteral("timingCfrButton"));
    ASSERT_NE(vfr, nullptr);
    ASSERT_NE(cfr, nullptr);

    VideoSettingsModel changed;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged,
                     [&](const VideoSettingsModel& settings) { changed = settings; });

    vfr->click();
    EXPECT_FALSE(changed.cfr);
    EXPECT_TRUE(vfr->isChecked());

    auto* mp4 = page.findChild<QPushButton*>(QStringLiteral("containerMp4Button"));
    ASSERT_NE(mp4, nullptr);
    mp4->click();
    EXPECT_TRUE(changed.cfr);
    EXPECT_TRUE(cfr->isChecked());
    EXPECT_FALSE(vfr->isEnabled());
}

TEST_F(ConfigPageTest, OutputEffectiveSummaryReflectsFormatControls) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* summary = page.findChild<QLabel*>(QStringLiteral("outputEffectiveSummaryLabel"));
    ASSERT_NE(summary, nullptr);

    auto* res720 = page.findChild<QPushButton*>(QStringLiteral("outputRes720Button"));
    auto* frame_rate = page.findChild<QComboBox*>(QStringLiteral("frameRateCombo"));
    ASSERT_NE(res720, nullptr);
    ASSERT_NE(frame_rate, nullptr);

    res720->click();
    const int idx24 = frame_rate->findData(24);
    ASSERT_GE(idx24, 0);
    frame_rate->setCurrentIndex(idx24);

    EXPECT_TRUE(summary->text().contains(QStringLiteral("720p")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("24 fps")));
}

TEST_F(ConfigPageTest, FilenameTokenChips_AreShown) {
    ConfigPage page(output_defaults_, video_defaults_);

    int chip_count = 0;
    const auto labels = page.findChildren<QLabel*>();
    for (const auto* label : labels) {
        if (label->property("labelRole").toString() == QStringLiteral("tokenChip"))
            ++chip_count;
    }
    EXPECT_GE(chip_count, 4) << "Output card should expose compact filename token chips";
}

TEST_F(ConfigPageTest, BuiltInAndModifiedStates_UsePresetCopy) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption builtin;
    builtin.id = QStringLiteral("builtin");
    builtin.label = QStringLiteral("High Quality AV1");
    builtin.built_in = true;
    builtin.modified = false;

    std::vector<ConfigPage::ProfileOption> options{builtin};
    // Clean state: built-in badge visible, dirty indicator hidden.
    page.setPresetOptions(options, builtin.id, QString(), /*dirty=*/false);
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Built-in preset")));
    auto* dirty_indicator = page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator"));
    ASSERT_NE(dirty_indicator, nullptr);
    EXPECT_TRUE(dirty_indicator->isHidden()) << "Dirty indicator must be hidden when clean";

    // Dirty state: dirty indicator visible; built-in badge still shows.
    options[0].modified = true;
    page.setPresetOptions(options, builtin.id, QString(), /*dirty=*/true);
    EXPECT_FALSE(dirty_indicator->isHidden()) << "Dirty indicator must be visible when dirty";
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Built-in preset")));
}

// ── HYBRID-SETTINGS-WEBCAM-R1: inline WebcamSetupPanel replaces stub + nav ──

TEST_F(ConfigPageTest, WebcamSetupPanel_IsEmbeddedInSettings) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr) << "Settings must contain an inline WebcamSetupPanel";
}

TEST_F(ConfigPageTest, WebcamSetupPanel_ContainsCameraPreview) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);

    auto* preview = panel->findChild<ui::widgets::CameraPreview*>();
    EXPECT_NE(preview, nullptr) << "WebcamSetupPanel must contain a CameraPreview widget";
}

TEST_F(ConfigPageTest, WebcamSetupPanel_ContainsDeviceAndResolutionCombos) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);

    EXPECT_NE(panel->findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo")), nullptr);
    EXPECT_NE(panel->findChild<QComboBox*>(QStringLiteral("webcamPanelResolutionCombo")), nullptr);
}

TEST_F(ConfigPageTest, WebcamSetupPanel_ContainsEnableToggle) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);

    EXPECT_NE(panel->findChild<QWidget*>(QStringLiteral("webcamPanelEnableToggle")), nullptr);
}

TEST_F(ConfigPageTest, WebcamSetupPanel_HasCompactRescanNotLargeOpenSetup) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The old "Open Webcam Setup" button must be gone from the primary settings flow.
    for (const auto* btn : page.findChildren<QPushButton*>())
        EXPECT_NE(btn->text(), QStringLiteral("Open Webcam Setup"))
            << "Settings must not require 'Open Webcam Setup' for standard configuration";

    // The compact rescan button lives inside the panel.
    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>(QStringLiteral("webcamPanelRescanBtn")), nullptr);
}

TEST_F(ConfigPageTest, WebcamSetupPanel_ApplySettingsUpdatesEnabledState) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);

    WebcamSettings s;
    s.enabled = true;
    page.setWebcamSettings(s);

    auto* toggle = panel->findChild<QWidget*>(QStringLiteral("webcamPanelEnableToggle"));
    ASSERT_NE(toggle, nullptr);
    // ExoToggle inherits QAbstractButton — isChecked() reflects enabled state.
    auto* btn = qobject_cast<QAbstractButton*>(toggle);
    ASSERT_NE(btn, nullptr);
    EXPECT_TRUE(btn->isChecked());
}

TEST_F(ConfigPageTest, WebcamSetupPanel_SettingsChangedSignalPropagates) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::webcamSettingsChanged, [&emitted](const WebcamSettings&) { emitted = true; });

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);
    // Emit from the panel and verify ConfigPage re-emits.
    emit panel->settingsChanged(WebcamSettings{});
    EXPECT_TRUE(emitted);
}

TEST_F(ConfigPageTest, ReadinessBlocked_ShowsDiagnosticsAction) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setReadinessStatus(QStringLiteral("BLOCKED"));

    bool has_diag_btn = false;
    const auto buttons = page.findChildren<QPushButton*>();
    for (const auto* b : buttons) {
        if (b->text() == QStringLiteral("Open Diagnostics..."))
            has_diag_btn = true;
    }
    EXPECT_TRUE(has_diag_btn);
}

TEST_F(ConfigPageTest, ReadinessReady_ShowsReadyText) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setReadinessStatus(QStringLiteral("READY"));

    bool has_ready = false;
    const auto labels = page.findChildren<QLabel*>();
    for (const auto* l : labels) {
        if (l->text() == QStringLiteral("Ready to record"))
            has_ready = true;
    }
    EXPECT_TRUE(has_ready);
}

TEST_F(ConfigPageTest, ProfileOptions_PopulateCombo) {
    ConfigPage page(output_defaults_, video_defaults_);

    std::vector<ConfigPage::ProfileOption> opts;
    ConfigPage::ProfileOption po;
    po.id = QStringLiteral("test");
    po.label = QStringLiteral("Test Profile");
    opts.push_back(po);

    page.setPresetOptions(opts, QStringLiteral("test"), QString(), false);

    const auto combos = page.findChildren<QComboBox*>();
    bool found = false;
    for (const auto* c : combos) {
        for (int i = 0; i < c->count(); ++i) {
            if (c->itemData(i).toString() == QStringLiteral("test"))
                found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(ConfigPageTest, VideoQualityChange_EmitsVideoSettingsChanged) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged,
                     [&emitted](const VideoSettingsModel&) { emitted = true; });

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("videoQualityCombo"));
    ASSERT_NE(combo, nullptr);

    combo->setCurrentIndex(0); // High — same as default, won't emit
    EXPECT_FALSE(emitted);

    combo->setCurrentIndex(2); // Small
    EXPECT_TRUE(emitted);
}

TEST_F(ConfigPageTest, QualitySegmentClick_EachSegmentUpdatesModel) {
    ConfigPage page(output_defaults_, video_defaults_);

    VideoSettingsModel changed;
    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged, [&](const VideoSettingsModel& settings) {
        ++emit_count;
        changed = settings;
    });

    auto* small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    auto* balanced_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentBalanced"));
    auto* high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    ASSERT_NE(small_segment, nullptr);
    ASSERT_NE(balanced_segment, nullptr);
    ASSERT_NE(high_segment, nullptr);

    // Default quality is High, so each click below is a real change and emits.
    small_segment->click();
    EXPECT_EQ(changed.quality, recorder_core::NvencQualityPreset::Small);
    EXPECT_TRUE(small_segment->isChecked());
    EXPECT_TRUE(small_segment->property("qualitySegmentSelected").toBool());
    EXPECT_FALSE(high_segment->isChecked());

    balanced_segment->click();
    EXPECT_EQ(changed.quality, recorder_core::NvencQualityPreset::Balanced);
    EXPECT_TRUE(balanced_segment->isChecked());
    EXPECT_FALSE(small_segment->isChecked());

    high_segment->click();
    EXPECT_EQ(changed.quality, recorder_core::NvencQualityPreset::High);
    EXPECT_TRUE(high_segment->isChecked());
    EXPECT_FALSE(balanced_segment->isChecked());

    EXPECT_EQ(emit_count, 3);
}

TEST_F(ConfigPageTest, SetVideoSettings_UpdatesQualitySegmentSelection) {
    ConfigPage page(output_defaults_, video_defaults_);

    VideoSettingsModel balanced = video_defaults_;
    balanced.quality = recorder_core::NvencQualityPreset::Balanced;
    page.setVideoSettings(balanced);

    auto* small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    auto* balanced_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentBalanced"));
    auto* high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    ASSERT_NE(small_segment, nullptr);
    ASSERT_NE(balanced_segment, nullptr);
    ASSERT_NE(high_segment, nullptr);

    EXPECT_FALSE(high_segment->isChecked());
    EXPECT_TRUE(balanced_segment->isChecked());
    EXPECT_FALSE(small_segment->isChecked());
    EXPECT_FALSE(high_segment->property("qualitySegmentSelected").toBool());
    EXPECT_TRUE(balanced_segment->property("qualitySegmentSelected").toBool());
    EXPECT_FALSE(small_segment->property("qualitySegmentSelected").toBool());
}

TEST_F(ConfigPageTest, SetRecordingControlsLocked_DisablesKeyControls) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setRecordingControlsLocked(true);

    auto* profile_combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(profile_combo, nullptr);
    EXPECT_FALSE(profile_combo->isEnabled());

    auto* quality_combo = page.findChild<QComboBox*>(QStringLiteral("videoQualityCombo"));
    ASSERT_NE(quality_combo, nullptr);
    EXPECT_FALSE(quality_combo->isEnabled());
    auto* quality_high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    ASSERT_NE(quality_high_segment, nullptr);
    EXPECT_FALSE(quality_high_segment->isEnabled());
    auto* quality_small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    ASSERT_NE(quality_small_segment, nullptr);
    EXPECT_FALSE(quality_small_segment->isEnabled());
    auto* frame_rate = page.findChild<QComboBox*>(QStringLiteral("frameRateCombo"));
    ASSERT_NE(frame_rate, nullptr);
    EXPECT_FALSE(frame_rate->isEnabled());
    auto* timing_cfr = page.findChild<QPushButton*>(QStringLiteral("timingCfrButton"));
    ASSERT_NE(timing_cfr, nullptr);
    EXPECT_FALSE(timing_cfr->isEnabled());
    auto* output_res = page.findChild<QPushButton*>(QStringLiteral("outputRes1080Button"));
    ASSERT_NE(output_res, nullptr);
    EXPECT_FALSE(output_res->isEnabled());

    auto* dest_edit = page.findChild<QLineEdit*>(QStringLiteral("destinationEdit"));
    ASSERT_NE(dest_edit, nullptr);
    EXPECT_FALSE(dest_edit->isEnabled());

    auto* naming_edit = page.findChild<QLineEdit*>(QStringLiteral("namingEdit"));
    ASSERT_NE(naming_edit, nullptr);
    EXPECT_FALSE(naming_edit->isEnabled());

    auto* mic_combo = page.findChild<QComboBox*>(QStringLiteral("micDeviceCombo"));
    ASSERT_NE(mic_combo, nullptr);
    EXPECT_FALSE(mic_combo->isEnabled());

    // Webcam restart-class controls lock; live enable/mirror remain editable.
    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);
    auto* webcam_toggle = panel->findChild<QWidget*>(QStringLiteral("webcamPanelEnableToggle"));
    ASSERT_NE(webcam_toggle, nullptr);
    EXPECT_TRUE(webcam_toggle->isEnabled());
    auto* webcam_mirror = panel->findChild<QWidget*>(QStringLiteral("webcamPanelMirrorToggle"));
    ASSERT_NE(webcam_mirror, nullptr);
    EXPECT_TRUE(webcam_mirror->isEnabled());
    auto* webcam_device = panel->findChild<QComboBox*>(QStringLiteral("webcamPanelDeviceCombo"));
    ASSERT_NE(webcam_device, nullptr);
    EXPECT_FALSE(webcam_device->isEnabled());
    auto* webcam_resolution = panel->findChild<QComboBox*>(QStringLiteral("webcamPanelResolutionCombo"));
    ASSERT_NE(webcam_resolution, nullptr);
    EXPECT_FALSE(webcam_resolution->isEnabled());
    auto* webcam_rescan = panel->findChild<QPushButton*>(QStringLiteral("webcamPanelRescanBtn"));
    ASSERT_NE(webcam_rescan, nullptr);
    EXPECT_FALSE(webcam_rescan->isEnabled());

    auto* lock_note = page.findChild<QLabel*>(QStringLiteral("lockNoteLabel"));
    ASSERT_NE(lock_note, nullptr);
    EXPECT_FALSE(lock_note->isHidden());
}

TEST_F(ConfigPageTest, DefaultControlsAreEnabled) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* quality_combo = page.findChild<QComboBox*>(QStringLiteral("videoQualityCombo"));
    ASSERT_NE(quality_combo, nullptr);
    EXPECT_TRUE(quality_combo->isEnabled());
    auto* quality_high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    ASSERT_NE(quality_high_segment, nullptr);
    EXPECT_TRUE(quality_high_segment->isEnabled());
    auto* quality_small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    ASSERT_NE(quality_small_segment, nullptr);
    EXPECT_TRUE(quality_small_segment->isEnabled());

    // Mic device combo: disabled by default because no audio plan has been set yet.
    // (In production, setAudioUiState is called immediately after construction, so
    // the default state is transient and never visible to the user.)
    auto* mic_combo = page.findChild<QComboBox*>(QStringLiteral("micDeviceCombo"));
    ASSERT_NE(mic_combo, nullptr);
    EXPECT_FALSE(mic_combo->isEnabled());

    auto* lock_note = page.findChild<QLabel*>(QStringLiteral("lockNoteLabel"));
    ASSERT_NE(lock_note, nullptr);
    EXPECT_TRUE(lock_note->isHidden());
}

TEST_F(ConfigPageTest, WebcamInfoLabel_DisabledState_ToggleReflectsState) {
    // The old info-label text is replaced by the WebcamSetupPanel's enable toggle.
    ConfigPage page(output_defaults_, video_defaults_);

    WebcamSettings ws;
    ws.enabled = false;
    page.setWebcamSettings(ws);

    auto* panel = page.findChild<ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    ASSERT_NE(panel, nullptr);
    auto* toggle =
        qobject_cast<QAbstractButton*>(panel->findChild<QWidget*>(QStringLiteral("webcamPanelEnableToggle")));
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isChecked()) << "Disabled state must reflect in the enable toggle";
}

TEST_F(ConfigPageTest, WebcamInfoLabel_EnabledNoDevice_DoesNotShowStaleMessage) {
    ConfigPage page(output_defaults_, video_defaults_);

    WebcamSettings ws;
    ws.enabled = true;
    ws.device_id = "";
    page.setWebcamSettings(ws);

    const auto labels = page.findChildren<QLabel*>();
    for (const auto* l : labels) {
        EXPECT_FALSE(l->text().contains(QStringLiteral("Configure on Webcam Details page")))
            << "Stale copy found: " << l->text().toStdString();
    }
}

TEST_F(ConfigPageTest, MicSourceLabel_DoesNotSaySelectDeviceOnRecordPage) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto labels = page.findChildren<QLabel*>();
    for (const auto* l : labels) {
        EXPECT_FALSE(l->text().contains(QStringLiteral("Select device on Record page")))
            << "Stale mic copy found: " << l->text().toStdString();
    }
}

TEST_F(ConfigPageTest, TokenHelpToggle_HiddenByDefault) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* toggle = page.findChild<QPushButton*>(QStringLiteral("tokenHelpToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->text(), QStringLiteral("Show token reference"));
}

TEST_F(ConfigPageTest, QualitySegment_HasSimpleLabels) {
    // Caption labels removed; segment labels are now "Small"/"Balanced"/"High" without CQ numbers.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    auto* balanced_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentBalanced"));
    auto* high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    ASSERT_NE(small_segment, nullptr);
    ASSERT_NE(balanced_segment, nullptr);
    ASSERT_NE(high_segment, nullptr);
    EXPECT_EQ(small_segment->text(), QStringLiteral("Small"));
    EXPECT_EQ(balanced_segment->text(), QStringLiteral("Balanced"));
    EXPECT_EQ(high_segment->text(), QStringLiteral("High"));
    // Caption labels are gone.
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("qualityBadgeLabel")), nullptr);
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("qualitySettingsLabel")), nullptr);
}

// ── SETTINGS-AUDIO-METER-R1: live mono meters in the Settings Audio card ─────

TEST_F(ConfigPageTest, SettingsAudio_ExposesSysMeterWidget) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    ASSERT_NE(meter, nullptr) << "Settings Audio card must contain a system mono meter";
}

TEST_F(ConfigPageTest, SettingsAudio_ExposesAppMeterWidget) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    ASSERT_NE(meter, nullptr) << "Settings Audio card must contain an app mono meter";
}

TEST_F(ConfigPageTest, SettingsAudio_ExposesMicMeterWidget) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioMicMeter"));
    ASSERT_NE(meter, nullptr) << "Settings Audio card must contain a mic mono meter";
}

TEST_F(ConfigPageTest, SettingsAudio_MetersInactiveByDefault) {
    ConfigPage page(output_defaults_, video_defaults_);
    auto* sys = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    auto* app = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    auto* mic = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioMicMeter"));
    ASSERT_NE(sys, nullptr);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(mic, nullptr);
    EXPECT_FALSE(sys->isActive()) << "System meter must be inactive before any level update";
    EXPECT_FALSE(app->isActive()) << "App meter must be inactive before any level update";
    EXPECT_FALSE(mic->isActive()) << "Mic meter must be inactive before any level update";
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_SysActiveDoesNotModifyAppOrMic) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioMeterLevels(0.5f, 0.0f, 0.0f, /*sys_active=*/true, /*app_active=*/false,
                             /*mic_active=*/false);

    auto* sys = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    auto* app = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    auto* mic = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioMicMeter"));
    ASSERT_NE(sys, nullptr);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(mic, nullptr);
    EXPECT_TRUE(sys->isActive());
    EXPECT_FLOAT_EQ(sys->level(), 0.5f);
    EXPECT_FALSE(app->isActive());
    EXPECT_FLOAT_EQ(app->level(), 0.0f);
    EXPECT_FALSE(mic->isActive());
    EXPECT_FLOAT_EQ(mic->level(), 0.0f);
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_AppActiveDoesNotModifySystemOrMic) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioMeterLevels(0.0f, 0.7f, 0.0f, /*sys_active=*/false, /*app_active=*/true,
                             /*mic_active=*/false);

    auto* sys = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    auto* app = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    auto* mic = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioMicMeter"));
    ASSERT_NE(sys, nullptr);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(mic, nullptr);
    EXPECT_FALSE(sys->isActive());
    EXPECT_FLOAT_EQ(sys->level(), 0.0f);
    EXPECT_TRUE(app->isActive());
    EXPECT_FLOAT_EQ(app->level(), 0.7f);
    EXPECT_FALSE(mic->isActive());
    EXPECT_FLOAT_EQ(mic->level(), 0.0f);
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_MicActiveDoesNotModifySystemOrApp) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioMeterLevels(0.0f, 0.0f, 0.4f, /*sys_active=*/false, /*app_active=*/false,
                             /*mic_active=*/true);

    auto* sys = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    auto* app = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    auto* mic = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioMicMeter"));
    ASSERT_NE(sys, nullptr);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(mic, nullptr);
    EXPECT_FALSE(sys->isActive());
    EXPECT_FLOAT_EQ(sys->level(), 0.0f);
    EXPECT_FALSE(app->isActive());
    EXPECT_FLOAT_EQ(app->level(), 0.0f);
    EXPECT_TRUE(mic->isActive());
    EXPECT_FLOAT_EQ(mic->level(), 0.4f);
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_InactiveSourceHasZeroLevel) {
    ConfigPage page(output_defaults_, video_defaults_);

    // First activate all three meters.
    page.setAudioMeterLevels(0.6f, 0.5f, 0.4f, true, true, true);

    // Now deactivate system.
    page.setAudioMeterLevels(0.0f, 0.5f, 0.4f, /*sys_active=*/false, true, true);

    auto* sys = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    ASSERT_NE(sys, nullptr);
    EXPECT_FALSE(sys->isActive());
    EXPECT_FLOAT_EQ(sys->level(), 0.0f);
}

TEST_F(ConfigPageTest, SettingsAudio_NoWebcamMeter) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The webcam source must not have an audio meter in the Settings Audio card.
    const auto meters = page.findChildren<ui::widgets::VUMeterWidget*>();
    // Only sys/app/mic meters exist — exactly 3.
    EXPECT_EQ(meters.size(), 3) << "Expected exactly 3 audio meters (sys/app/mic); webcam must not have one";
}

TEST_F(ConfigPageTest, SettingsAudio_NoLRChannelTerminology) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The Settings Audio card must not use L/R channel labels.
    const auto labels = page.findChildren<QLabel*>();
    for (const auto* label : labels) {
        const QString text = label->text();
        EXPECT_FALSE(text == QStringLiteral("L")) << "Found standalone 'L' channel label in Settings";
        EXPECT_FALSE(text == QStringLiteral("R")) << "Found standalone 'R' channel label in Settings";
        EXPECT_FALSE(text.contains(QStringLiteral("Left channel"))) << "Found 'Left channel' label in Settings";
        EXPECT_FALSE(text.contains(QStringLiteral("Right channel"))) << "Found 'Right channel' label in Settings";
    }
}

TEST_F(ConfigPageTest, SettingsAudio_ExistingSignalEmissionsUnchanged) {
    ConfigPage page(output_defaults_, video_defaults_);

    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::audioSettingsChanged,
                     [&emit_count](const capability::AudioUiState&) { ++emit_count; });

    // Find the system audio checkbox by object name (text varies by target kind).
    // THEME-SLICE-1: audio source rows are ExoCheckBox (not QCheckBox).
    auto* sys_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioSysCheck"));
    ASSERT_NE(sys_check, nullptr) << "settingsAudioSysCheck not found in Settings Audio card";

    // Toggling must still emit audioSettingsChanged.
    const bool was_checked = sys_check->isChecked();
    sys_check->setChecked(!was_checked);
    EXPECT_GE(emit_count, 1);
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_DbLabelUpdatesCorrectly) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Active at ~0.5 level01 → db = 0.5 * 60 - 60 = -30 → "−30 dB"
    page.setAudioMeterLevels(0.5f, 0.0f, 0.0f, true, false, false);

    auto* db_label = page.findChild<QLabel*>(QStringLiteral("settingsAudioSysDbLabel"));
    ASSERT_NE(db_label, nullptr);
    EXPECT_TRUE(db_label->text().contains(QStringLiteral("dB"))) << "dB label should show dBFS value when active";
}

TEST_F(ConfigPageTest, SetAudioMeterLevels_InactiveDbLabelShowsDash) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioMeterLevels(0.0f, 0.0f, 0.0f, false, false, false);

    auto* db_label = page.findChild<QLabel*>(QStringLiteral("settingsAudioSysDbLabel"));
    ASSERT_NE(db_label, nullptr);
    EXPECT_EQ(db_label->text(), QStringLiteral("–")) << "dB label should show dash when inactive";
}

// ── APP-AUDIO-ROW-FIX-R1: ConfigPage/ViewModel integration ───────────────────

TEST_F(ConfigPageTest, SetAudioUiState_WindowWithAppRow_EnablesAppCheckbox) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Window;
    state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
        {recorder_core::AudioSourceKind::Sys, true, false},
    };
    page.setAudioUiState(state);

    EXPECT_TRUE(AppSectionVisible(page)) << "App section must be visible for Window target";

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr) << "settingsAudioAppCheck not found";
    EXPECT_TRUE(app_check->isEnabled()) << "App checkbox must be enabled when App row is present";
    EXPECT_TRUE(app_check->isChecked());
}

TEST_F(ConfigPageTest, SetAudioUiState_DisplayMode_HidesAppSection) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Display;
    state.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
    };
    page.setAudioUiState(state);

    EXPECT_FALSE(AppSectionVisible(page)) << "App section must be hidden for Display target";
}

TEST_F(ConfigPageTest, SetAudioUiState_DisplayMode_SysLabelIsComputerAudio) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState display_state;
    display_state.target_kind = capability::CaptureTargetKind::Display;
    display_state.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
    };
    page.setAudioUiState(display_state);

    auto* sys_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioSysCheck"));
    ASSERT_NE(sys_check, nullptr);
    EXPECT_EQ(sys_check->text(), QStringLiteral("Computer audio"));
    EXPECT_FALSE(AppSectionVisible(page)) << "App section must be hidden for Display target";
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Not available for current capture target")));
}

TEST_F(ConfigPageTest, SetAudioUiState_WindowMode_SysLabelIsOtherSystemAudio) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState window_state;
    window_state.target_kind = capability::CaptureTargetKind::Window;
    window_state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
        {recorder_core::AudioSourceKind::Sys, false, false},
    };
    page.setAudioUiState(window_state);

    auto* sys_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioSysCheck"));
    ASSERT_NE(sys_check, nullptr);
    EXPECT_EQ(sys_check->text(), QStringLiteral("Other system audio"));
    EXPECT_TRUE(AppSectionVisible(page)) << "App section must not be hidden for Window target";
}

// ── AUDIO-SOURCE-POLICY-R1: context-aware Settings Audio card ─────────────────

TEST_F(ConfigPageTest, AudioPolicy_DisplayMode_ShowsComputerAudioPlusmic) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Display;
    state.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
    };
    page.setAudioUiState(state);

    EXPECT_TRUE(HasCheckText(page, QStringLiteral("Computer audio")));
    EXPECT_FALSE(HasCheckText(page, QStringLiteral("Other system audio")));
    EXPECT_FALSE(AppSectionVisible(page)) << "App section must be hidden for Display target";
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Not available for current capture target")));
}

TEST_F(ConfigPageTest, AudioPolicy_WindowMode_ShowsAppPlusOtherSystemPlusMic) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Window;
    state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Mic, false, false},
        {recorder_core::AudioSourceKind::Sys, false, false},
    };
    page.setAudioUiState(state);

    EXPECT_TRUE(HasCheckText(page, QStringLiteral("Application audio")));
    EXPECT_TRUE(HasCheckText(page, QStringLiteral("Other system audio")));
    EXPECT_FALSE(HasCheckText(page, QStringLiteral("Computer audio")));
    EXPECT_TRUE(AppSectionVisible(page)) << "App section must be visible for Window target";
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Not available for current capture target")));
}

TEST_F(ConfigPageTest, AudioPolicy_DisplayToWindow_AppSectionBecomesVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState display_state;
    display_state.target_kind = capability::CaptureTargetKind::Display;
    display_state.source_rows = {{recorder_core::AudioSourceKind::SystemOutput, true, false},
                                 {recorder_core::AudioSourceKind::Mic, false, false}};
    page.setAudioUiState(display_state);
    EXPECT_FALSE(AppSectionVisible(page));

    capability::AudioUiState window_state;
    window_state.target_kind = capability::CaptureTargetKind::Window;
    window_state.source_rows = {{recorder_core::AudioSourceKind::App, true, false},
                                {recorder_core::AudioSourceKind::Mic, false, false},
                                {recorder_core::AudioSourceKind::Sys, false, false}};
    page.setAudioUiState(window_state);
    EXPECT_TRUE(AppSectionVisible(page));
}

TEST_F(ConfigPageTest, AudioPolicy_WindowToDisplay_AppSectionHides) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState window_state;
    window_state.target_kind = capability::CaptureTargetKind::Window;
    window_state.source_rows = {{recorder_core::AudioSourceKind::App, true, false},
                                {recorder_core::AudioSourceKind::Mic, false, false},
                                {recorder_core::AudioSourceKind::Sys, false, false}};
    page.setAudioUiState(window_state);
    EXPECT_TRUE(AppSectionVisible(page));

    capability::AudioUiState display_state;
    display_state.target_kind = capability::CaptureTargetKind::Display;
    display_state.source_rows = {{recorder_core::AudioSourceKind::SystemOutput, true, false},
                                 {recorder_core::AudioSourceKind::Mic, false, false}};
    page.setAudioUiState(display_state);
    EXPECT_FALSE(AppSectionVisible(page));
}

TEST_F(ConfigPageTest, AudioPolicy_AppMeterInactiveForDisplayMode) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState display_state;
    display_state.target_kind = capability::CaptureTargetKind::Display;
    display_state.source_rows = {{recorder_core::AudioSourceKind::SystemOutput, true, false},
                                 {recorder_core::AudioSourceKind::Mic, false, false}};
    page.setAudioUiState(display_state);

    // App meter must stay inactive regardless of level update.
    page.setAudioMeterLevels(0.8f, 0.5f, 0.0f, true, false, false);

    auto* app_meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioAppMeter"));
    ASSERT_NE(app_meter, nullptr);
    EXPECT_FALSE(app_meter->isActive()) << "App meter must not be active for Display mode";
}

TEST_F(ConfigPageTest, AudioPolicy_SysMeterActiveForDisplayMode) {
    ConfigPage page(output_defaults_, video_defaults_);

    capability::AudioUiState display_state;
    display_state.target_kind = capability::CaptureTargetKind::Display;
    display_state.source_rows = {{recorder_core::AudioSourceKind::SystemOutput, true, false},
                                 {recorder_core::AudioSourceKind::Mic, false, false}};
    page.setAudioUiState(display_state);

    page.setAudioMeterLevels(0.7f, 0.0f, 0.0f, /*sys_active=*/true, false, false);

    auto* sys_meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    ASSERT_NE(sys_meter, nullptr);
    EXPECT_TRUE(sys_meter->isActive()) << "System meter (Computer audio) must be active for Display mode";
}

// ---------------------------------------------------------------------------
// Lock/order invariant tests
// ---------------------------------------------------------------------------

// Helper: build a Display AudioUiState with a sys + mic row.
static capability::AudioUiState MakeDisplayAudioState() {
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Display;
    s.source_rows = {{recorder_core::AudioSourceKind::SystemOutput, true, false},
                     {recorder_core::AudioSourceKind::Mic, false, false}};
    return s;
}

// Helper: build a Window AudioUiState with app + sys + mic rows.
static capability::AudioUiState MakeWindowAudioState() {
    capability::AudioUiState s;
    s.target_kind = capability::CaptureTargetKind::Window;
    s.source_rows = {{recorder_core::AudioSourceKind::App, true, false},
                     {recorder_core::AudioSourceKind::Sys, false, false},
                     {recorder_core::AudioSourceKind::Mic, false, false}};
    return s;
}

TEST_F(ConfigPageTest, LockOrderInvariant_AudioThenLock_AppCheckDisabled) {
    // Test 23/25: setAudioUiState followed by setRecordingControlsLocked(true)
    // must keep the App checkbox disabled.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeWindowAudioState());
    page.setRecordingControlsLocked(true);

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    EXPECT_FALSE(app_check->isEnabled()) << "Audio then lock: App checkbox must be disabled when controls are locked";
}

TEST_F(ConfigPageTest, LockOrderInvariant_LockThenAudio_AppCheckDisabled) {
    // Test 25: setRecordingControlsLocked(true) followed by setAudioUiState
    // must keep the App checkbox disabled.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setRecordingControlsLocked(true);
    page.setAudioUiState(MakeWindowAudioState());

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    EXPECT_FALSE(app_check->isEnabled())
        << "Lock then audio: App checkbox must remain disabled when controls are locked";
}

TEST_F(ConfigPageTest, LockOrderInvariant_MeterUpdateCannotReenableLockedControls) {
    // Test 25: A setAudioMeterLevels call must not re-enable locked controls.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeWindowAudioState());
    page.setRecordingControlsLocked(true);

    // Verify locked before meter update.
    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    ASSERT_FALSE(app_check->isEnabled());

    // Apply a meter update — must not re-enable the locked checkbox.
    page.setAudioMeterLevels(0.5f, 0.3f, 0.1f, true, true, true);
    EXPECT_FALSE(app_check->isEnabled()) << "Meter update must not re-enable a locked App checkbox";
}

TEST_F(ConfigPageTest, LockOrderInvariant_UnlockRestoresControls) {
    // Unlock after lock must restore the App checkbox to enabled.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeWindowAudioState());
    page.setRecordingControlsLocked(true);
    page.setRecordingControlsLocked(false);

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    EXPECT_TRUE(app_check->isEnabled()) << "After unlock, App checkbox must be re-enabled for Window target";
}

TEST_F(ConfigPageTest, LockOrderInvariant_DisplayTarget_AppCheckAlwaysDisabledRegardlessOfLock) {
    // Test 23/27: Display target — App check is always disabled (not visible/available),
    // regardless of lock state.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeDisplayAudioState());

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    EXPECT_FALSE(app_check->isEnabled()) << "Display target: App checkbox must be disabled (not available)";

    page.setRecordingControlsLocked(true);
    EXPECT_FALSE(app_check->isEnabled()) << "Display target + lock: App checkbox must still be disabled";

    page.setRecordingControlsLocked(false);
    EXPECT_FALSE(app_check->isEnabled()) << "Display target + unlock: App checkbox must stay disabled (no App row)";
}

TEST_F(ConfigPageTest, AudioState_OneSnapshotDeterminesRowVisibility) {
    // Test 23: One audio snapshot fully determines row visibility.
    // Window target → App section visible; Display target → App section hidden.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeWindowAudioState());
    EXPECT_TRUE(AppSectionVisible(page)) << "Window target: App section must be visible";

    page.setAudioUiState(MakeDisplayAudioState());
    EXPECT_FALSE(AppSectionVisible(page)) << "Display target: App section must be hidden";
}

TEST_F(ConfigPageTest, AudioState_TargetSwitch_UpdatesSettingsImmediately) {
    // Test 26: Target switch must update Settings audio card state immediately.
    ConfigPage page(output_defaults_, video_defaults_);

    // Start with Window.
    page.setAudioUiState(MakeWindowAudioState());
    EXPECT_TRUE(AppSectionVisible(page));

    // Switch to Display.
    page.setAudioUiState(MakeDisplayAudioState());
    EXPECT_FALSE(AppSectionVisible(page)) << "After switching to Display, App section must disappear immediately";
}

TEST_F(ConfigPageTest, AudioState_NoStaleAppRow_AfterWindowToDisplay) {
    // Test 27: No stale App row after Window → Display switch.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeWindowAudioState());
    page.setAudioUiState(MakeDisplayAudioState());

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    // App section hidden → the check is either hidden or disabled.
    EXPECT_FALSE(app_check->isEnabled()) << "After Window → Display switch, App checkbox must not be enabled";
}

TEST_F(ConfigPageTest, AudioState_NoMissingRow_AfterDisplayToWindow) {
    // Test 28: No hidden relevant row after Display → Window.
    ConfigPage page(output_defaults_, video_defaults_);

    page.setAudioUiState(MakeDisplayAudioState());
    EXPECT_FALSE(AppSectionVisible(page));

    page.setAudioUiState(MakeWindowAudioState());
    EXPECT_TRUE(AppSectionVisible(page)) << "After Display → Window switch, App section must appear";

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    ASSERT_NE(app_check, nullptr);
    EXPECT_TRUE(app_check->isEnabled()) << "After Display → Window switch, App checkbox must be enabled";
}

// ---------------------------------------------------------------------------
// Preset card UX tests (complete preset management workflow)
// ---------------------------------------------------------------------------

TEST_F(ConfigPageTest, PresetCombo_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    // "profileCombo" is the legacy objectName kept for existing tests.
    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("profileCombo")), nullptr);
}

TEST_F(ConfigPageTest, PresetSaveButton_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("presetSaveButton")), nullptr);
}

TEST_F(ConfigPageTest, PresetSaveAsButton_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton")), nullptr);
}

TEST_F(ConfigPageTest, PresetDirtyIndicator_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_NE(page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator")), nullptr);
}

// S1-REDESIGN: presetDefaultBadge removed (redundant — combo already shows the name).
TEST_F(ConfigPageTest, PresetDefaultBadge_IsRemoved) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDefaultBadge")), nullptr)
        << "presetDefaultBadge was removed in S1-redesign; it must not exist";
}

TEST_F(ConfigPageTest, PresetManageButton_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_NE(page.findChild<QToolButton*>(QStringLiteral("presetManageButton")), nullptr);
}

TEST_F(ConfigPageTest, SetPresetOptions_PopulatesComboWithIds) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption a;
    a.id = QStringLiteral("preset_a");
    a.label = QStringLiteral("Preset A");
    ConfigPage::ProfileOption b;
    b.id = QStringLiteral("preset_b");
    b.label = QStringLiteral("Preset B");

    page.setPresetOptions({a, b}, QStringLiteral("preset_a"), QStringLiteral("preset_a"), false);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("preset_a"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("preset_b"));
}

TEST_F(ConfigPageTest, SetPresetOptions_MarkesDefaultWithStar_WhenNotSelected) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption dflt;
    dflt.id = QStringLiteral("default_id");
    dflt.label = QStringLiteral("Default Preset");
    ConfigPage::ProfileOption other;
    other.id = QStringLiteral("other_id");
    other.label = QStringLiteral("Other Preset");

    // Select "other", default is "default_id" — default row must get "★" suffix.
    page.setPresetOptions({dflt, other}, QStringLiteral("other_id"), QStringLiteral("default_id"), false);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    // Default (index 0) should have "★" in its text since it's not selected.
    EXPECT_TRUE(combo->itemText(0).contains(QStringLiteral("★")))
        << "Non-selected default must carry a ★ suffix; got: " << combo->itemText(0).toStdString();
    // Selected (index 1) must not carry the "★".
    EXPECT_FALSE(combo->itemText(1).contains(QStringLiteral("★")));
}

// S1-REDESIGN: the two badge visibility tests below are replaced by a no-badge assertion.
// The "default" state is now signalled only via a "★" suffix in the combo item text.
TEST_F(ConfigPageTest, SetPresetOptions_SelectedIsDefault_NoBadgeWidget) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("dflt");
    p.label = QStringLiteral("My Preset");

    page.setPresetOptions({p}, QStringLiteral("dflt"), QStringLiteral("dflt"), false);

    // Badge widget was removed in S1-redesign.
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDefaultBadge")), nullptr)
        << "presetDefaultBadge must not exist (removed in S1-redesign)";
}

TEST_F(ConfigPageTest, SetPresetOptions_SelectedIsNotDefault_NoBadgeWidget) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption dflt;
    dflt.id = QStringLiteral("d");
    dflt.label = QStringLiteral("Default");
    ConfigPage::ProfileOption other;
    other.id = QStringLiteral("o");
    other.label = QStringLiteral("Other");

    page.setPresetOptions({dflt, other}, QStringLiteral("o"), QStringLiteral("d"), false);

    // Badge widget was removed in S1-redesign.
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDefaultBadge")), nullptr)
        << "presetDefaultBadge must not exist (removed in S1-redesign)";
}

TEST_F(ConfigPageTest, SetPresetOptions_DirtyTrue_ShowsDirtyIndicatorAndEnablesSave) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("x");
    p.label = QStringLiteral("X");
    page.setPresetOptions({p}, QStringLiteral("x"), QStringLiteral("x"), /*dirty=*/true);

    auto* indicator = page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator"));
    ASSERT_NE(indicator, nullptr);
    EXPECT_FALSE(indicator->isHidden()) << "Dirty indicator must be visible when dirty=true";

    auto* save_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveButton"));
    ASSERT_NE(save_btn, nullptr);
    EXPECT_FALSE(save_btn->isHidden()) << "Save button must be visible when dirty";
    EXPECT_TRUE(save_btn->isEnabled()) << "Save button must be enabled when dirty";
}

TEST_F(ConfigPageTest, SetPresetOptions_DirtyFalse_HidesDirtyIndicatorAndSaveButton) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("x");
    p.label = QStringLiteral("X");
    page.setPresetOptions({p}, QStringLiteral("x"), QStringLiteral("x"), /*dirty=*/false);

    auto* indicator = page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator"));
    ASSERT_NE(indicator, nullptr);
    EXPECT_TRUE(indicator->isHidden()) << "Dirty indicator must be hidden when clean";

    auto* save_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveButton"));
    ASSERT_NE(save_btn, nullptr);
    EXPECT_TRUE(save_btn->isHidden()) << "Save button must be hidden when clean";
}

TEST_F(ConfigPageTest, SetPresetDirty_TogglesIndicatorAndSaveButton) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("p");
    p.label = QStringLiteral("P");
    page.setPresetOptions({p}, QStringLiteral("p"), QStringLiteral("p"), false);

    auto* indicator = page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator"));
    auto* save_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveButton"));
    ASSERT_NE(indicator, nullptr);
    ASSERT_NE(save_btn, nullptr);

    page.setPresetDirty(true);
    EXPECT_FALSE(indicator->isHidden()) << "Dirty indicator must appear after setPresetDirty(true)";
    EXPECT_FALSE(save_btn->isHidden()) << "Save button must appear after setPresetDirty(true)";
    EXPECT_TRUE(save_btn->isEnabled());

    page.setPresetDirty(false);
    EXPECT_TRUE(indicator->isHidden()) << "Dirty indicator must hide after setPresetDirty(false)";
    EXPECT_TRUE(save_btn->isHidden()) << "Save button must hide after setPresetDirty(false)";
}

TEST_F(ConfigPageTest, ComboSelection_EmitsPresetSelected) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption a;
    a.id = QStringLiteral("aa");
    a.label = QStringLiteral("AA");
    ConfigPage::ProfileOption b;
    b.id = QStringLiteral("bb");
    b.label = QStringLiteral("BB");
    page.setPresetOptions({a, b}, QStringLiteral("aa"), QString(), false);

    QString emitted_id;
    QObject::connect(&page, &ConfigPage::presetSelected, [&emitted_id](const QString& id) { emitted_id = id; });

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    combo->setCurrentIndex(1); // switch to "bb"
    EXPECT_EQ(emitted_id, QStringLiteral("bb")) << "Selecting a combo row must emit presetSelected(id)";
}

TEST_F(ConfigPageTest, SetPresetOptionsDoesNotEmitPresetSelected) {
    // setPresetOptions is a programmatic update and must NOT emit presetSelected.
    ConfigPage page(output_defaults_, video_defaults_);

    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::presetSelected, [&emit_count](const QString&) { ++emit_count; });

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("p");
    p.label = QStringLiteral("P");
    page.setPresetOptions({p}, QStringLiteral("p"), QStringLiteral("p"), false);

    EXPECT_EQ(emit_count, 0) << "setPresetOptions must not emit presetSelected";
}

TEST_F(ConfigPageTest, SetRecordingControlsLocked_DisablesPresetSaveButtons) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Make the page dirty so Save button is normally enabled.
    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("p");
    p.label = QStringLiteral("P");
    page.setPresetOptions({p}, QStringLiteral("p"), QStringLiteral("p"), /*dirty=*/true);

    auto* save_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveButton"));
    auto* save_as_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton"));
    ASSERT_NE(save_btn, nullptr);
    ASSERT_NE(save_as_btn, nullptr);

    // Baseline: before lock, save is enabled.
    EXPECT_TRUE(save_btn->isEnabled());
    EXPECT_TRUE(save_as_btn->isEnabled());

    page.setRecordingControlsLocked(true);

    EXPECT_FALSE(save_btn->isEnabled()) << "Save button must be disabled when locked";
    EXPECT_FALSE(save_as_btn->isEnabled()) << "Save As button must be disabled when locked";

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    EXPECT_FALSE(manage_btn->isEnabled()) << "Manage button must be disabled when locked";
}

TEST_F(ConfigPageTest, OverflowMenu_ExposesExpectedActions) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    auto* menu = manage_btn->menu();
    ASSERT_NE(menu, nullptr);

    // Collect all action texts.
    QStringList action_texts;
    for (const auto* action : menu->actions()) {
        if (!action->isSeparator() && !action->text().isEmpty())
            action_texts << action->text();
    }

    EXPECT_TRUE(action_texts.contains(QStringLiteral("Save preset"))) << "Missing: Save preset";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("New preset from default…")))
        << "Missing: New preset from default…";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Duplicate preset"))) << "Missing: Duplicate preset";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Rename preset…"))) << "Missing: Rename preset…";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Delete preset"))) << "Missing: Delete preset";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Set as default preset"))) << "Missing: Set as default preset";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Reset changes"))) << "Missing: Reset changes";
    EXPECT_TRUE(action_texts.contains(QStringLiteral("Reset all presets to factory defaults…")))
        << "Missing: Reset all presets to factory defaults…";
}

TEST_F(ConfigPageTest, SaveButton_Click_EmitsSavePresetRequested) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("p");
    p.label = QStringLiteral("P");
    page.setPresetOptions({p}, QStringLiteral("p"), QStringLiteral("p"), /*dirty=*/true);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::savePresetRequested, [&emitted]() { emitted = true; });

    auto* save_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveButton"));
    ASSERT_NE(save_btn, nullptr);
    save_btn->click();
    EXPECT_TRUE(emitted) << "Clicking Save button must emit savePresetRequested";
}

TEST_F(ConfigPageTest, SetDefaultPresetAction_Disabled_WhenSelectedIsDefault) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("d");
    p.label = QStringLiteral("D");
    // selected == default → "Set as default" must be disabled.
    page.setPresetOptions({p}, QStringLiteral("d"), QStringLiteral("d"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    auto* menu = manage_btn->menu();
    ASSERT_NE(menu, nullptr);

    for (const auto* action : menu->actions()) {
        if (action->text() == QStringLiteral("Set as default preset")) {
            EXPECT_FALSE(action->isEnabled())
                << "Set as default must be disabled when selected preset is already the default";
            return;
        }
    }
    FAIL() << "Set as default preset action not found in menu";
}

TEST_F(ConfigPageTest, SetDefaultPresetAction_Enabled_WhenSelectedIsNotDefault) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption dflt;
    dflt.id = QStringLiteral("d");
    dflt.label = QStringLiteral("D");
    ConfigPage::ProfileOption other;
    other.id = QStringLiteral("o");
    other.label = QStringLiteral("O");
    // selected != default → "Set as default" must be enabled.
    page.setPresetOptions({dflt, other}, QStringLiteral("o"), QStringLiteral("d"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    auto* menu = manage_btn->menu();
    ASSERT_NE(menu, nullptr);

    for (const auto* action : menu->actions()) {
        if (action->text() == QStringLiteral("Set as default preset")) {
            EXPECT_TRUE(action->isEnabled())
                << "Set as default must be enabled when selected preset is not the default";
            return;
        }
    }
    FAIL() << "Set as default preset action not found in menu";
}

TEST_F(ConfigPageTest, ResetChanges_And_ResetToDefaults_AreDistinctActions) {
    // The two reset actions must never be merged into one ambiguous action.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    auto* menu = manage_btn->menu();
    ASSERT_NE(menu, nullptr);

    bool found_reset_changes = false;
    bool found_reset_all = false;
    for (const auto* action : menu->actions()) {
        if (action->text() == QStringLiteral("Reset changes"))
            found_reset_changes = true;
        if (action->text().contains(QStringLiteral("factory defaults")))
            found_reset_all = true;
    }
    EXPECT_TRUE(found_reset_changes) << "Missing 'Reset changes' action in overflow menu";
    EXPECT_TRUE(found_reset_all) << "Missing 'Reset all presets to factory defaults' action in overflow menu";
}

// ---- MP4 automatic-split gating (VR-005 / functional P2-005) -------------

TEST_F(ConfigPageTest, Mp4DisablesAutomaticSplitControlsWithHonestSummary) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Every30Min;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled());

    auto* summary = page.findChild<QLabel*>(QStringLiteral("splitSummaryLabel"));
    ASSERT_NE(summary, nullptr);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("MKV/WebM")));
    EXPECT_FALSE(summary->text().contains(QStringLiteral("Manual splits")));
}

TEST_F(ConfigPageTest, SplitModeSurvivesContainerRoundTripThroughMp4) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.mode = SplitRecordingMode::Every15Min;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled());
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));

    auto* mp4_btn = page.findChild<QPushButton*>(QStringLiteral("containerMp4Button"));
    ASSERT_NE(mp4_btn, nullptr);
    mp4_btn->click();
    EXPECT_FALSE(combo->isEnabled());
    // The configured mode is preserved while MP4 is selected (not reset to Off).
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));

    auto* mkv_btn = page.findChild<QPushButton*>(QStringLiteral("containerMkvButton"));
    ASSERT_NE(mkv_btn, nullptr);
    mkv_btn->click();
    EXPECT_TRUE(combo->isEnabled());
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));
}

TEST_F(ConfigPageTest, Mp4HidesCustomSplitIntervalEditor) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Custom;
    settings.split.custom_minutes = 45;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("splitCustomMinutesSpin"));
    ASSERT_NE(spin, nullptr);
    EXPECT_FALSE(spin->isEnabled());
    ASSERT_NE(spin->parentWidget(), nullptr);
    EXPECT_TRUE(spin->parentWidget()->isHidden());
    // Custom interval value survives the MP4 detour.
    EXPECT_EQ(spin->value(), 45);
}

// ---- Split-by-size controls (SPLIT-BY-SIZE-R1) ----------------------------

TEST_F(ConfigPageTest, SplitSizeModeCombo_ExistsWithOffAndCustomItems) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitSizeModeCombo"));
    ASSERT_NE(combo, nullptr);
    // Should have at least Off and Custom.
    EXPECT_GE(combo->count(), 2);
}

TEST_F(ConfigPageTest, SplitSizeMode_Off_HidesCustomSizeSpin) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.size_mode = SplitSizeMode::Off;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("splitCustomSizeSpin"));
    ASSERT_NE(spin, nullptr);
    // When size_mode == Off the custom widget should be hidden.
    ASSERT_NE(spin->parentWidget(), nullptr);
    EXPECT_TRUE(spin->parentWidget()->isHidden());
}

TEST_F(ConfigPageTest, SplitSizeMode_Custom_ShowsCustomSizeSpin) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.size_mode = SplitSizeMode::Custom;
    settings.split.custom_size_mb = 512;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("splitCustomSizeSpin"));
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(spin->parentWidget(), nullptr);
    EXPECT_FALSE(spin->parentWidget()->isHidden());
    EXPECT_EQ(spin->value(), 512);
}

TEST_F(ConfigPageTest, Mp4_DisablesSplitSizeCombo) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.size_mode = SplitSizeMode::Custom;
    settings.split.custom_size_mb = 1024;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setOutputSettings(settings);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitSizeModeCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_FALSE(combo->isEnabled());
}

// ── SETTINGS-TIERS-P3: Presence + Appearance cards (moved from AdvancedPage) ──

TEST_F(ConfigPageTest, PresenceCard_CardTitleVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Presence"))) << "Settings must contain a Presence card title";
}

TEST_F(ConfigPageTest, PresenceCard_AllTogglesExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Presence rows use ExoToggle (right-aligned pill), found by objectName.
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("overlayCheck")), nullptr)
        << "Recording overlay ExoToggle missing";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("diagnosticsOverlayCheck")), nullptr)
        << "Diagnostics overlay ExoToggle missing";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("notificationsCheck")), nullptr)
        << "Notifications ExoToggle missing";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("keepInTrayCheck")), nullptr)
        << "Close-to-tray ExoToggle missing";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("quickControlsCheck")), nullptr)
        << "Quick controls ExoToggle missing";
}

TEST_F(ConfigPageTest, PresenceCard_SetShowOverlay_UpdatesCheckState) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setShowOverlay(false);

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("overlayCheck"));
    ASSERT_NE(toggle, nullptr) << "Recording overlay ExoToggle not found";
    EXPECT_FALSE(toggle->isOn()) << "setShowOverlay(false) must turn off the toggle";
}

TEST_F(ConfigPageTest, PresenceCard_SetShowOverlay_DoesNotEmitSignal) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::showOverlayChanged, [&emitted](bool) { emitted = true; });

    // Setter must use QSignalBlocker — no spurious emission.
    page.setShowOverlay(false);
    EXPECT_FALSE(emitted) << "setShowOverlay must not emit showOverlayChanged";
}

TEST_F(ConfigPageTest, PresenceCard_ShowOverlayToggle_EmitsSignal) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setShowOverlay(true);

    bool emitted = false;
    bool emitted_value = true;
    QObject::connect(&page, &ConfigPage::showOverlayChanged, [&](bool show) {
        emitted = true;
        emitted_value = show;
    });

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("overlayCheck"));
    ASSERT_NE(toggle, nullptr) << "Recording overlay ExoToggle not found";
    toggle->setOn(false); // user toggling triggers toggled() → signal
    EXPECT_TRUE(emitted) << "showOverlayChanged must be emitted on user toggle";
    EXPECT_FALSE(emitted_value);
}

TEST_F(ConfigPageTest, PresenceCard_SetKeepRunningInTray_UpdatesCheckState) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setKeepRunningInTray(true);

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("keepInTrayCheck"));
    ASSERT_NE(toggle, nullptr) << "Close-to-tray ExoToggle not found";
    EXPECT_TRUE(toggle->isOn()) << "setKeepRunningInTray(true) must turn on the toggle";
}

TEST_F(ConfigPageTest, AppearanceCard_CardTitleVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Appearance"))) << "Settings must contain an Appearance card title";
}

TEST_F(ConfigPageTest, AppearanceCard_ThemePickerHasFourCards) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The theme picker should have 4 theme card buttons (one per theme in kExoThemes).
    int theme_card_count = 0;
    for (const auto* btn : page.findChildren<QPushButton*>()) {
        if (btn->property("themePickerCard").toBool())
            ++theme_card_count;
    }
    EXPECT_EQ(theme_card_count, 4)
        << "Theme picker must have 4 cards (dark-default, dark-indigo, light-paper, light-slate)";
}

TEST_F(ConfigPageTest, AppearanceCard_SetThemeId_SelectsCorrectCard) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setThemeId(QStringLiteral("dark-default"));

    bool found = false;
    for (const auto* btn : page.findChildren<QPushButton*>()) {
        if (btn->property("themeId").toString() == QStringLiteral("dark-default")) {
            EXPECT_TRUE(btn->isChecked()) << "setThemeId must check the matching card";
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "dark-default theme card not found";
}

TEST_F(ConfigPageTest, AppearanceCard_SetThemeId_DoesNotEmitSignal) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::themeIdChanged, [&emitted](const QString&) { emitted = true; });

    page.setThemeId(QStringLiteral("dark-default"));
    EXPECT_FALSE(emitted) << "setThemeId must not emit themeIdChanged";
}

TEST_F(ConfigPageTest, AdvancedDetailsButton_IsGone) {
    ConfigPage page(output_defaults_, video_defaults_);

    // SETTINGS-TIERS-P3: the "Open Advanced" signpost button is removed.
    auto* advanced_btn = page.findChild<QPushButton*>(QStringLiteral("advancedDetailsBtn"));
    EXPECT_EQ(advanced_btn, nullptr) << "Settings must not contain the 'Open Advanced' signpost button after P3";
}

TEST_F(ConfigPageTest, DeveloperCard_HiddenByDefault) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Expert mode is off by default; the Developer card must be explicitly hidden.
    auto* card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(card, nullptr) << "settingsDeveloperCard widget not found";
    EXPECT_TRUE(card->isHidden()) << "Developer card must be hidden when expert mode is off";
}

TEST_F(ConfigPageTest, DeveloperCard_VisibleWhenExpertModeEnabled) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setExpertModeEnabled(true);

    auto* card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(card, nullptr) << "settingsDeveloperCard widget not found";
    EXPECT_FALSE(card->isHidden()) << "Developer card must not be hidden when expert mode is on";
}

// ── SETTINGS-SEARCH-R1: settings search box filter ───────────────────────────

TEST_F(ConfigPageTest, SettingsSearch_BoxExists_WithStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr) << "Settings must contain a search QLineEdit (objectName settingsSearchBox)";
    EXPECT_EQ(box->text(), QString()) << "Search box must be empty on construction";
}

TEST_F(ConfigPageTest, SettingsSearch_CountLabelExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* label = page.findChild<QLabel*>(QStringLiteral("settingsSearchCountLabel"));
    ASSERT_NE(label, nullptr) << "Settings must contain a match count label";
    // Count label is hidden when query is empty.
    EXPECT_TRUE(label->isHidden()) << "Match count label must be hidden when search box is empty";
}

TEST_F(ConfigPageTest, SettingsSearch_EmptyQuery_AllCardsVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);

    // Type something, then clear — must restore all cards.
    box->setText(QStringLiteral("audio"));
    box->clear();

    // Cards verified by presence of their card titles (visible QLabel).
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Preset")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Format & encoding")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Audio")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Webcam")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Output")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Presence")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Appearance")));
}

TEST_F(ConfigPageTest, SettingsSearch_AudioQuery_ShowsAudioHidesFmtAndWebcam) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("microphone"));

    // "Microphone" is an audio keyword → Audio card stays.
    // "Format & encoding" and "Webcam" must be hidden.
    // The containerSegmented widget lives in the Format & encoding card; if the card
    // is hidden, isHidden() is true for the card itself (not necessarily its descendants).
    // We instead check visibility of the audio panel by finding the VU meter which is
    // audio-only, and the container segmented which is format-only.
    auto* audio_meter = page.findChild<ui::widgets::VUMeterWidget*>(QStringLiteral("settingsAudioSysMeter"));
    ASSERT_NE(audio_meter, nullptr);
    // Audio meter's top-level-card ancestor is the audio panel; confirm it is not hidden.
    // Walk up until we find a widget whose parent is visible but the widget itself has a
    // known card title sibling.  The easiest stable check: the audio panel widget
    // that is a sibling of the meter IS visible (meter itself may not be mapped yet).
    // We simply verify that a search for "microphone" does not hide the audio panel's
    // direct parent by checking the frame's hidden state up the hierarchy.
    QWidget* w = audio_meter->parentWidget();
    bool audio_card_visible = false;
    while (w) {
        // The audio panel has a panelRole property.
        if (w->property("panelRole").toString() == QStringLiteral("panel")) {
            audio_card_visible = !w->isHidden();
            break;
        }
        w = w->parentWidget();
    }
    EXPECT_TRUE(audio_card_visible) << "Audio card must remain visible when query matches audio keywords";

    // Format & encoding card must be hidden.
    QWidget* container_seg = page.findChild<QWidget*>(QStringLiteral("containerSegmented"));
    ASSERT_NE(container_seg, nullptr);
    QWidget* fmt_card = container_seg->parentWidget();
    bool fmt_card_hidden = false;
    while (fmt_card) {
        if (fmt_card->property("panelRole").toString() == QStringLiteral("panel")) {
            fmt_card_hidden = fmt_card->isHidden();
            break;
        }
        fmt_card = fmt_card->parentWidget();
    }
    EXPECT_TRUE(fmt_card_hidden) << "Format & encoding card must be hidden when query is 'microphone'";
}

TEST_F(ConfigPageTest, SettingsSearch_MatchCountLabel_ShowsAfterTyping) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("audio"));

    auto* label = page.findChild<QLabel*>(QStringLiteral("settingsSearchCountLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->isHidden()) << "Match count label must appear when query is non-empty";
    EXPECT_FALSE(label->text().isEmpty()) << "Match count label must not be empty during search";
}

TEST_F(ConfigPageTest, SettingsSearch_NoMatch_CountLabelSaysNoMatches) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("zzzzunlikelysearchterm99999"));

    auto* label = page.findChild<QLabel*>(QStringLiteral("settingsSearchCountLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), QStringLiteral("No matches"))
        << "No-match query must show 'No matches' in the count label";
}

// Wave 2: the per-card Output "Advanced" expander was dissolved. Split controls are now
// expert-gated and live inside the Output card (objectName "splitExpertSection"). Searching
// must therefore respect the expert gate rather than auto-opening a (no-longer-existing)
// expander.
TEST_F(ConfigPageTest, SettingsSearch_SplitQuery_ExpertOn_KeepsSplitControlsVisible) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* split = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(split, nullptr);
    ASSERT_FALSE(split->isHidden()) << "Split controls must be visible once Expert mode is on";

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("split"));

    // 'split' matches the Output card; with Expert on, the split controls stay visible.
    EXPECT_FALSE(split->isHidden()) << "Searching 'split' with Expert on must keep the split controls visible";
}

TEST_F(ConfigPageTest, SettingsSearch_SplitQuery_ExpertOff_SplitStaysHidden) {
    ConfigPage page(output_defaults_, video_defaults_);
    ASSERT_FALSE(page.expertModeEnabled());

    auto* split = page.findChild<QWidget*>(QStringLiteral("splitExpertSection"));
    ASSERT_NE(split, nullptr);
    ASSERT_TRUE(split->isHidden()) << "Split controls start hidden when Expert mode is off";

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("split"));

    // Split is expert-gated: a search must NOT reveal it without Expert mode.
    EXPECT_TRUE(split->isHidden())
        << "Searching 'split' must not reveal expert-gated split controls when Expert is off";
}

TEST_F(ConfigPageTest, SettingsSearch_DeveloperKeyword_ExpertOff_ShowsHint) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Expert mode is off by default.
    ASSERT_FALSE(page.expertModeEnabled());

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("developer"));

    // Developer card must stay hidden (Expert mode off).
    auto* dev_card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(dev_card, nullptr);
    EXPECT_TRUE(dev_card->isHidden()) << "Developer card must remain hidden when Expert mode is off during search";

    // Expert hint label must appear.
    auto* hint = page.findChild<QLabel*>(QStringLiteral("searchExpertHintLabel"));
    ASSERT_NE(hint, nullptr);
    EXPECT_FALSE(hint->isHidden())
        << "Expert hint label must appear when a Developer keyword matches but Expert is off";
    EXPECT_FALSE(hint->text().isEmpty()) << "Expert hint label must have non-empty text";
}

TEST_F(ConfigPageTest, SettingsSearch_DeveloperKeyword_ExpertOn_ShowsCard) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setExpertModeEnabled(true);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("developer"));

    auto* dev_card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(dev_card, nullptr);
    EXPECT_FALSE(dev_card->isHidden()) << "Developer card must be visible when Expert mode is on and keyword matches";

    auto* hint = page.findChild<QLabel*>(QStringLiteral("searchExpertHintLabel"));
    ASSERT_NE(hint, nullptr);
    EXPECT_TRUE(hint->isHidden()) << "Expert hint must be hidden when Expert mode is already on";
}

TEST_F(ConfigPageTest, SettingsSearch_ClearAfterDeveloperMatch_HidesHintShowsGatedState) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* box = page.findChild<QLineEdit*>(QStringLiteral("settingsSearchBox"));
    ASSERT_NE(box, nullptr);
    box->setText(QStringLiteral("developer"));

    // Expert hint visible while searching.
    auto* hint = page.findChild<QLabel*>(QStringLiteral("searchExpertHintLabel"));
    ASSERT_NE(hint, nullptr);
    ASSERT_FALSE(hint->isHidden());

    box->clear();

    EXPECT_TRUE(hint->isHidden()) << "Expert hint must hide after clearing the search box";

    // Developer card must return to gated state (hidden because Expert is off).
    auto* dev_card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(dev_card, nullptr);
    EXPECT_TRUE(dev_card->isHidden()) << "Developer card must revert to hidden state after clearing search";
}

// ── S5: SettingsPopoverRow unit tests ────────────────────────────────────────

class SettingsPopoverRowTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

TEST_F(SettingsPopoverRowTest, ConstructsWithLabel) {
    ui::widgets::SettingsPopoverRow row(QStringLiteral("Test row"));
    const auto labels = row.findChildren<QLabel*>();
    bool found = false;
    for (const auto* lbl : labels) {
        if (lbl->text() == QStringLiteral("Test row")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Row label must appear in SettingsPopoverRow";
}

TEST_F(SettingsPopoverRowTest, CogButtonExists) {
    ui::widgets::SettingsPopoverRow row(QStringLiteral("Test row"));
    auto* cog = row.findChild<QToolButton*>(QStringLiteral("settingsPopoverCog"));
    ASSERT_NE(cog, nullptr) << "SettingsPopoverRow must contain a cog QToolButton";
    EXPECT_TRUE(cog->isEnabled());
}

TEST_F(SettingsPopoverRowTest, PopoverContentLayout_AcceptsSubControl) {
    ui::widgets::SettingsPopoverRow row(QStringLiteral("Mic post-processing"));
    auto* layout = row.popoverContentLayout();
    ASSERT_NE(layout, nullptr);

    auto* sub_check = new QCheckBox(QStringLiteral("High-pass filter"));
    sub_check->setObjectName(QStringLiteral("testHpfCheck"));
    layout->addWidget(sub_check);

    // The sub-control must be findable as a descendant of the row.
    auto* found = row.findChild<QCheckBox*>(QStringLiteral("testHpfCheck"));
    ASSERT_NE(found, nullptr) << "Sub-control added to popoverContentLayout must be a descendant of the row";
}

TEST_F(SettingsPopoverRowTest, SetStatusText_ShowsAndHidesLabel) {
    ui::widgets::SettingsPopoverRow row(QStringLiteral("Microphone post-processing"));

    row.setStatusText(QStringLiteral("High-pass \xC2\xB7 Gate"));
    const auto labels = row.findChildren<QLabel*>();
    bool found_status = false;
    for (const auto* lbl : labels) {
        // Use !isHidden() rather than isVisible(): in headless tests the row widget
        // is never show()n, so isVisible() checks the full ancestor chain and returns
        // false even when the label's own hidden flag is cleared.
        if (lbl->text() == QStringLiteral("High-pass \xC2\xB7 Gate") && !lbl->isHidden()) {
            found_status = true;
            break;
        }
    }
    EXPECT_TRUE(found_status) << "setStatusText must un-hide the status label with the given text";

    // Setting empty string must hide it.
    row.setStatusText(QString());
    bool still_unhidden = false;
    for (const auto* lbl : row.findChildren<QLabel*>()) {
        if (lbl->text() == QStringLiteral("High-pass \xC2\xB7 Gate") && !lbl->isHidden()) {
            still_unhidden = true;
            break;
        }
    }
    EXPECT_FALSE(still_unhidden) << "setStatusText(empty) must hide the status label";
}

// ── S5: ConfigPage integration — sub-controls still findable by objectName ───

TEST_F(ConfigPageTest, S5_MicPostProcessing_SubControlsStillFindableByObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);

    // All four mic DSP controls must still exist (reparented into popover, not deleted).
    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micHpfCheck")), nullptr)
        << "micHpfCheck must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QDoubleSpinBox*>(QStringLiteral("micHpfCutoffSpin")), nullptr)
        << "micHpfCutoffSpin must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micGateCheck")), nullptr)
        << "micGateCheck must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QDoubleSpinBox*>(QStringLiteral("micGateThresholdSpin")), nullptr)
        << "micGateThresholdSpin must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micAgcCheck")), nullptr)
        << "micAgcCheck must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QDoubleSpinBox*>(QStringLiteral("micAgcTargetSpin")), nullptr)
        << "micAgcTargetSpin must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micRnnoiseCheck")), nullptr)
        << "micRnnoiseCheck must still exist after S5 reparenting";
}

TEST_F(ConfigPageTest, S5_LimiterControls_StillFindableByObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("limiterCheck")), nullptr)
        << "limiterCheck must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QDoubleSpinBox*>(QStringLiteral("limiterCeilingSpin")), nullptr)
        << "limiterCeilingSpin must still exist after S5 reparenting";
}

TEST_F(ConfigPageTest, S5_SplitControls_StillFindableByObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("splitModeCombo")), nullptr)
        << "splitModeCombo must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("splitSizeModeCombo")), nullptr)
        << "splitSizeModeCombo must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("splitCustomMinutesSpin")), nullptr)
        << "splitCustomMinutesSpin must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("splitCustomSizeSpin")), nullptr)
        << "splitCustomSizeSpin must still exist after S5 reparenting";
}

} // namespace
} // namespace exosnap
