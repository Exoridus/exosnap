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
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/support_level.h>

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "models/WebcamSettings.h"
#include "pages/ConfigPage.h"
#include "ui/widgets/CameraPreview.h"
#include "ui/widgets/ExoCheckBox.h"
#include "ui/widgets/ExoToggle.h"
#include "ui/widgets/InfoHintIcon.h"
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

TEST_F(ConfigPageTest, ContainerComboExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("containerCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_GE(combo->findData(static_cast<int>(capability::Container::Matroska)), 0);
    EXPECT_GE(combo->findData(static_cast<int>(capability::Container::WebM)), 0);
    EXPECT_GE(combo->findData(static_cast<int>(capability::Container::Mp4)), 0);
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
        if (b->text() == QStringLiteral("\xe2\x80\xa6")) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Preset overflow button not found";
}

TEST_F(ConfigPageTest, HybridCardTitles_AreVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The hybrid Settings IA splits the old combined card into discrete compact cards.
    // v10 further splits "Format & encoding" into "Container & codecs" + "Quality & timing".
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Preset")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Container & codecs")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Quality & timing")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Audio")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Webcam")));
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Output")));

    // The old combined "Preset & Format" / "Format & encoding" card titles are gone.
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Preset & Format")));
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Profile & Format")));
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Format & encoding")));
}

TEST_F(ConfigPageTest, OutputResolution_IsFunctional) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("outputResCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled()) << "Output resolution combo must be interactive";
    EXPECT_GE(combo->count(), 5);

    OutputSettingsModel changed;
    bool emitted = false;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, [&](const OutputSettingsModel& settings) {
        emitted = true;
        changed = settings;
    });

    const int idx1080 = combo->findData(static_cast<int>(OutputResolutionMode::FHD1080));
    ASSERT_GE(idx1080, 0);
    combo->setCurrentIndex(idx1080);
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

TEST_F(ConfigPageTest, TimingCombo_MapsToVideoSettingsAndMp4DisablesVfr) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* timing = page.findChild<QComboBox*>(QStringLiteral("timingCombo"));
    auto* container = page.findChild<QComboBox*>(QStringLiteral("containerCombo"));
    ASSERT_NE(timing, nullptr);
    ASSERT_NE(container, nullptr);

    VideoSettingsModel changed;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged,
                     [&](const VideoSettingsModel& settings) { changed = settings; });

    // Select VFR (itemData 0).
    const int vfr_idx = timing->findData(0);
    ASSERT_GE(vfr_idx, 0);
    timing->setCurrentIndex(vfr_idx);
    EXPECT_FALSE(changed.cfr);
    EXPECT_EQ(timing->currentData().toInt(), 0);

    // Switching to MP4 forces CFR and disables the VFR item.
    container->setCurrentIndex(container->findData(static_cast<int>(capability::Container::Mp4)));
    EXPECT_TRUE(changed.cfr);
    EXPECT_EQ(timing->currentData().toInt(), 1);

    auto* model = qobject_cast<QStandardItemModel*>(timing->model());
    ASSERT_NE(model, nullptr);
    EXPECT_FALSE(model->item(timing->findData(0))->isEnabled());
}

TEST_F(ConfigPageTest, FilenameTokenChips_AreShown) {
    // v10 (Task #4): token chips must be permanently present (not hidden behind a toggle).
    ConfigPage page(output_defaults_, video_defaults_);

    int chip_count = 0;
    const auto labels = page.findChildren<QLabel*>();
    for (const auto* label : labels) {
        if (label->property("labelRole").toString() == QStringLiteral("tokenChip"))
            ++chip_count;
    }
    EXPECT_GE(chip_count, 4) << "Output card should expose compact filename token chips";
    // All expected token chips must be present.
    EXPECT_EQ(chip_count, 8)
        << "Expected 8 token chips ({datetime},{date},{time},{app},{title},{target},{profile},{container})";
}

TEST_F(ConfigPageTest, FilenameTokenChips_SitBehindTheTokensDisclosure) {
    // Canon (suite-settings.jsx): the chips are collapsed by default and open
    // from the small "tokens" disclosure on the filename-pattern row.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* chip_flow = page.findChild<QWidget*>(QStringLiteral("tokenChipFlow"));
    ASSERT_NE(chip_flow, nullptr) << "tokenChipFlow widget must exist";
    EXPECT_TRUE(chip_flow->isHidden()) << "Token chips start collapsed";

    auto* toggle = page.findChild<QToolButton*>(QStringLiteral("tokensToggle"));
    ASSERT_NE(toggle, nullptr) << "tokens disclosure must exist";
    toggle->setChecked(true);
    EXPECT_FALSE(chip_flow->isHidden()) << "The disclosure reveals the chips";
    toggle->setChecked(false);
    EXPECT_TRUE(chip_flow->isHidden()) << "…and collapses them again";
}

TEST_F(ConfigPageTest, DeepLinkScroll_PulsesLandingOnTargetSection) {
    // The hotkey "Rebind" notification deep-links here via scrollToSection. Even when the
    // target card is already within the viewport the jump must give a visible cue, so
    // scrollToSection sets a transient "landing" property (accent border via QSS) on the
    // target section panel.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* hotkeys_panel = page.findChild<QWidget*>(QStringLiteral("settingsHotkeysPanel"));
    ASSERT_NE(hotkeys_panel, nullptr) << "hotkeys settings panel must exist";
    EXPECT_FALSE(hotkeys_panel->property("landing").toBool()) << "no landing cue before the jump";

    page.scrollToSection(QStringLiteral("settings/hotkeys"));
    EXPECT_TRUE(hotkeys_panel->property("landing").toBool())
        << "scrollToSection must pulse the landing cue on the target section";
}

TEST_F(ConfigPageTest, EmbeddedHotkeyRow_UnboundShowsOnlySet) {
    // v0.9 polish: the embedded Hotkeys card exposes no dead buttons — a bound row shows
    // the × (Clear) plus a "Change" primary; an unbound row collapses to just "Set".
    ConfigPage page(output_defaults_, video_defaults_);

    auto* set0 = page.findChild<QPushButton*>(QStringLiteral("settingsHkSetBtn_0"));
    auto* unset0 = page.findChild<QPushButton*>(QStringLiteral("settingsHkUnsetBtn_0"));
    ASSERT_NE(set0, nullptr);
    ASSERT_NE(unset0, nullptr);

    // Rows seed from non-empty default bindings, so the row starts bound.
    EXPECT_EQ(set0->text(), QStringLiteral("Change"));
    EXPECT_TRUE(unset0->isVisibleTo(&page)) << "the × is available while a binding exists";

    // Clearing the binding via the × drops the row to the unbound state.
    unset0->click();

    EXPECT_EQ(set0->text(), QStringLiteral("Set")) << "an unbound row's primary reverts to Set";
    EXPECT_FALSE(unset0->isVisibleTo(&page)) << "the × hides once the row is unbound";
}

TEST_F(ConfigPageTest, BuiltInAndModifiedStates_UsePresetCopy) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption builtin;
    builtin.id = QStringLiteral("builtin");
    builtin.label = QStringLiteral("High Quality AV1");
    builtin.built_in = true;
    builtin.modified = false;

    std::vector<ConfigPage::ProfileOption> options{builtin};
    // Clean state: the built-in marker lives INSIDE the combo option (carried on
    // kPresetBuiltInRole for the delegate), not in a separate toolbar label that
    // would shift the layout. The combo text carries no "(changed)" hint.
    page.setPresetOptions(options, builtin.id, /*dirty=*/false);
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Built-in preset")))
        << "the external 'Built-in preset' badge must no longer exist";
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), builtin.label);
    EXPECT_TRUE(combo->itemData(combo->currentIndex(), ConfigPage::kPresetBuiltInRole).toBool())
        << "the built-in option must carry the badge role for the delegate";

    // Dirty state: "(changed)" hint appears; the option keeps its built-in role.
    options[0].modified = true;
    page.setPresetOptions(options, builtin.id, /*dirty=*/true);
    EXPECT_EQ(combo->currentText(), builtin.label + QStringLiteral(" (changed)"));
    EXPECT_TRUE(combo->itemData(combo->currentIndex(), ConfigPage::kPresetBuiltInRole).toBool());

    // A user preset carries no built-in badge role.
    ConfigPage::ProfileOption user;
    user.id = QStringLiteral("user");
    user.label = QStringLiteral("My preset");
    user.built_in = false;
    page.setPresetOptions({user}, user.id, /*dirty=*/false);
    EXPECT_FALSE(combo->itemData(combo->currentIndex(), ConfigPage::kPresetBuiltInRole).toBool());
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

    page.setPresetOptions(opts, QStringLiteral("test"), false);

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
    EXPECT_EQ(changed.cq, recorder_core::CanonicalCq(recorder_core::NvencQualityPreset::Small));
    EXPECT_TRUE(small_segment->isChecked());
    EXPECT_TRUE(small_segment->property("qualitySegmentSelected").toBool());
    EXPECT_FALSE(high_segment->isChecked());

    balanced_segment->click();
    EXPECT_EQ(changed.cq, recorder_core::CanonicalCq(recorder_core::NvencQualityPreset::Balanced));
    EXPECT_TRUE(balanced_segment->isChecked());
    EXPECT_FALSE(small_segment->isChecked());

    high_segment->click();
    EXPECT_EQ(changed.cq, recorder_core::CanonicalCq(recorder_core::NvencQualityPreset::High));
    EXPECT_TRUE(high_segment->isChecked());
    EXPECT_FALSE(balanced_segment->isChecked());

    EXPECT_EQ(emit_count, 3);
}

