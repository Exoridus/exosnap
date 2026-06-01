#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>

#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "models/WebcamSettings.h"
#include "pages/ConfigPage.h"

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

TEST_F(ConfigPageTest, ContainerRadiosExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto radios = page.findChildren<QRadioButton*>();
    EXPECT_GE(radios.size(), 3);
}

TEST_F(ConfigPageTest, VideoQualityComboExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto combos = page.findChildren<QComboBox*>();
    EXPECT_GE(combos.size(), 2);
}

TEST_F(ConfigPageTest, QualityCardsExist_WithStableObjectNames) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualityCardBalanced")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualityCardSmall")), nullptr);
    EXPECT_NE(page.findChild<QPushButton*>(QStringLiteral("qualityCardCustom")), nullptr);
}

TEST_F(ConfigPageTest, AudioSourceCheckboxesExist) {
    ConfigPage page(output_defaults_, video_defaults_);

    const auto checks = page.findChildren<QCheckBox*>();
    EXPECT_GE(checks.size(), 6);
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

TEST_F(ConfigPageTest, PresetAndFormatSectionTitle_IsVisible) {
    ConfigPage page(output_defaults_, video_defaults_);

    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Preset & Format")));
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Profile & Format")));
}

TEST_F(ConfigPageTest, BuiltInAndModifiedStates_UsePresetCopy) {
    ConfigPage page(output_defaults_, video_defaults_);

    ConfigPage::ProfileOption builtin;
    builtin.id = QStringLiteral("builtin");
    builtin.label = QStringLiteral("High Quality AV1");
    builtin.built_in = true;
    builtin.modified = false;

    std::vector<ConfigPage::ProfileOption> options{builtin};
    page.setProfileOptions(options, builtin.id, false);
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Built-in preset")));

    options[0].modified = true;
    page.setProfileOptions(options, builtin.id, true);
    EXPECT_TRUE(HasLabelText(page, QStringLiteral("Modified from preset")));
    EXPECT_FALSE(HasLabelText(page, QStringLiteral("Modified from built-in")));
}

TEST_F(ConfigPageTest, WebcamDetailsButtonExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* webcam_btn = page.findChild<QPushButton*>(QStringLiteral("webcamDetailsBtn"));
    ASSERT_NE(webcam_btn, nullptr);
    EXPECT_EQ(webcam_btn->text(), QStringLiteral("Open Webcam Setup"));
}

TEST_F(ConfigPageTest, AdvancedDetailsButtonExists) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* advanced_btn = page.findChild<QPushButton*>(QStringLiteral("advancedDetailsBtn"));
    ASSERT_NE(advanced_btn, nullptr);
    EXPECT_EQ(advanced_btn->text(), QStringLiteral("Open Advanced"));
}

TEST_F(ConfigPageTest, WebcamDetailsButtonClick_EmitsSignal) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::webcamDetailsRequested, [&emitted]() { emitted = true; });

    auto* webcam_btn = page.findChild<QPushButton*>(QStringLiteral("webcamDetailsBtn"));
    ASSERT_NE(webcam_btn, nullptr);
    webcam_btn->click();
    EXPECT_TRUE(emitted);
}

TEST_F(ConfigPageTest, AdvancedDetailsButtonClick_EmitsSignal) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    QObject::connect(&page, &ConfigPage::advancedRequested, [&emitted]() { emitted = true; });

    auto* advanced_btn = page.findChild<QPushButton*>(QStringLiteral("advancedDetailsBtn"));
    ASSERT_NE(advanced_btn, nullptr);
    advanced_btn->click();
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

    page.setProfileOptions(opts, QStringLiteral("test"), false);

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

TEST_F(ConfigPageTest, QualityCardClick_UpdatesModelAndSummary) {
    ConfigPage page(output_defaults_, video_defaults_);

    bool emitted = false;
    VideoSettingsModel changed;
    QObject::connect(&page, &ConfigPage::videoSettingsChanged, [&](const VideoSettingsModel& settings) {
        emitted = true;
        changed = settings;
    });

    auto* high_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh"));
    auto* small_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardSmall"));
    ASSERT_NE(high_card, nullptr);
    ASSERT_NE(small_card, nullptr);

    small_card->click();
    EXPECT_TRUE(emitted);
    EXPECT_EQ(changed.quality, recorder_core::NvencQualityPreset::Small);
    EXPECT_TRUE(small_card->property("qualityCardSelected").toBool());
    EXPECT_FALSE(high_card->property("qualityCardSelected").toBool());

    auto* settings_label = page.findChild<QLabel*>(QStringLiteral("qualitySettingsLabel"));
    ASSERT_NE(settings_label, nullptr);
    EXPECT_TRUE(settings_label->text().contains(QStringLiteral("CQ 30")));

    auto* badge_label = page.findChild<QLabel*>(QStringLiteral("qualityBadgeLabel"));
    ASSERT_NE(badge_label, nullptr);
    EXPECT_EQ(badge_label->text(), QStringLiteral("Smaller files"));
}