TEST_F(ConfigPageTest, SetVideoSettings_UpdatesQualitySegmentSelection) {
    ConfigPage page(output_defaults_, video_defaults_);

    VideoSettingsModel balanced = video_defaults_;
    balanced.cq = recorder_core::CanonicalCq(recorder_core::NvencQualityPreset::Balanced);
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

// The "✓ Current format" footer summarises frame rate + timing, but its refresh
// used to hang off the container/codec paths only: changing the frame-rate combo
// (currentIndexChanged fires for programmatic setCurrentIndex too) never called
// updateFormatDisplay(), so the footer kept showing the construction-time
// "60 fps · CFR" regardless of the actual selection.
TEST_F(ConfigPageTest, FormatSummary_RefreshesOnFrameRateComboChange) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* frame_rate = page.findChild<QComboBox*>(QStringLiteral("frameRateCombo"));
    ASSERT_NE(frame_rate, nullptr);
    auto* summary = page.findChild<QLabel*>(QStringLiteral("compatOkLabel"));
    ASSERT_NE(summary, nullptr);

    const int idx24 = frame_rate->findData(24);
    ASSERT_GE(idx24, 0);
    frame_rate->setCurrentIndex(idx24);

    EXPECT_TRUE(summary->text().contains(QStringLiteral("24 fps")))
        << "Summary must follow the frame-rate combo, got: " << summary->text().toStdString();
}

// Same stale-summary bug on the fully programmatic path (preset load / visual
// harness): setVideoSettings() syncs every combo behind QSignalBlockers, so the
// footer must be refreshed explicitly there.
TEST_F(ConfigPageTest, FormatSummary_RefreshesOnSetVideoSettings) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* summary = page.findChild<QLabel*>(QStringLiteral("compatOkLabel"));
    ASSERT_NE(summary, nullptr);

    VideoSettingsModel changed = video_defaults_;
    changed.frame_rate_num = 24;
    changed.frame_rate_den = 1;
    changed.cfr = true;
    page.setVideoSettings(changed);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("24 fps")))
        << "Summary must follow setVideoSettings, got: " << summary->text().toStdString();
    EXPECT_TRUE(summary->text().contains(QStringLiteral("CFR")));

    changed.frame_rate_num = 60;
    changed.cfr = false;
    page.setVideoSettings(changed);
    EXPECT_TRUE(summary->text().contains(QStringLiteral("60 fps")));
    EXPECT_TRUE(summary->text().contains(QStringLiteral("VFR")))
        << "Summary must follow the CFR/VFR flag, got: " << summary->text().toStdString();
}

// The Frame timing combo drives video_settings_.cfr through onTimingSelected();
// the footer must follow it as well (same signal path as the frame-rate combo).
TEST_F(ConfigPageTest, FormatSummary_RefreshesOnTimingComboChange) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* timing = page.findChild<QComboBox*>(QStringLiteral("timingCombo"));
    ASSERT_NE(timing, nullptr);
    auto* summary = page.findChild<QLabel*>(QStringLiteral("compatOkLabel"));
    ASSERT_NE(summary, nullptr);

    const int vfr_idx = timing->findData(0);
    ASSERT_GE(vfr_idx, 0);
    timing->setCurrentIndex(vfr_idx);

    EXPECT_TRUE(summary->text().contains(QStringLiteral("VFR")))
        << "Summary must follow the timing combo, got: " << summary->text().toStdString();
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
    auto* timing_combo = page.findChild<QComboBox*>(QStringLiteral("timingCombo"));
    ASSERT_NE(timing_combo, nullptr);
    EXPECT_FALSE(timing_combo->isEnabled());
    auto* output_res = page.findChild<QComboBox*>(QStringLiteral("outputResCombo"));
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

TEST_F(ConfigPageTest, UpdatesCard_IsPresent) {
    // v10 (Task #5): Updates card must be present in the right column.
    ConfigPage page(output_defaults_, video_defaults_);

    auto* updates_card = page.findChild<QWidget*>(QStringLiteral("settingsUpdatesCard"));
    ASSERT_NE(updates_card, nullptr) << "settingsUpdatesCard must exist";
    EXPECT_FALSE(updates_card->isHidden()) << "Updates card must not be explicitly hidden by default";

    // ADR 0034: the slim card has an auto-check toggle + a status action button
    // (the unplanned Update-channel combo was removed).
    auto* auto_toggle = page.findChild<QWidget*>(QStringLiteral("updatesAutoCheckToggle"));
    EXPECT_NE(auto_toggle, nullptr) << "updatesAutoCheckToggle must exist";

    auto* action_btn = page.findChild<QPushButton*>(QStringLiteral("updatesActionButton"));
    EXPECT_NE(action_btn, nullptr) << "updatesActionButton must exist";

    auto* channel_combo = page.findChild<QComboBox*>(QStringLiteral("updatesChannelCombo"));
    EXPECT_EQ(channel_combo, nullptr) << "Update-channel combo must be gone (slim card)";
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

TEST_F(ConfigPageTest, PresetSaveAsButton_HasStableObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton")), nullptr);
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

    page.setPresetOptions({a, b}, QStringLiteral("preset_a"), false);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->count(), 2);
    EXPECT_EQ(combo->itemData(0).toString(), QStringLiteral("preset_a"));
    EXPECT_EQ(combo->itemData(1).toString(), QStringLiteral("preset_b"));
}

// S1-REDESIGN: the two badge visibility tests below are replaced by a no-badge assertion.
// The "default" state is now signalled only via a "★" suffix in the combo item text.
TEST_F(ConfigPageTest, SetPresetOptions_SelectedIsDefault_NoBadgeWidget) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption p;
    p.id = QStringLiteral("dflt");
    p.label = QStringLiteral("My Preset");

    page.setPresetOptions({p}, QStringLiteral("dflt"), false);

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

    page.setPresetOptions({dflt, other}, QStringLiteral("o"), false);

    // Badge widget was removed in S1-redesign.
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDefaultBadge")), nullptr)
        << "presetDefaultBadge must not exist (removed in S1-redesign)";
}

namespace {
// Helper: fetch a named action from the preset overflow ("…") menu by its text.
QAction* PresetMenuAction(ConfigPage& page, const QString& text) {
    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    if (!manage_btn || !manage_btn->menu())
        return nullptr;
    for (QAction* a : manage_btn->menu()->actions())
        if (a->text() == text)
            return a;
    return nullptr;
}
} // namespace

// Toolbar simplification: "Save as new…" is the only standalone button (shown while
// (changed)); Reset moved into the overflow menu and is enabled exactly while (changed).
TEST_F(ConfigPageTest, ChangedState_ShowsSaveAsNewButtonAndEnablesResetAction) {
    ConfigPage page(output_defaults_, video_defaults_);
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), /*dirty=*/false);

    auto* save_as = page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton"));
    auto* reset = PresetMenuAction(page, QStringLiteral("Reset"));
    ASSERT_NE(save_as, nullptr);
    ASSERT_NE(reset, nullptr);
    // The standalone Reset button is gone.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetResetButton")), nullptr);
    EXPECT_FALSE(save_as->isVisibleTo(&page));
    EXPECT_FALSE(reset->isEnabled());

    page.setPresetDirty(true);
    EXPECT_TRUE(save_as->isVisibleTo(&page));
    EXPECT_TRUE(reset->isEnabled());
}

// Delete is a menu action now: enabled for a user preset regardless of (changed),
// and disabled for a built-in.
TEST_F(ConfigPageTest, DeleteAction_UserPresetOnly_IndependentOfChanged) {
    ConfigPage page(output_defaults_, video_defaults_);
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});

    // The standalone Delete button is gone.
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetDeleteButton")), nullptr);

    page.setPresetOptions(opts, QStringLiteral("preset.abc"), /*dirty=*/false);
    auto* del = PresetMenuAction(page, QStringLiteral("Delete"));
    ASSERT_NE(del, nullptr);
    EXPECT_TRUE(del->isEnabled()); // clean user preset is deletable

    page.setPresetOptions(opts, QStringLiteral("preset.default"), /*dirty=*/true);
    EXPECT_FALSE(del->isEnabled()); // built-in: never
}

// The overflow menu holds every secondary action; the duplicate "Save as new…" entry is
// gone (it is the standalone button), and Rename is built-in-gated.
TEST_F(ConfigPageTest, OverflowMenu_HasSecondaryActions_RenameDisabledForBuiltIn) {
    ConfigPage page(output_defaults_, video_defaults_);
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), false);

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    ASSERT_NE(manage_btn->menu(), nullptr);
    QStringList texts;
    for (QAction* a : manage_btn->menu()->actions())
        if (!a->isSeparator())
            texts << a->text();
    EXPECT_EQ(texts, (QStringList() << QStringLiteral("Rename\xe2\x80\xa6") << QStringLiteral("Reset")
                                    << QStringLiteral("Delete") << QStringLiteral("Export\xe2\x80\xa6")
                                    << QStringLiteral("Import\xe2\x80\xa6")));
    // The overflow menu no longer duplicates "Save as new…".
    EXPECT_EQ(PresetMenuAction(page, QStringLiteral("Save as new\xe2\x80\xa6")), nullptr);
    // Rename stays intentionally disabled for a built-in preset.
    auto* rename = PresetMenuAction(page, QStringLiteral("Rename\xe2\x80\xa6"));
    ASSERT_NE(rename, nullptr);
    EXPECT_FALSE(rename->isEnabled());
}

// (changed) is a hint rendered in the combo text, not a warning widget.
TEST_F(ConfigPageTest, ChangedSuffix_AppendedToSelectedComboText) {
    ConfigPage page(output_defaults_, video_defaults_);
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.default"), false);
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("profileCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));
    page.setPresetDirty(true);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default (changed)"));
    page.setPresetDirty(false);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Default"));
}

// Removed affordances stay removed (guards against regressions).
TEST_F(ConfigPageTest, LegacyPresetControls_AreGone) {
    ConfigPage page(output_defaults_, video_defaults_);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetSaveButton")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetExportButton")), nullptr);
    EXPECT_EQ(page.findChild<QPushButton*>(QStringLiteral("presetImportButton")), nullptr);
    EXPECT_EQ(page.findChild<QLabel*>(QStringLiteral("presetDirtyIndicator")), nullptr);
}

// Production call site: the QInputDialog loops in onSavePresetAs/onRenamePreset
// validate through this exact predicate before emitting.
TEST_F(ConfigPageTest, PresetNameRejected_FoldsTrimsAndExcludesSelf) {
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Streaming"), false, false, true, {}});
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("  streaming "), opts, QString()));
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("default"), opts, QString()));
    EXPECT_TRUE(ConfigPage::presetNameRejected(QStringLiteral("   "), opts, QString()));
    EXPECT_FALSE(ConfigPage::presetNameRejected(QStringLiteral("Streaming"), opts, QStringLiteral("preset.abc")));
    EXPECT_FALSE(ConfigPage::presetNameRejected(QStringLiteral("Fresh"), opts, QString()));
}

TEST_F(ConfigPageTest, ComboSelection_EmitsPresetSelected) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption a;
    a.id = QStringLiteral("aa");
    a.label = QStringLiteral("AA");
    ConfigPage::ProfileOption b;
    b.id = QStringLiteral("bb");
    b.label = QStringLiteral("BB");
    page.setPresetOptions({a, b}, QStringLiteral("aa"), false);

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
    page.setPresetOptions({p}, QStringLiteral("p"), false);

    EXPECT_EQ(emit_count, 0) << "setPresetOptions must not emit presetSelected";
}

TEST_F(ConfigPageTest, SetRecordingControlsLocked_DisablesPresetActions) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Make the page dirty (Save as new / Reset) and select a user preset (Delete)
    // so the button + its menu actions are normally enabled.
    std::vector<ConfigPage::ProfileOption> opts;
    opts.push_back({QStringLiteral("preset.default"), QStringLiteral("Default"), true, false, true, {}});
    opts.push_back({QStringLiteral("preset.abc"), QStringLiteral("Mine"), false, false, true, {}});
    page.setPresetOptions(opts, QStringLiteral("preset.abc"), /*dirty=*/true);

    auto* save_as_btn = page.findChild<QPushButton*>(QStringLiteral("presetSaveAsButton"));
    auto* reset_action = PresetMenuAction(page, QStringLiteral("Reset"));
    auto* delete_action = PresetMenuAction(page, QStringLiteral("Delete"));
    ASSERT_NE(save_as_btn, nullptr);
    ASSERT_NE(reset_action, nullptr);
    ASSERT_NE(delete_action, nullptr);

    // Baseline: before lock, all three are enabled.
    EXPECT_TRUE(save_as_btn->isEnabled());
    EXPECT_TRUE(reset_action->isEnabled());
    EXPECT_TRUE(delete_action->isEnabled());

    page.setRecordingControlsLocked(true);

    EXPECT_FALSE(save_as_btn->isEnabled()) << "Save As button must be disabled when locked";
    EXPECT_FALSE(reset_action->isEnabled()) << "Reset action must be disabled when locked";
    EXPECT_FALSE(delete_action->isEnabled()) << "Delete action must be disabled when locked";

    auto* manage_btn = page.findChild<QToolButton*>(QStringLiteral("presetManageButton"));
    ASSERT_NE(manage_btn, nullptr);
    EXPECT_FALSE(manage_btn->isEnabled()) << "Manage button must be disabled when locked";
}

// ---- MP4 automatic-split gating (VR-005 / functional P2-005) -------------

TEST_F(ConfigPageTest, Mp4DisablesAutomaticSplitControlsWithHonestSummary) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Every30Min;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
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
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
    page.setOutputSettings(settings);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(combo->isEnabled());
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));

    auto* container = page.findChild<QComboBox*>(QStringLiteral("containerCombo"));
    ASSERT_NE(container, nullptr);
    container->setCurrentIndex(container->findData(static_cast<int>(capability::Container::Mp4)));
    EXPECT_FALSE(combo->isEnabled());
    // The configured mode is preserved while MP4 is selected (not reset to Off).
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));

    container->setCurrentIndex(container->findData(static_cast<int>(capability::Container::Matroska)));
    EXPECT_TRUE(combo->isEnabled());
    EXPECT_EQ(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Every15Min));
}

TEST_F(ConfigPageTest, Mp4HidesCustomSplitIntervalEditor) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Custom;
    settings.split.custom_minutes = 45;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
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

TEST_F(ConfigPageTest, SplitSizeModeToggle_ReplacesTheOffCustomCombo) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable

    // The old Off/Custom combo is gone; on/off is an ExoToggle.
    EXPECT_EQ(page.findChild<QComboBox*>(QStringLiteral("splitSizeModeCombo")), nullptr);
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitSizeModeToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isOn()); // default: split by size off
}

TEST_F(ConfigPageTest, SplitSizeMode_Off_HidesCustomSizeSpin) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.size_mode = SplitSizeMode::Off;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
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
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
    page.setOutputSettings(settings);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("splitCustomSizeSpin"));
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(spin->parentWidget(), nullptr);
    EXPECT_FALSE(spin->parentWidget()->isHidden());
    EXPECT_EQ(spin->value(), 512);
}

TEST_F(ConfigPageTest, Mp4_DisablesSplitSizeToggle) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Mp4;
    settings.split.size_mode = SplitSizeMode::Custom;
    settings.split.custom_size_mb = 1024;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable
    page.setOutputSettings(settings);

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitSizeModeToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isEnabled());
    // The persisted mode is preserved (Custom) even though MP4 cannot honour it.
    EXPECT_TRUE(toggle->isOn());
}

// ---- Split toggles: on/off maps to the SplitRecordingMode::Off enum, interval/size
//      editors only appear while the toggle is on (persistence model unchanged) --------

TEST_F(ConfigPageTest, SplitModeToggle_OffHidesIntervalRow_OnShowsIt) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.mode = SplitRecordingMode::Off;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    page.setOutputSettings(settings);

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitModeToggle"));
    auto* interval_row = page.findChild<QWidget*>(QStringLiteral("splitIntervalRow"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(interval_row, nullptr);
    EXPECT_FALSE(toggle->isOn());
    EXPECT_TRUE(interval_row->isHidden()) << "interval selector hidden while split is off";

    // Turning the toggle on selects a concrete interval and reveals the selector.
    toggle->setOn(true);
    EXPECT_FALSE(interval_row->isHidden());
    auto* combo = page.findChild<QComboBox*>(QStringLiteral("splitModeCombo"));
    ASSERT_NE(combo, nullptr);
    EXPECT_NE(combo->currentData().toInt(), static_cast<int>(SplitRecordingMode::Off));
}

TEST_F(ConfigPageTest, SplitModeToggle_OffEmitsModeOff) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.mode = SplitRecordingMode::Every30Min;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    page.setOutputSettings(settings);

    OutputSettingsModel emitted;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged,
                     [&emitted](const OutputSettingsModel& m) { emitted = m; });

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitModeToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isOn());
    toggle->setOn(false);
    EXPECT_EQ(emitted.split.mode, SplitRecordingMode::Off) << "toggle off persists as SplitRecordingMode::Off";
}

TEST_F(ConfigPageTest, SplitSizeToggle_OnEmitsCustom_OffEmitsOff) {
    OutputSettingsModel settings = output_defaults_;
    settings.container = capability::Container::Matroska;
    settings.split.size_mode = SplitSizeMode::Off;

    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    page.setOutputSettings(settings);

    OutputSettingsModel emitted;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged,
                     [&emitted](const OutputSettingsModel& m) { emitted = m; });

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitSizeModeToggle"));
    ASSERT_NE(toggle, nullptr);
    toggle->setOn(true);
    EXPECT_EQ(emitted.split.size_mode, SplitSizeMode::Custom);
    toggle->setOn(false);
    EXPECT_EQ(emitted.split.size_mode, SplitSizeMode::Off);
}

// ── SETTINGS-TIERS-P3: Presence + Appearance cards (moved from AdvancedPage) ──

TEST_F(ConfigPageTest, PresenceCard_CardTitleVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    // v0.9 polish: the card is titled "Notifications & overlays" (renamed from the
    // imprecise "Presence"); internal ids keep the presence_ prefix.
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Notifications & overlays")))
        << "Settings must contain the Notifications & overlays card title";
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Presence"))) << "the old \"Presence\" card title must be gone";
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

    // Expert mode off by default; the Developer card is built lazily on first
    // expert-enable, so by default it doesn't exist yet (which still means not shown).
    auto* card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    EXPECT_TRUE(card == nullptr || card->isHidden());
}

TEST_F(ConfigPageTest, DeveloperCard_VisibleWhenExpertModeEnabled) {
    ConfigPage page(output_defaults_, video_defaults_);

    page.setExpertModeEnabled(true);

    auto* card = page.findChild<QWidget*>(QStringLiteral("settingsDeveloperCard"));
    ASSERT_NE(card, nullptr) << "settingsDeveloperCard widget not found";
    EXPECT_FALSE(card->isHidden()) << "Developer card must not be hidden when expert mode is on";
}