TEST_F(ConfigPageTest, SetVideoSettings_UpdatesQualityCardSelection) {
    ConfigPage page(output_defaults_, video_defaults_);

    VideoSettingsModel balanced = video_defaults_;
    balanced.quality = recorder_core::NvencQualityPreset::Balanced;
    page.setVideoSettings(balanced);

    auto* high_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh"));
    auto* balanced_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardBalanced"));
    auto* small_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardSmall"));
    auto* custom_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardCustom"));
    ASSERT_NE(high_card, nullptr);
    ASSERT_NE(balanced_card, nullptr);
    ASSERT_NE(small_card, nullptr);
    ASSERT_NE(custom_card, nullptr);

    EXPECT_FALSE(high_card->property("qualityCardSelected").toBool());
    EXPECT_TRUE(balanced_card->property("qualityCardSelected").toBool());
    EXPECT_FALSE(small_card->property("qualityCardSelected").toBool());
    EXPECT_FALSE(custom_card->property("qualityCardSelected").toBool());
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
    auto* quality_high_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh"));
    ASSERT_NE(quality_high_card, nullptr);
    EXPECT_FALSE(quality_high_card->isEnabled());
    auto* quality_custom_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardCustom"));
    ASSERT_NE(quality_custom_card, nullptr);
    EXPECT_FALSE(quality_custom_card->isEnabled());

    auto* dest_edit = page.findChild<QLineEdit*>(QStringLiteral("destinationEdit"));
    ASSERT_NE(dest_edit, nullptr);
    EXPECT_FALSE(dest_edit->isEnabled());

    auto* naming_edit = page.findChild<QLineEdit*>(QStringLiteral("namingEdit"));
    ASSERT_NE(naming_edit, nullptr);
    EXPECT_FALSE(naming_edit->isEnabled());

    auto* mic_combo = page.findChild<QComboBox*>(QStringLiteral("micDeviceCombo"));
    ASSERT_NE(mic_combo, nullptr);
    EXPECT_FALSE(mic_combo->isEnabled());

    auto* webcam_btn = page.findChild<QPushButton*>(QStringLiteral("webcamDetailsBtn"));
    ASSERT_NE(webcam_btn, nullptr);
    EXPECT_FALSE(webcam_btn->isEnabled());

    auto* lock_note = page.findChild<QLabel*>(QStringLiteral("lockNoteLabel"));
    ASSERT_NE(lock_note, nullptr);
    EXPECT_FALSE(lock_note->isHidden());
}

TEST_F(ConfigPageTest, DefaultControlsAreEnabled) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* quality_combo = page.findChild<QComboBox*>(QStringLiteral("videoQualityCombo"));
    ASSERT_NE(quality_combo, nullptr);
    EXPECT_TRUE(quality_combo->isEnabled());
    auto* quality_high_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardHigh"));
    ASSERT_NE(quality_high_card, nullptr);
    EXPECT_TRUE(quality_high_card->isEnabled());
    auto* quality_custom_card = page.findChild<QPushButton*>(QStringLiteral("qualityCardCustom"));
    ASSERT_NE(quality_custom_card, nullptr);
    EXPECT_FALSE(quality_custom_card->isEnabled());

    auto* mic_combo = page.findChild<QComboBox*>(QStringLiteral("micDeviceCombo"));
    ASSERT_NE(mic_combo, nullptr);
    EXPECT_TRUE(mic_combo->isEnabled());

    auto* lock_note = page.findChild<QLabel*>(QStringLiteral("lockNoteLabel"));
    ASSERT_NE(lock_note, nullptr);
    EXPECT_TRUE(lock_note->isHidden());
}

TEST_F(ConfigPageTest, WebcamInfoLabel_DisabledState_ShowsDisabledText) {
    ConfigPage page(output_defaults_, video_defaults_);

    WebcamSettings ws;
    ws.enabled = false;
    ws.device_id = "";
    page.setWebcamSettings(ws);

    const auto labels = page.findChildren<QLabel*>();
    bool found_disabled = false;
    for (const auto* l : labels) {
        if (l->text() == QStringLiteral("Webcam recording is disabled.")) {
            found_disabled = true;
            break;
        }
    }
    EXPECT_TRUE(found_disabled);
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

TEST_F(ConfigPageTest, QualityBadgeLabel_ExistsAndNotEmpty) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* label = page.findChild<QLabel*>(QStringLiteral("qualityBadgeLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->text().isEmpty());
}

TEST_F(ConfigPageTest, QualitySettingsLabel_ExistsAndNotEmpty) {
    ConfigPage page(output_defaults_, video_defaults_);

    auto* label = page.findChild<QLabel*>(QStringLiteral("qualitySettingsLabel"));
    ASSERT_NE(label, nullptr);
    EXPECT_FALSE(label->text().isEmpty());
}

TEST_F(ConfigPageTest, QualitySettingsLabel_UpdatesOnSetVideoSettings) {
    ConfigPage page(output_defaults_, video_defaults_);

    VideoSettingsModel high;
    high.quality = recorder_core::NvencQualityPreset::High;
    high.cfr = true;
    high.capture_cursor = false;
    page.setVideoSettings(high);

    auto* settings_label = page.findChild<QLabel*>(QStringLiteral("qualitySettingsLabel"));
    ASSERT_NE(settings_label, nullptr);
    EXPECT_TRUE(settings_label->text().contains(QStringLiteral("CQ 19")));
    EXPECT_TRUE(settings_label->text().contains(QStringLiteral("Cursor off")));

    auto* badge_label = page.findChild<QLabel*>(QStringLiteral("qualityBadgeLabel"));
    ASSERT_NE(badge_label, nullptr);
    EXPECT_EQ(badge_label->text(), QStringLiteral("Sharper · larger files"));
}

} // namespace
} // namespace exosnap