// ── Cogwheels -> inline: ConfigPage integration — sub-controls still findable by objectName ───

TEST_F(ConfigPageTest, S5_MicPostProcessing_SubControlsStillFindableByObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // audio-expert subtree is built lazily on first enable

    // All four mic DSP controls must still exist (reparented into the disclosure content, not deleted).
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
    page.setExpertModeEnabled(true); // audio-expert subtree is built lazily on first enable

    EXPECT_NE(page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("limiterCheck")), nullptr)
        << "limiterCheck must still exist after S5 reparenting";
    EXPECT_NE(page.findChild<QDoubleSpinBox*>(QStringLiteral("limiterCeilingSpin")), nullptr)
        << "limiterCeilingSpin must still exist after S5 reparenting";
}

TEST_F(ConfigPageTest, ClockSlavingCheck_ExistsInExpertAudio) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // audio-expert subtree is built lazily on first enable

    auto* check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("clockSlavingCheck"));
    ASSERT_NE(check, nullptr) << "clockSlavingCheck must exist in the audio-expert section";
    EXPECT_TRUE(check->isChecked()) << "clock slaving defaults on";
}

TEST_F(ConfigPageTest, S5_SplitControls_StillFindableByObjectName) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // split controls are lazily built on expert-enable

    EXPECT_NE(page.findChild<QComboBox*>(QStringLiteral("splitModeCombo")), nullptr)
        << "splitModeCombo (interval selector) must still exist";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitModeToggle")), nullptr)
        << "splitModeToggle must exist";
    EXPECT_NE(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("splitSizeModeToggle")), nullptr)
        << "splitSizeModeToggle replaces the old splitSizeModeCombo";
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("splitCustomMinutesSpin")), nullptr)
        << "splitCustomMinutesSpin must still exist";
    EXPECT_NE(page.findChild<QSpinBox*>(QStringLiteral("splitCustomSizeSpin")), nullptr)
        << "splitCustomSizeSpin must still exist";
}

// ---------------------------------------------------------------------------
// Settings/Diagnostics polish — Slice 3: cogwheels -> inline (SettingsPopoverRow removed)
// ---------------------------------------------------------------------------

// No cogwheel popover button survives anywhere on the page — the three former gears
// (clock slaving, brickwall limiter, mic post-processing) are all flattened.
TEST_F(ConfigPageTest, CogwheelsInline_NoSettingsPopoverCogRemains) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // audio-expert subtree is built lazily on first enable

    EXPECT_EQ(page.findChild<QToolButton*>(QStringLiteral("settingsPopoverCog")), nullptr)
        << "no SettingsPopoverRow cog button should remain after the cogwheels -> inline slice";
}

// Brickwall limiter: the ceiling spin is a plain inline control (no popover) whose
// visibility tracks the limiter checkbox.
TEST_F(ConfigPageTest, BrickwallLimiter_CeilingSpinVisibilityTracksToggle) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("limiterCheck"));
    auto* ceiling = page.findChild<QDoubleSpinBox*>(QStringLiteral("limiterCeilingSpin"));
    ASSERT_NE(check, nullptr) << "limiterCheck must exist as a plain inline toggle";
    ASSERT_NE(ceiling, nullptr) << "limiterCeilingSpin must exist inline in the same row";

    EXPECT_TRUE(check->isChecked()) << "brickwall limiter defaults on";
    EXPECT_FALSE(ceiling->isHidden()) << "ceiling spin must be visible while the limiter is on";

    check->setChecked(false);
    EXPECT_TRUE(ceiling->isHidden()) << "ceiling spin must hide when the limiter is turned off";

    check->setChecked(true);
    EXPECT_FALSE(ceiling->isHidden()) << "ceiling spin must reappear when the limiter is turned back on";
}

// Microphone post-processing: the disclosure starts collapsed (stage rows hidden)
// and expanding it via the chevron button reveals the four stage controls in place.
TEST_F(ConfigPageTest, MicPostProcessing_DisclosureStartsCollapsedAndExpands) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* disclosure = page.findChild<QToolButton*>(QStringLiteral("micPostProcessingDisclosure"));
    auto* content = page.findChild<QWidget*>(QStringLiteral("micPostProcessingContent"));
    ASSERT_NE(disclosure, nullptr) << "the mic post-processing chevron disclosure button must exist";
    ASSERT_NE(content, nullptr) << "the mic post-processing content container must exist";

    EXPECT_FALSE(disclosure->isChecked()) << "the disclosure starts collapsed";
    EXPECT_TRUE(content->isHidden()) << "stage rows stay hidden until expanded";

    disclosure->setChecked(true);
    EXPECT_FALSE(content->isHidden()) << "stage rows must appear once the disclosure is expanded";

    // The four DSP stage controls are reparented into the content container, not deleted.
    EXPECT_NE(content->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micHpfCheck")), nullptr);
    EXPECT_NE(content->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micGateCheck")), nullptr);
    EXPECT_NE(content->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micAgcCheck")), nullptr);
    EXPECT_NE(content->findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micRnnoiseCheck")), nullptr);

    disclosure->setChecked(false);
    EXPECT_TRUE(content->isHidden()) << "collapsing again must hide the stage rows";
}

// The mic post-processing header shows a live "Off" / active-stage-list status,
// independent of whether the disclosure is expanded.
TEST_F(ConfigPageTest, MicPostProcessing_HeaderStatusReflectsActiveStages) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* status = page.findChild<QLabel*>(QStringLiteral("micPostProcessingStatus"));
    auto* hpf = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("micHpfCheck"));
    ASSERT_NE(status, nullptr);
    ASSERT_NE(hpf, nullptr);

    EXPECT_EQ(status->text(), QStringLiteral("Off")) << "status reads \"Off\" when every stage is disabled";

    hpf->setChecked(true);
    EXPECT_EQ(status->text(), QStringLiteral("High-pass")) << "status label must reflect the enabled HPF stage";
}

// Audio clock slaving keeps its explanatory info-i even as a plain inline row (a
// genuine on/off tradeoff: gentle correction vs byte-exact capture).
TEST_F(ConfigPageTest, ClockSlaving_PlainInlineRow_NoCog) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("clockSlavingCheck"));
    ASSERT_NE(check, nullptr);
    // Global absence of any popover cog is covered by CogwheelsInline_NoSettingsPopoverCogRemains;
    // here we just confirm the plain inline toggle still works end to end.
    check->setChecked(false);
    EXPECT_FALSE(check->isChecked());
    check->setChecked(true);
    EXPECT_TRUE(check->isChecked());
}

// ---------------------------------------------------------------------------
// 0.7.0 — S7: HEVC un-gating + video bit-depth control
// ---------------------------------------------------------------------------

// HEVC must be a normal, always-present codec choice (not behind a debug gate or
// labelled "(debug)"). GPU-verified end-to-end in S3.
TEST_F(ConfigPageTest, S7_VideoCodecCombo_IncludesHevcNonDebug) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* combo = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    ASSERT_NE(combo, nullptr);

    const int hevc_idx = combo->findData(static_cast<int>(capability::VideoCodec::HevcNvenc));
    ASSERT_GE(hevc_idx, 0) << "HEVC must be present in the video codec list";
    EXPECT_EQ(combo->itemText(hevc_idx), QStringLiteral("HEVC"))
        << "HEVC entry must be labelled \"HEVC\" (no \"(debug)\" suffix)";

    // AV1 and H.264 must also still be present.
    EXPECT_GE(combo->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)), 0);
    EXPECT_GE(combo->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)), 0);
}

// The superseded roadmap mockups for HEVC codec + bit depth must be gone; the real
// video bit-depth combo exists with 8-bit / 10-bit items.
TEST_F(ConfigPageTest, S7_VideoBitDepthControl_ExistsAndMockupsRemoved) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // expert format controls are lazily built on enable

    EXPECT_EQ(page.findChild<QComboBox*>(QStringLiteral("roadmapDummy_hevcCodec")), nullptr)
        << "the HEVC-codec mockup row is superseded by the real codec combo";
    EXPECT_EQ(page.findChild<QComboBox*>(QStringLiteral("roadmapDummy_bitDepth")), nullptr)
        << "the bit-depth mockup row is superseded by the real bit-depth combo";

    auto* depth = page.findChild<QComboBox*>(QStringLiteral("videoBitDepthCombo"));
    ASSERT_NE(depth, nullptr);
    EXPECT_GE(depth->findData(static_cast<int>(capability::BitDepth::Bit8)), 0);
    EXPECT_GE(depth->findData(static_cast<int>(capability::BitDepth::Bit10)), 0);
}

// 10-bit is selectable only for HEVC / AV1; for H.264 the 10-bit item is disabled.
TEST_F(ConfigPageTest, S7_TenBit_DisabledForH264_EnabledForHevcAv1) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* depth = page.findChild<QComboBox*>(QStringLiteral("videoBitDepthCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(depth, nullptr);

    const auto ten_item_enabled = [&]() -> bool {
        auto* model = qobject_cast<QStandardItemModel*>(depth->model());
        EXPECT_NE(model, nullptr);
        const int ten = depth->findData(static_cast<int>(capability::BitDepth::Bit10));
        EXPECT_GE(ten, 0);
        return model->item(ten)->isEnabled();
    };

    // Select H.264 → 10-bit disabled.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_FALSE(ten_item_enabled());

    // Select HEVC → 10-bit enabled.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_TRUE(ten_item_enabled());

    // Select AV1 → 10-bit enabled.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_TRUE(ten_item_enabled());
}

// Switching the codec from HEVC (10-bit) to H.264 snaps the bit depth back to 8-bit
// in the emitted model (mirrors the capability / reconcile fallback).
TEST_F(ConfigPageTest, S7_CodecChangeToH264_ResetsTenBitToEight) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* depth = page.findChild<QComboBox*>(QStringLiteral("videoBitDepthCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(depth, nullptr);

    // HEVC + 10-bit.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    depth->setCurrentIndex(depth->findData(static_cast<int>(capability::BitDepth::Bit10)));
    EXPECT_EQ(emitted.bit_depth, capability::BitDepth::Bit10);

    // Switch to H.264 → bit depth must reset to 8-bit.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_EQ(emitted.bit_depth, capability::BitDepth::Bit8);
    EXPECT_EQ(depth->currentData().toInt(), static_cast<int>(capability::BitDepth::Bit8));
}

// Chroma subsampling: the debug placeholder is superseded by a real expert combo
// with 4:2:0 (default) and 4:4:4 items; 4:2:2 is intentionally absent.
TEST_F(ConfigPageTest, ChromaControl_ExistsAndPlaceholderRemoved) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // expert format controls are lazily built on enable

    EXPECT_EQ(page.findChild<QComboBox*>(QStringLiteral("roadmapDummy_chromaSubsampling")), nullptr)
        << "the chroma mockup row is superseded by the real chroma combo";

    auto* chroma = page.findChild<QComboBox*>(QStringLiteral("videoChromaCombo"));
    ASSERT_NE(chroma, nullptr);
    EXPECT_GE(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs420)), 0);
    EXPECT_GE(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444)), 0);
    EXPECT_EQ(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs422)), -1)
        << "4:2:2 must not be offered (no NVENC 4:2:2 path)";
    // Default is 4:2:0.
    EXPECT_EQ(chroma->currentData().toInt(), static_cast<int>(capability::ChromaSubsampling::Cs420));
}

// 4:4:4 is selectable only for 8-bit H.264/HEVC: the item is disabled for AV1 and
// for 10-bit, and a stored 4:4:4 selection snaps back to 4:2:0 in the emitted model
// when the codec/bit-depth stops supporting it.
TEST_F(ConfigPageTest, Chroma444_GatedPerCodecAndBitDepth_SnapsBack) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted = output_defaults_;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* depth = page.findChild<QComboBox*>(QStringLiteral("videoBitDepthCombo"));
    auto* chroma = page.findChild<QComboBox*>(QStringLiteral("videoChromaCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(depth, nullptr);
    ASSERT_NE(chroma, nullptr);

    const auto item444_enabled = [&]() -> bool {
        auto* model = qobject_cast<QStandardItemModel*>(chroma->model());
        EXPECT_NE(model, nullptr);
        const int idx = chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444));
        EXPECT_GE(idx, 0);
        return model->item(idx)->isEnabled();
    };

    // H.264 8-bit → 4:4:4 selectable; selecting it reaches the model.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_TRUE(item444_enabled());
    chroma->setCurrentIndex(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444)));
    EXPECT_EQ(emitted.chroma_subsampling, capability::ChromaSubsampling::Cs444);

    // Switch to AV1 → item disabled and the selection snaps back to 4:2:0.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_FALSE(item444_enabled());
    EXPECT_EQ(emitted.chroma_subsampling, capability::ChromaSubsampling::Cs420);
    EXPECT_EQ(chroma->currentData().toInt(), static_cast<int>(capability::ChromaSubsampling::Cs420));

    // HEVC 8-bit → selectable again; then 10-bit disables it and snaps back.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_TRUE(item444_enabled());
    chroma->setCurrentIndex(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444)));
    EXPECT_EQ(emitted.chroma_subsampling, capability::ChromaSubsampling::Cs444);
    depth->setCurrentIndex(depth->findData(static_cast<int>(capability::BitDepth::Bit10)));
    EXPECT_FALSE(item444_enabled());
    EXPECT_EQ(emitted.chroma_subsampling, capability::ChromaSubsampling::Cs420);
}

// Once probed runtime capabilities arrive, the 4:4:4 gate consults the per-GPU
// CapabilitySet: a GPU that cannot do H.264 4:4:4 disables the item (naming the
// GPU as the reason) and snaps a previously-valid 4:4:4 selection back to 4:2:0.
TEST_F(ConfigPageTest, Chroma444_GatedByProbedGpuSupport_SnapsBackWithGpuReason) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* chroma = page.findChild<QComboBox*>(QStringLiteral("videoChromaCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(chroma, nullptr);

    auto* model = qobject_cast<QStandardItemModel*>(chroma->model());
    ASSERT_NE(model, nullptr);
    const int idx444 = chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444));
    ASSERT_GE(idx444, 0);
    const auto item444 = [&]() { return model->item(idx444); };

    // H.264 8-bit: 4:4:4 selectable under the static rule; select it.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_TRUE(item444()->isEnabled());
    chroma->setCurrentIndex(chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444)));
    EXPECT_EQ(chroma->currentData().toInt(), static_cast<int>(capability::ChromaSubsampling::Cs444));

    // Probe result: this GPU cannot encode H.264 4:4:4.
    auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.chroma444[capability::VideoCodec::H264Nvenc] = {capability::SupportLevel::NotImplemented, "GPU lacks YUV444"};
    page.setRuntimeCapabilities(caps);

    // Item is disabled, the tooltip names the GPU, and the selection snapped back.
    EXPECT_FALSE(item444()->isEnabled());
    EXPECT_TRUE(item444()->toolTip().contains(QStringLiteral("GPU")));
    EXPECT_EQ(chroma->currentData().toInt(), static_cast<int>(capability::ChromaSubsampling::Cs420));
}

// A GPU that DOES support 4:4:4 for the current codec keeps the item enabled,
// exactly as the static rule would.
TEST_F(ConfigPageTest, Chroma444_EnabledWhenProbedGpuSupportsIt) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* chroma = page.findChild<QComboBox*>(QStringLiteral("videoChromaCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(chroma, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(chroma->model());
    ASSERT_NE(model, nullptr);
    const int idx444 = chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444));
    ASSERT_GE(idx444, 0);

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));

    // Baseline advertises HEVC 4:4:4 as ValidUnvalidated → IsSelectable → enabled.
    auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    page.setRuntimeCapabilities(caps);
    EXPECT_TRUE(model->item(idx444)->isEnabled());
}

// Before any probe arrives, the gate falls back to the static codec/bit-depth
// rule unchanged: H.264 8-bit → 4:4:4 selectable.
TEST_F(ConfigPageTest, Chroma444_StaticRuleAppliesBeforeProbe) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* chroma = page.findChild<QComboBox*>(QStringLiteral("videoChromaCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(chroma, nullptr);
    auto* model = qobject_cast<QStandardItemModel*>(chroma->model());
    ASSERT_NE(model, nullptr);
    const int idx444 = chroma->findData(static_cast<int>(capability::ChromaSubsampling::Cs444));
    ASSERT_GE(idx444, 0);

    // No setRuntimeCapabilities() call — pre-probe behavior.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_TRUE(model->item(idx444)->isEnabled());
}

// Colour range: the combo exists with Full / Limited items and defaults to
// Limited (fix/color-range-signaling — common consumer players ignore the
// range flag and always expand limited->full, so Full looked permanently
// crushed there; Full remains available as an opt-in).
TEST_F(ConfigPageTest, ColorRangeControl_ExistsWithFullAndLimited) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // expert format controls are lazily built on enable

    auto* range = page.findChild<QComboBox*>(QStringLiteral("videoColorRangeCombo"));
    ASSERT_NE(range, nullptr);
    EXPECT_GE(range->findData(static_cast<int>(capability::ColorRange::Full)), 0);
    EXPECT_GE(range->findData(static_cast<int>(capability::ColorRange::Limited)), 0);
    // Default is Limited.
    EXPECT_EQ(range->currentData().toInt(), static_cast<int>(capability::ColorRange::Limited));
}

// Selecting Limited emits the colour range in the model; it is never codec-gated
// (works for H.264, which cannot take 10-bit).
TEST_F(ConfigPageTest, ColorRange_SelectingLimited_EmitsModel_NotGated) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted = output_defaults_;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* range = page.findChild<QComboBox*>(QStringLiteral("videoColorRangeCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(range, nullptr);

    // Even with H.264 (no 10-bit), colour range remains fully selectable.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    range->setCurrentIndex(range->findData(static_cast<int>(capability::ColorRange::Limited)));
    EXPECT_EQ(emitted.color_range, capability::ColorRange::Limited);
    EXPECT_TRUE(range->isEnabled());

    // Back to Full.
    range->setCurrentIndex(range->findData(static_cast<int>(capability::ColorRange::Full)));
    EXPECT_EQ(emitted.color_range, capability::ColorRange::Full);
}

// NVENC-PRESET-R1: the encoder-preset combo exists with all seven P1..P7 items
// and defaults to P4 (balanced) — replaces the Debug-only roadmapDummy_encoderPreset.
TEST_F(ConfigPageTest, EncoderPresetControl_ExistsWithAllSevenPresets_DefaultsToP4) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // expert format controls are lazily built on enable

    auto* preset = page.findChild<QComboBox*>(QStringLiteral("videoEncoderPresetCombo"));
    ASSERT_NE(preset, nullptr);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P1)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P2)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P3)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P4)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P5)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P6)), 0);
    EXPECT_GE(preset->findData(static_cast<int>(recorder_core::NvencPreset::P7)), 0);
    // Default is P4.
    EXPECT_EQ(preset->currentData().toInt(), static_cast<int>(recorder_core::NvencPreset::P4));

    // Debug dummy row (roadmapDummy_encoderPreset) must be gone — this is now a real control.
    EXPECT_EQ(page.findChild<QComboBox*>(QStringLiteral("roadmapDummy_encoderPreset")), nullptr);
}

// Selecting a preset emits the model; it is never codec-gated (works for H.264,
// which previously hardcoded P6 with no user control).
TEST_F(ConfigPageTest, EncoderPreset_SelectingP7_EmitsModel_NotGated) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted = output_defaults_;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* preset = page.findChild<QComboBox*>(QStringLiteral("videoEncoderPresetCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(preset, nullptr);

    // Even with H.264 (previously hardcoded to P6), the preset remains fully selectable.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    preset->setCurrentIndex(preset->findData(static_cast<int>(recorder_core::NvencPreset::P7)));
    EXPECT_EQ(emitted.nvenc_preset, recorder_core::NvencPreset::P7);
    EXPECT_TRUE(preset->isEnabled());

    // Back to P1.
    preset->setCurrentIndex(preset->findData(static_cast<int>(recorder_core::NvencPreset::P1)));
    EXPECT_EQ(emitted.nvenc_preset, recorder_core::NvencPreset::P1);
}

// ── ADR 0035 Slice 2: Frame pacing select (Smooth / Newest) ─────────────────

TEST_F(ConfigPageTest, FramePacingSelectReflectsAndSetsModel) {
    // framePacingSelect must exist in the Expert Video (fmt_expert_section_), which is
    // built lazily on first expert-enable.
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* sel = page.findChild<QComboBox*>(QStringLiteral("framePacingSelect"));
    ASSERT_NE(sel, nullptr) << "framePacingSelect must exist in Expert Video";

    // Default model has Smooth; control must reflect that.
    EXPECT_EQ(sel->currentData().toInt(), static_cast<int>(recorder_core::FramePacingMode::Smooth));

    // Apply model with Newest → control updates without emitting.
    VideoSettingsModel newest_model = video_defaults_;
    newest_model.frame_pacing = recorder_core::FramePacingMode::Newest;
    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged,
                     [&emit_count](const VideoSettingsModel&) { ++emit_count; });
    page.setVideoSettings(newest_model);
    EXPECT_EQ(sel->currentData().toInt(), static_cast<int>(recorder_core::FramePacingMode::Newest));
    EXPECT_EQ(emit_count, 0) << "setVideoSettings must not emit videoSettingsChanged";

    // Toggle back to Smooth via the combo → videoSettingsChanged emits Smooth.
    VideoSettingsModel emitted;
    bool emitted_flag = false;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged, [&](const VideoSettingsModel& s) {
        emitted = s;
        emitted_flag = true;
    });
    const int smooth_idx = sel->findData(static_cast<int>(recorder_core::FramePacingMode::Smooth));
    ASSERT_GE(smooth_idx, 0);
    sel->setCurrentIndex(smooth_idx);
    EXPECT_TRUE(emitted_flag);
    EXPECT_EQ(emitted.frame_pacing, recorder_core::FramePacingMode::Smooth);
}

// ── HDR handling control (replaces roadmapDummy_hdr10) ──────────────────────

// The real HDR-handling combo exists with exactly the two user-facing choices
// (Tone-map to SDR default, Record native HDR10); HdrMode::Off is intentionally
// never offered. The roadmap mockup is gone.
TEST_F(ConfigPageTest, HdrModeControl_ExistsWithTonemapAndHdr10_NoOff) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // expert format controls are lazily built on enable

    EXPECT_EQ(page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("roadmapDummy_hdr10")), nullptr)
        << "the HDR10 mockup toggle is superseded by the real HDR-handling combo";

    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(hdr, nullptr);
    EXPECT_GE(hdr->findData(static_cast<int>(recorder_core::HdrMode::TonemapSdr)), 0);
    EXPECT_GE(hdr->findData(static_cast<int>(recorder_core::HdrMode::Hdr10)), 0);
    EXPECT_LT(hdr->findData(static_cast<int>(recorder_core::HdrMode::Off)), 0)
        << "HdrMode::Off must never be offered in the UI";

    // Default model has TonemapSdr; control must reflect that.
    EXPECT_EQ(hdr->currentData().toInt(), static_cast<int>(recorder_core::HdrMode::TonemapSdr));
}

// Hdr10 is selectable only for HEVC / AV1 (capability::QueryHdr10Native); for
// H.264 the item is disabled, mirroring the bit-depth disabled-item pattern.
TEST_F(ConfigPageTest, HdrMode_DisabledForH264_EnabledForHevcAv1) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(hdr, nullptr);

    const auto hdr10_item_enabled = [&]() -> bool {
        auto* model = qobject_cast<QStandardItemModel*>(hdr->model());
        EXPECT_NE(model, nullptr);
        const int idx = hdr->findData(static_cast<int>(recorder_core::HdrMode::Hdr10));
        EXPECT_GE(idx, 0);
        return model->item(idx)->isEnabled();
    };

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_FALSE(hdr10_item_enabled());

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_TRUE(hdr10_item_enabled());

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_TRUE(hdr10_item_enabled());
}

// Selecting Record native HDR10 with a capable codec emits the model.
TEST_F(ConfigPageTest, HdrMode_SelectingHdr10WithAv1_EmitsModel) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted = output_defaults_;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(hdr, nullptr);

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    hdr->setCurrentIndex(hdr->findData(static_cast<int>(recorder_core::HdrMode::Hdr10)));
    EXPECT_EQ(emitted.hdr_mode, recorder_core::HdrMode::Hdr10);

    hdr->setCurrentIndex(hdr->findData(static_cast<int>(recorder_core::HdrMode::TonemapSdr)));
    EXPECT_EQ(emitted.hdr_mode, recorder_core::HdrMode::TonemapSdr);
}

// Unlike bit depth, switching the codec to H.264 while Hdr10 is selected must NOT
// silently rewrite the stored setting back to TonemapSdr: the live pre-flight
// blocker (rec.hdr.h264) owns that conflict at recording time. The control shows
// the conflict (disabled item + calm inline hint) without discarding the choice.
TEST_F(ConfigPageTest, HdrMode_CodecChangeToH264_DoesNotResetHdr10) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    OutputSettingsModel emitted = output_defaults_;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged, &page,
                     [&emitted](const OutputSettingsModel& s) { emitted = s; });

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(hdr, nullptr);

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    hdr->setCurrentIndex(hdr->findData(static_cast<int>(recorder_core::HdrMode::Hdr10)));
    ASSERT_EQ(emitted.hdr_mode, recorder_core::HdrMode::Hdr10);

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_EQ(emitted.hdr_mode, recorder_core::HdrMode::Hdr10)
        << "HDR mode must survive a codec change to H.264 -- the pre-flight blocker "
           "handles the conflict, the UI must not silently discard the choice";
    EXPECT_EQ(hdr->currentData().toInt(), static_cast<int>(recorder_core::HdrMode::Hdr10));
}

// The calm inline hint ("Not available with H.264...") is visible only while
// H.264 disables Hdr10, and hides again once a capable codec is selected. The
// HDR row itself is relevance-gated on an HDR-active display, so the test
// first delivers display facts that keep the row (and thus its hint) shown.
TEST_F(ConfigPageTest, HdrMode_H264Hint_VisibleOnlyWhenDisabled) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    capability::DisplayHdrFacts hdr_display;
    hdr_display.name = "\\\\.\\DISPLAY1";
    hdr_display.hdr_active = true;
    caps.runtime.displays.push_back(hdr_display);
    page.setRuntimeCapabilities(caps);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    ASSERT_NE(codec, nullptr);

    // The page is never shown() in this headless test, so QWidget::isVisible()
    // would be false regardless of the control's own setVisible() calls (it ANDs
    // in ancestor/top-level shown state). isHidden() reflects only this widget's
    // own explicit visibility flag, which is what updateVideoHdrModeControl() drives.
    const auto hint_visible = [&]() {
        for (const auto* label : page.findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("Not available with H.264")))
                return !label->isHidden();
        }
        return false;
    };

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_FALSE(hint_visible());

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_TRUE(hint_visible());

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_FALSE(hint_visible());
}

// Hydrate-replay: a setting arriving via the constructor (before any lazy build
// runs) must still be reflected once the widgets exist -- mirrors the historical
// colour-range/encoder-preset hydration-bug class.
TEST_F(ConfigPageTest, HdrMode_HydratesFromConstructorSettings) {
    OutputSettingsModel initial = output_defaults_;
    initial.video_codec = capability::VideoCodec::Av1Nvenc;
    initial.hdr_mode = recorder_core::HdrMode::Hdr10;

    ConfigPage page(initial, video_defaults_);
    // The HDR combo is built lazily on first expert-enable; the constructor setting
    // must survive the wait and land once the widget exists (hydration replay).
    page.setExpertModeEnabled(true);
    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->currentData().toInt(), static_cast<int>(recorder_core::HdrMode::Hdr10));
}

// setOutputSettings() must carry hdr_mode through to the combo without emitting
// formatSettingsChanged -- the same field-drop bug class MergeFormatSelection was
// created to prevent (color_range / bit_depth / nvenc_preset all had it before).
TEST_F(ConfigPageTest, HdrMode_SetOutputSettings_HydratesWithoutEmitting) {
    ConfigPage page(output_defaults_, video_defaults_);

    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::formatSettingsChanged,
                     [&emit_count](const OutputSettingsModel&) { ++emit_count; });

    OutputSettingsModel incoming = output_defaults_;
    incoming.video_codec = capability::VideoCodec::HevcNvenc;
    incoming.hdr_mode = recorder_core::HdrMode::Hdr10;
    page.setOutputSettings(incoming);

    // The HDR combo is built lazily; enabling expert mode builds it and re-seeds from
    // the settings applied above. Neither setOutputSettings nor the lazy build may emit.
    page.setExpertModeEnabled(true);
    auto* hdr = page.findChild<QComboBox*>(QStringLiteral("videoHdrModeCombo"));
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->currentData().toInt(), static_cast<int>(recorder_core::HdrMode::Hdr10));
    EXPECT_EQ(emit_count, 0) << "setOutputSettings must not emit formatSettingsChanged";
}

// ── v0.9 polish: expert-view relevance gating ────────────────────────────────
// Expert rows exist only when the active codec/GPU/display can actually use
// them; a gated-out row is hidden (no layout gap), never merely disabled.

// The HDR-handling row is hidden until the probed display facts contain an
// HDR-active display; SDR-only facts keep it hidden, an HDR-active display
// reveals it. Pre-probe (no facts yet) counts as "no HDR display".
TEST_F(ConfigPageTest, HdrRow_HiddenWithoutHdrActiveDisplay_ShownWithOne) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* row = page.findChild<QWidget*>(QStringLiteral("videoHdrModeRow"));
    ASSERT_NE(row, nullptr);

    // Pre-probe: no display facts → hidden.
    EXPECT_TRUE(row->isHidden()) << "HDR row must stay hidden before display facts arrive";

    // SDR-only facts → still hidden.
    auto sdr_caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    capability::DisplayHdrFacts sdr_display;
    sdr_display.name = "\\\\.\\DISPLAY1";
    sdr_display.hdr_active = false;
    sdr_caps.runtime.displays.push_back(sdr_display);
    page.setRuntimeCapabilities(sdr_caps);
    EXPECT_TRUE(row->isHidden()) << "HDR row must stay hidden on an SDR-only system";

    // One HDR-active display among several → shown.
    auto hdr_caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    hdr_caps.runtime.displays.push_back(sdr_display);
    capability::DisplayHdrFacts hdr_display;
    hdr_display.name = "\\\\.\\DISPLAY2";
    hdr_display.hdr_active = true;
    hdr_caps.runtime.displays.push_back(hdr_display);
    page.setRuntimeCapabilities(hdr_caps);
    EXPECT_FALSE(row->isHidden()) << "HDR row must appear once an HDR-active display is detected";
}

// While the HDR row is gated out, its H.264 hint is gated out with it — the
// hint narrates a row that is not there otherwise.
TEST_F(ConfigPageTest, HdrRow_GatedOut_TakesTheH264HintWithIt) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    ASSERT_NE(codec, nullptr);
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));

    // No HDR display known → neither the row nor the hint may show.
    for (const auto* label : page.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("Not available with H.264")))
            EXPECT_TRUE(label->isHidden()) << "the H.264 hint must not show while the HDR row is gated out";
    }
}

// The Bit depth row exists only when the selected codec carries 10-bit at all
// (HEVC/AV1); for 8-bit-only H.264 the whole row is hidden and the stored value
// has snapped back to 8-bit (no stale 10-bit state behind the gate).
TEST_F(ConfigPageTest, BitDepthRow_HiddenForH264_ShownForHevcAv1) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* row = page.findChild<QWidget*>(QStringLiteral("videoBitDepthRow"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(row, nullptr);

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::H264Nvenc)));
    EXPECT_TRUE(row->isHidden()) << "Bit depth row must be hidden for 8-bit-only H.264";

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_FALSE(row->isHidden()) << "Bit depth row must be shown for HEVC";

    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_FALSE(row->isHidden()) << "Bit depth row must be shown for AV1";
}

// The Chroma row exists only when the selected codec + active GPU can carry
// 4:4:4 at all: hidden for AV1 (no NVENC 4:4:4 path) and hidden once a probe
// reports the GPU cannot encode YUV444 for the selected codec. A 10-bit
// bit-depth conflict keeps the row visible (disabled item + hint) because it
// is user-fixable in place.
TEST_F(ConfigPageTest, ChromaRow_HiddenForAv1AndGpuWithout444_VisibleFor10BitConflict) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* codec = page.findChild<QComboBox*>(QStringLiteral("videoCodecCombo"));
    auto* depth = page.findChild<QComboBox*>(QStringLiteral("videoBitDepthCombo"));
    auto* row = page.findChild<QWidget*>(QStringLiteral("videoChromaRow"));
    ASSERT_NE(codec, nullptr);
    ASSERT_NE(depth, nullptr);
    ASSERT_NE(row, nullptr);

    // AV1 (default) → no 4:4:4 path at all → row hidden.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::Av1Nvenc)));
    EXPECT_TRUE(row->isHidden()) << "Chroma row must be hidden for AV1 (4:2:0 only)";

    // HEVC 8-bit → row shown.
    codec->setCurrentIndex(codec->findData(static_cast<int>(capability::VideoCodec::HevcNvenc)));
    EXPECT_FALSE(row->isHidden()) << "Chroma row must be shown for HEVC on a capable GPU";

    // HEVC 10-bit → conflict is user-fixable (switch bit depth) → row stays.
    depth->setCurrentIndex(depth->findData(static_cast<int>(capability::BitDepth::Bit10)));
    EXPECT_FALSE(row->isHidden()) << "a 10-bit conflict must not hide the Chroma row";
    depth->setCurrentIndex(depth->findData(static_cast<int>(capability::BitDepth::Bit8)));

    // Probe says this GPU has no YUV444 encode for HEVC → row hidden.
    auto caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    caps.chroma444[capability::VideoCodec::HevcNvenc] = {capability::SupportLevel::NotImplemented, "GPU lacks YUV444"};
    page.setRuntimeCapabilities(caps);
    EXPECT_TRUE(row->isHidden()) << "Chroma row must be hidden when the active GPU cannot encode 4:4:4";
}

// ── v0.9 polish: regroup — Frame pacing lives in Quality & timing ───────────

// Frame pacing is a timing control: its row must sit in the Quality & timing
// card (a sibling of the frame-rate rows), not inside the Container & codecs
// expert section, and it follows the Expert toggle like the other expert rows.
TEST_F(ConfigPageTest, FramePacingRow_LivesInQualityCard_ExpertGated) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* row = page.findChild<QWidget*>(QStringLiteral("framePacingRow"));
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->isHidden()) << "Frame pacing row must be visible in expert mode";

    // Not a descendant of the Container & codecs expert section.
    auto* fmt_expert = page.findChild<QWidget*>(QStringLiteral("fmtExpertSection"));
    ASSERT_NE(fmt_expert, nullptr);
    EXPECT_EQ(fmt_expert->findChild<QComboBox*>(QStringLiteral("framePacingSelect")), nullptr)
        << "Frame pacing must have moved out of the Container & codecs expert section";
    EXPECT_FALSE(fmt_expert->isAncestorOf(row));

    // Hosted by the Quality & timing card: the row's direct parent is the card
    // widget that also (transitively) hosts the frame-rate combo.
    auto* frame_rate = page.findChild<QComboBox*>(QStringLiteral("frameRateCombo"));
    ASSERT_NE(frame_rate, nullptr);
    ASSERT_NE(row->parentWidget(), nullptr);
    EXPECT_TRUE(row->parentWidget()->isAncestorOf(frame_rate))
        << "Frame pacing row must be hosted by the Quality & timing card";

    // Leaves with the Expert toggle.
    page.setExpertModeEnabled(false);
    EXPECT_TRUE(row->isHidden()) << "Frame pacing row must hide when Expert mode turns off";
}

// Destroying a shown page used to abort: WebcamSetupPanel::hideEvent stops its preview and
// relays previewActiveRequested into ConfigPage, but by teardown time the receiver is no
// longer a ConfigPage. Reaching the end of this test without aborting is the assertion.
TEST_F(ConfigPageTest, DestroyingAShownPageDoesNotDispatchOntoTheHalfDestroyedPage) {
    {
        ConfigPage page(output_defaults_, video_defaults_);
        page.show();
        QCoreApplication::processEvents();
        ASSERT_NE(page.findChild<ui::widgets::WebcamSetupPanel*>(), nullptr);
    }
    QCoreApplication::processEvents();
    SUCCEED();
}

// --- Expert-mode CQ precision row -------------------------------------------------
// The row carries the CQ spin box; the named tiers stay on the segmented control above
// it, so the row itself must not restate a tier.
TEST_F(ConfigPageTest, CqRow_HasInfoHintAndNoTierLabel) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* row = page.findChild<QWidget*>(QStringLiteral("qualityExpertWidget"));
    ASSERT_NE(row, nullptr);
    EXPECT_NE(row->findChild<ui::widgets::InfoHintIcon*>(QStringLiteral("qualityCqInfoHint")), nullptr);

    for (const auto* label : row->findChildren<QLabel*>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("High")));
        EXPECT_FALSE(label->text().contains(QStringLiteral("Balanced")));
        EXPECT_FALSE(label->text().contains(QStringLiteral("Small")));
    }
}

// A CQ that lands between tiers still highlights the tier it is closest to.
TEST_F(ConfigPageTest, CqSpinBox_SegmentSelectionFollowsNearestPreset) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("qualityCqSpin"));
    auto* high_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentHigh"));
    auto* small_segment = page.findChild<QPushButton*>(QStringLiteral("qualitySegmentSmall"));
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(high_segment, nullptr);
    ASSERT_NE(small_segment, nullptr);

    spin->setValue(20); // nearest canonical tier is High (19)
    EXPECT_EQ(recorder_core::NearestQualityPreset(20), recorder_core::NvencQualityPreset::High);
    EXPECT_TRUE(high_segment->property("qualitySegmentSelected").toBool());

    spin->setValue(29); // nearest canonical tier is Small (30)
    EXPECT_TRUE(small_segment->property("qualitySegmentSelected").toBool());
}

// A non-canonical CQ is never snapped onto a tier; it reaches the model verbatim.
TEST_F(ConfigPageTest, CqSpinBox_KeepsNonCanonicalValuesAndReachesTheModel) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    VideoSettingsModel changed;
    int emit_count = 0;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged, [&](const VideoSettingsModel& s) {
        ++emit_count;
        changed = s;
    });

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("qualityCqSpin"));
    ASSERT_NE(spin, nullptr);

    spin->setValue(16);
    EXPECT_FALSE(recorder_core::IsCanonicalCq(16));
    EXPECT_EQ(emit_count, 1);
    EXPECT_EQ(changed.cq, 16u);
    EXPECT_EQ(spin->value(), 16) << "the value must survive, not snap to a tier";
}

// Scrolling the settings page must not silently retune quality.
TEST_F(ConfigPageTest, CqSpinBox_IgnoresWheelUnlessFocused) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("qualityCqSpin"));
    ASSERT_NE(spin, nullptr);
    spin->setValue(24);
    ASSERT_FALSE(spin->hasFocus());

    QWheelEvent wheel(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(spin, &wheel);
    EXPECT_EQ(spin->value(), 24) << "unfocused wheel must not change the value";

    // The wheel is swallowed, not merely unhandled.
    EXPECT_FALSE(wheel.isAccepted());

    // The counterpart matters: without it the test would also pass if the wheel were
    // ignored unconditionally. Focus needs an active window.
    page.show();
    page.activateWindow();
    QCoreApplication::processEvents();
    spin->setFocus(Qt::MouseFocusReason);
    ASSERT_TRUE(spin->hasFocus()) << "cannot verify the focused branch without focus";

    QWheelEvent focused(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(spin, &focused);
    EXPECT_NE(spin->value(), 24) << "focused wheel steps the value";
}

// The CQ input sits in the same column as every other settings-row input.
TEST_F(ConfigPageTest, CqSpinBox_MatchesTheRowInputColumnWidth) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* spin = page.findChild<QSpinBox*>(QStringLiteral("qualityCqSpin"));
    auto* container = page.findChild<QComboBox*>(QStringLiteral("containerCombo"));
    ASSERT_NE(spin, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_GT(container->width(), 0) << "a zero reference width would make this test vacuous";
    EXPECT_EQ(spin->width(), container->width());
}

// ── Audio source row order + merge label (product-spec §5) ───────────────────

TEST_F(ConfigPageTest, SettingsAudio_MergeControlUsesDocumentedLabel) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The per-row merge control carries the exact spec label "Merge with above".
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Merge with above")))
        << "Settings Audio rows must label the merge control 'Merge with above'";
    // The old inverted "Separate track" label must be gone.
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Separate track")))
        << "The inverted 'Separate track' label must no longer appear";
}

TEST_F(ConfigPageTest, SettingsAudio_RowsFollowAppSysMicOrder) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Window target makes the App row visible so all three rows are laid out.
    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Window;
    state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Sys, true, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
    };
    page.setAudioUiState(state);

    page.resize(1280, 900);
    page.show();
    QCoreApplication::processEvents();

    auto* app_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioAppCheck"));
    auto* sys_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioSysCheck"));
    auto* mic_check = page.findChild<ui::widgets::ExoCheckBox*>(QStringLiteral("settingsAudioMicCheck"));
    ASSERT_NE(app_check, nullptr);
    ASSERT_NE(sys_check, nullptr);
    ASSERT_NE(mic_check, nullptr);
    ASSERT_TRUE(AppSectionVisible(page)) << "App row must be visible for a Window target";

    const int app_y = app_check->mapToGlobal(QPoint(0, 0)).y();
    const int sys_y = sys_check->mapToGlobal(QPoint(0, 0)).y();
    const int mic_y = mic_check->mapToGlobal(QPoint(0, 0)).y();
    EXPECT_LT(app_y, sys_y) << "APP row must sit above SYS row";
    EXPECT_LT(sys_y, mic_y) << "SYS row must sit above MIC row";
}

TEST_F(ConfigPageTest, SettingsAudio_MergeToggleReflectsAndSetsMergeWithAbove) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Window target so the App and Mic rows both carry a merge control. The Mic
    // row starts merged (merge_with_above=true); Sys starts separate.
    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Window;
    state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Sys, true, false},
        {recorder_core::AudioSourceKind::Mic, true, true},
    };
    page.setAudioUiState(state);

    auto* mic_merge = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("settingsAudioMicMerge"));
    auto* sys_merge = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("settingsAudioSysMerge"));
    ASSERT_NE(mic_merge, nullptr);
    ASSERT_NE(sys_merge, nullptr);

    // Sync direction: toggle-on == merged. A merged Mic row shows the toggle on; a
    // separate Sys row shows it off.
    EXPECT_TRUE(mic_merge->isChecked()) << "A merged row must show 'Merge with above' on";
    EXPECT_FALSE(sys_merge->isChecked()) << "A separate row must show 'Merge with above' off";

    // Emit direction: turning the Mic toggle off must clear merge_with_above.
    capability::AudioUiState emitted;
    bool got = false;
    QObject::connect(&page, &ConfigPage::audioSettingsChanged, [&](const capability::AudioUiState& s) {
        emitted = s;
        got = true;
    });
    mic_merge->setChecked(false);
    ASSERT_TRUE(got) << "toggling the merge control must emit audioSettingsChanged";
    bool saw_mic = false;
    for (const auto& row : emitted.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic) {
            saw_mic = true;
            EXPECT_FALSE(row.merge_with_above) << "toggle-off must set merge_with_above=false";
        }
    }
    EXPECT_TRUE(saw_mic) << "emitted state must carry the Mic row";
}

// ── Crash-report auto-send consent toggle (Developer/Advanced card) ────────────

TEST_F(ConfigPageTest, CrashReportsToggle_HiddenBeforeExpertMode) {
    ConfigPage page(output_defaults_, video_defaults_);

    // The Developer/Advanced card (and its crash-report toggle) is built lazily
    // on first expert-enable, so by default it doesn't exist yet.
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    EXPECT_EQ(toggle, nullptr) << "crashReportsAutoSendToggle must not exist before expert mode is enabled";
}

TEST_F(ConfigPageTest, CrashReportsToggle_VisibleAndLabeledWhenExpertModeEnabled) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true); // Developer card is lazily built on first expert-enable

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    ASSERT_NE(toggle, nullptr) << "crashReportsAutoSendToggle must exist once expert mode is enabled";
    EXPECT_FALSE(toggle->isHidden());
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Send crash reports automatically")))
        << "toggle row must carry the plain-language label";
}

TEST_F(ConfigPageTest, CrashReportsToggle_DefaultsOff) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);

    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_FALSE(toggle->isChecked()) << "consent defaults to off, matching auto_send_crash_reports' default";
}

TEST_F(ConfigPageTest, CrashReportsToggle_SetterAppliesBeforeAndAfterLazyBuild) {
    ConfigPage page(output_defaults_, video_defaults_);

    // Seed the persisted "on" state before the Developer card exists -- it must
    // be remembered and applied once the card is lazily constructed (same
    // contract as setDeveloperLogLevel).
    page.setAutoSendCrashReports(true);

    page.setExpertModeEnabled(true);
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isChecked()) << "the pre-seeded consent state must survive the lazy build";

    // Once the card exists, the setter must also apply immediately.
    page.setAutoSendCrashReports(false);
    EXPECT_FALSE(toggle->isChecked());
}

TEST_F(ConfigPageTest, CrashReportsToggle_TurningOnEmitsSignalTrue) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setExpertModeEnabled(true);
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_FALSE(toggle->isChecked());

    bool got = false;
    bool emitted_value = false;
    QObject::connect(&page, &ConfigPage::autoSendCrashReportsToggled, [&](bool enabled) {
        got = true;
        emitted_value = enabled;
    });
    toggle->setChecked(true);
    ASSERT_TRUE(got) << "turning the toggle on must emit autoSendCrashReportsToggled";
    EXPECT_TRUE(emitted_value);
}

TEST_F(ConfigPageTest, CrashReportsToggle_TurningOffEmitsSignalFalse) {
    ConfigPage page(output_defaults_, video_defaults_);
    page.setAutoSendCrashReports(true);
    page.setExpertModeEnabled(true);
    auto* toggle = page.findChild<ui::widgets::ExoToggle*>(QStringLiteral("crashReportsAutoSendToggle"));
    ASSERT_NE(toggle, nullptr);
    ASSERT_TRUE(toggle->isChecked());

    bool got = false;
    bool emitted_value = true;
    QObject::connect(&page, &ConfigPage::autoSendCrashReportsToggled, [&](bool enabled) {
        got = true;
        emitted_value = enabled;
    });
    toggle->setChecked(false);
    ASSERT_TRUE(got) << "turning the toggle off must emit autoSendCrashReportsToggled";
    EXPECT_FALSE(emitted_value);
}

} // namespace
} // namespace exosnap
