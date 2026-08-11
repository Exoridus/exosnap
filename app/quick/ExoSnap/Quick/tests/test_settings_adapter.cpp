#include "SettingsAdapter.h"

#include "QuickThemeTokens.h"

#include <capability/capability_builder.h>

#include <QVariantMap>

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

using capability::AudioCodec;
using capability::Container;
using capability::VideoCodec;

// Minimal signal counter -- avoids pulling Qt Test into a plain gtest target.
class SignalCounter {
  public:
    template <typename Signal> SignalCounter(SettingsAdapter& adapter, Signal signal) {
        QObject::connect(&adapter, signal, &adapter, [this]() { ++count_; });
    }

    [[nodiscard]] int count() const noexcept {
        return count_;
    }

  private:
    int count_ = 0;
};

// SettingsAdapter is a QObject, so every case gets its own in-place instance
// seeded with the static validated baseline and the shipped default preset.
class SettingsAdapterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        adapter.setCapabilities(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
        adapter.setConfig(MakeDefaultPreset().config);
    }

    SettingsAdapter adapter;
};

QVariantMap optionFor(const QVariantList& options, int value) {
    for (const QVariant& entry : options) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("value")).toInt() == value) {
            return map;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Reconciliation is delegated, not duplicated
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, SwitchingToMp4ReconcilesAudioCodec) {
    adapter.setAudioCodec(static_cast<int>(AudioCodec::Opus));
    ASSERT_EQ(adapter.audioCodec(), static_cast<int>(AudioCodec::Opus));

    adapter.setContainer(static_cast<int>(Container::Mp4));

    // Opus is prohibited in MP4 (ADR 0010); ReconcileContainerCodecs owns that
    // rule, so the adapter must surface AAC without deciding it itself.
    EXPECT_EQ(adapter.container(), static_cast<int>(Container::Mp4));
    EXPECT_EQ(adapter.audioCodec(), static_cast<int>(AudioCodec::Aac));
}

TEST_F(SettingsAdapterTest, ProhibitedCodecOptionStaysVisibleButUnselectable) {
    adapter.setContainer(static_cast<int>(Container::WebM));

    const QVariantMap aac = optionFor(adapter.audioCodecOptions(), static_cast<int>(AudioCodec::Aac));
    ASSERT_FALSE(aac.isEmpty());
    EXPECT_FALSE(aac.value(QStringLiteral("selectable")).toBool());
    EXPECT_FALSE(aac.value(QStringLiteral("reason")).toString().isEmpty());
}

TEST_F(SettingsAdapterTest, WebMForcesAv1AndOpus) {
    adapter.setContainer(static_cast<int>(Container::WebM));

    EXPECT_EQ(adapter.videoCodec(), static_cast<int>(VideoCodec::Av1));
    EXPECT_EQ(adapter.audioCodec(), static_cast<int>(AudioCodec::Opus));
}

// ---------------------------------------------------------------------------
// Edit notification contract
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, EditEmitsConfigEditedOnceAndNoOpDoesNot) {
    SignalCounter spy(adapter, &SettingsAdapter::configEdited);

    adapter.setCaptureCursor(!adapter.captureCursor());
    EXPECT_EQ(spy.count(), 1);

    const bool current = adapter.captureCursor();
    adapter.setCaptureCursor(current);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsAdapterTest, AppSettingsEditIsSeparateFromConfigEdit) {
    SignalCounter config_spy(adapter, &SettingsAdapter::configEdited);
    SignalCounter app_spy(adapter, &SettingsAdapter::appSettingsEdited);

    adapter.setShowNotifications(!adapter.showNotifications());

    EXPECT_EQ(config_spy.count(), 0);
    EXPECT_EQ(app_spy.count(), 1);
    EXPECT_EQ(adapter.appSettings().show_notifications, adapter.showNotifications());
}

// ---------------------------------------------------------------------------
// Quality / frame-rate mapping
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, QualityPresetMapsToCanonicalCq) {

    adapter.setQualityPreset(static_cast<int>(recorder_core::QualityPreset::Efficient));

    EXPECT_EQ(adapter.cq(), static_cast<int>(recorder_core::CanonicalCq(recorder_core::QualityPreset::Efficient)));
    EXPECT_EQ(adapter.qualityPreset(), static_cast<int>(recorder_core::QualityPreset::Efficient));
}

TEST_F(SettingsAdapterTest, MaxFrameRateClampsConfiguredRateAndOptions) {
    adapter.setFrameRate(240);
    ASSERT_EQ(adapter.frameRate(), 240);

    adapter.setMaxFrameRate(60);

    EXPECT_EQ(adapter.frameRate(), 60);
    for (const QVariant& entry : adapter.frameRateOptions()) {
        EXPECT_LE(entry.toMap().value(QStringLiteral("value")).toInt(), 60);
    }
}

TEST_F(SettingsAdapterTest, BitrateIsOnlyRelevantForBitrateModes) {
    adapter.setRateControl(static_cast<int>(recorder_core::RateControlMode::ConstantQuality));
    EXPECT_FALSE(adapter.bitrateRelevant());

    adapter.setRateControl(static_cast<int>(recorder_core::RateControlMode::VariableBitrate));
    EXPECT_TRUE(adapter.bitrateRelevant());
}

// ---------------------------------------------------------------------------
// Audio rows
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, MicrophoneRowEnableAndMergeRoundTrip) {

    adapter.setMicrophoneEnabled(true);
    ASSERT_TRUE(adapter.microphoneEnabled());

    adapter.setMicrophoneSeparate(false);
    EXPECT_FALSE(adapter.microphoneSeparate());

    adapter.setMicrophoneSeparate(true);
    EXPECT_TRUE(adapter.microphoneSeparate());
}

TEST_F(SettingsAdapterTest, AudioSummaryListsEnabledSourcesInProductOrder) {
    adapter.setSystemAudioEnabled(true);
    adapter.setMicrophoneEnabled(true);

    EXPECT_EQ(adapter.audioSummary(), QStringLiteral("SYS · MIC"));
}

// ---------------------------------------------------------------------------
// Codec-gated relevance
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, OpusLocksSampleRateAndExposesOpusControls) {
    adapter.setAudioCodec(static_cast<int>(AudioCodec::Opus));

    EXPECT_TRUE(adapter.opusControlsRelevant());
    EXPECT_FALSE(adapter.audioSampleRateRelevant());
    EXPECT_FALSE(adapter.audioBitDepthRelevant());
}

TEST_F(SettingsAdapterTest, FlacExposesBitDepthAndCompression) {
    adapter.setAudioCodec(static_cast<int>(AudioCodec::Flac));

    EXPECT_TRUE(adapter.audioBitDepthRelevant());
    EXPECT_TRUE(adapter.flacCompressionRelevant());
    EXPECT_FALSE(adapter.opusControlsRelevant());
}

// ---------------------------------------------------------------------------
// Preset naming
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, PresetNameRejectionFoldsCaseAndHonoursExcludeId) {

    QVariantList options;
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QStringLiteral("preset.abc"));
    entry.insert(QStringLiteral("label"), QStringLiteral("Streaming"));
    options.append(entry);
    adapter.setPresetState(options, QStringLiteral("preset.abc"), false);

    EXPECT_TRUE(adapter.presetNameRejected(QStringLiteral("   "), QString()));
    EXPECT_TRUE(adapter.presetNameRejected(QStringLiteral("streaming "), QString()));
    // Renaming a preset to its own current name is not a collision.
    EXPECT_FALSE(adapter.presetNameRejected(QStringLiteral("Streaming"), QStringLiteral("preset.abc")));
}

TEST_F(SettingsAdapterTest, PresetStateExposesNameAndDirtyStatusSeparately) {

    QVariantList options;
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QStringLiteral("preset.abc"));
    entry.insert(QStringLiteral("label"), QStringLiteral("Streaming"));
    options.append(entry);
    adapter.setPresetState(options, QStringLiteral("preset.abc"), true);

    EXPECT_EQ(adapter.selectedPresetName(), QStringLiteral("Streaming"));
    EXPECT_TRUE(adapter.presetDirty());
    EXPECT_NE(adapter.presetStatusText(), adapter.selectedPresetName());
}

// ---------------------------------------------------------------------------
// Update status presentation
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, UpdateStatusDrivesActionTextAndWhatsNewVisibility) {

    adapter.setUpdateStatus(QStringLiteral("checking"), QString(), QString());
    EXPECT_FALSE(adapter.updateActionEnabled());
    EXPECT_FALSE(adapter.whatsNewAvailable());

    adapter.setUpdateStatus(QStringLiteral("available"), QStringLiteral("v1.2"), QString());
    EXPECT_TRUE(adapter.updateActionEnabled());
    EXPECT_TRUE(adapter.whatsNewAvailable());
    EXPECT_TRUE(adapter.updateActionText().contains(QStringLiteral("v1.2")));
}

TEST_F(SettingsAdapterTest, RecordingLockDisablesTheUpdateAction) {
    adapter.setUpdateStatus(QStringLiteral("available"), QStringLiteral("v1.2"), QString());
    ASSERT_TRUE(adapter.updateActionEnabled());

    adapter.setControlsLocked(true);

    EXPECT_TRUE(adapter.controlsLocked());
    EXPECT_FALSE(adapter.updateActionEnabled());
}

// ---------------------------------------------------------------------------
// Output presentation
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, CustomResolutionActivationAndEvenSizeValidation) {
    EXPECT_FALSE(adapter.customResolutionActive());

    adapter.setResolutionMode(static_cast<int>(OutputResolutionMode::Custom));
    EXPECT_TRUE(adapter.customResolutionActive());
    EXPECT_FALSE(adapter.customResolutionValidation().isEmpty());

    adapter.setCustomWidth(1920);
    adapter.setCustomHeight(1080);
    EXPECT_TRUE(adapter.customResolutionValidation().isEmpty());
}

TEST_F(SettingsAdapterTest, SplitSummaryReportsOffAndActiveThresholds) {
    EXPECT_FALSE(adapter.splitByTimeEnabled());
    EXPECT_FALSE(adapter.splitBySizeEnabled());
    EXPECT_EQ(adapter.splitSummary(), QStringLiteral("Single file"));

    adapter.setSplitByTimeEnabled(true);
    EXPECT_TRUE(adapter.splitByTimeEnabled());
    EXPECT_NE(adapter.splitSummary(), QStringLiteral("Single file"));
}

// ---------------------------------------------------------------------------
// Webcam
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, WebcamResolutionPairsWidthWithSelectedHeight) {
    adapter.setWebcamResolution(1080);
    EXPECT_EQ(adapter.webcamResolution(), 1080);
    EXPECT_EQ(adapter.config().webcam.width, 1920);

    // The 4:3 VGA entry is the deliberate exception to the 16:9 pairing.
    adapter.setWebcamResolution(480);
    EXPECT_EQ(adapter.config().webcam.width, 640);
    EXPECT_EQ(adapter.config().webcam.height, 480);
}

TEST_F(SettingsAdapterTest, ChromaKeyParametersRoundTripThroughPercent) {
    adapter.setChromaKeyTolerance(65);
    adapter.setChromaKeySoftness(0);
    adapter.setChromaKeySpill(100);

    EXPECT_EQ(adapter.chromaKeyTolerance(), 65);
    EXPECT_EQ(adapter.chromaKeySoftness(), 0);
    EXPECT_EQ(adapter.chromaKeySpill(), 100);
    EXPECT_FLOAT_EQ(adapter.config().webcam.chroma_key.tolerance, 0.65f);
}

TEST_F(SettingsAdapterTest, WebcamAvailabilityFollowsDiscoveredDevices) {
    EXPECT_FALSE(adapter.webcamAvailable());

    QVariantList devices;
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), QStringLiteral("cam-1"));
    entry.insert(QStringLiteral("label"), QStringLiteral("Front camera"));
    devices.append(entry);
    adapter.setWebcamDevices(devices);

    EXPECT_TRUE(adapter.webcamAvailable());
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, ThemeOptionsOnlyOfferShippedThemeIds) {
    const QVariantList options = adapter.themeOptions();
    ASSERT_FALSE(options.isEmpty());

    for (const QVariant& entry : options) {
        const QString id = entry.toMap().value(QStringLiteral("value")).toString();
        // Every offered id must resolve to a real theme; an invented id would
        // silently fall back and leave the picker showing a theme it never set.
        QuickThemeTokens tokens;
        tokens.setThemeId(id);
        EXPECT_EQ(tokens.themeId(), id);
    }
}

TEST_F(SettingsAdapterTest, UnknownThemeIdFallsBackToTheShippedDefault) {
    QuickThemeTokens tokens;
    tokens.setThemeId(QStringLiteral("does-not-exist"));

    EXPECT_EQ(tokens.themeId(), QStringLiteral("dark-default"));
    EXPECT_TRUE(tokens.background().isValid());
    EXPECT_TRUE(tokens.accent().isValid());
}

TEST_F(SettingsAdapterTest, EveryShippedThemeResolvesOpaqueSurfacesAndText) {
    for (const QVariant& entry : SettingsAdapter{}.themeOptions()) {
        const QString id = entry.toMap().value(QStringLiteral("value")).toString();
        QuickThemeTokens tokens;
        tokens.setThemeId(id);

        EXPECT_TRUE(tokens.background().isValid()) << id.toStdString();
        EXPECT_TRUE(tokens.surface().isValid()) << id.toStdString();
        EXPECT_TRUE(tokens.text().isValid()) << id.toStdString();
        EXPECT_TRUE(tokens.textSecondary().isValid()) << id.toStdString();
        EXPECT_TRUE(tokens.warningSurface().isValid()) << id.toStdString();
        // Line tokens carry alpha; they must still parse from their rgba() form.
        EXPECT_TRUE(tokens.line().isValid()) << id.toStdString();
        EXPECT_TRUE(tokens.lineStrong().isValid()) << id.toStdString();
    }
}

// ---------------------------------------------------------------------------
// Hotkeys
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, HotkeyCaptureStateClearsOnRowRefresh) {
    adapter.beginHotkeyCapture(2);
    EXPECT_EQ(adapter.capturingHotkeyAction(), 2);

    adapter.setHotkeyRows(QVariantList{});

    EXPECT_EQ(adapter.capturingHotkeyAction(), -1);
    EXPECT_TRUE(adapter.hotkeyErrorText().isEmpty());
}

TEST_F(SettingsAdapterTest, HotkeyErrorEndsCaptureAndIsAttributedToTheAction) {
    adapter.beginHotkeyCapture(1);
    adapter.setHotkeyError(1, QStringLiteral("Alt+F4 is reserved by Windows."));

    EXPECT_EQ(adapter.capturingHotkeyAction(), -1);
    EXPECT_EQ(adapter.hotkeyErrorAction(), 1);
    EXPECT_FALSE(adapter.hotkeyErrorText().isEmpty());
}

TEST_F(SettingsAdapterTest, HotkeyCaptureIsRefusedWhileRecordingLocksControls) {
    adapter.setControlsLocked(true);
    adapter.beginHotkeyCapture(0);

    EXPECT_EQ(adapter.capturingHotkeyAction(), -1);
}

// ---- Overlay content ------------------------------------------------------

// The element properties report the RESOLVED set, so a checkbox bound to one
// shows what the overlay will actually draw under every preset -- not the
// custom list, which a named preset ignores.
TEST_F(SettingsAdapterTest, ElementPropertiesFollowTheSelectedPreset) {
    adapter.setDiagnosticsOverlayPreset(QStringLiteral("technical"));
    EXPECT_TRUE(adapter.diagnosticsOverlayFps());
    EXPECT_TRUE(adapter.diagnosticsOverlaySize());

    adapter.setDiagnosticsOverlayPreset(QStringLiteral("health"));
    EXPECT_FALSE(adapter.diagnosticsOverlayFps());
    EXPECT_FALSE(adapter.diagnosticsOverlaySize());
    EXPECT_TRUE(adapter.diagnosticsOverlayDrop());
}

// Unticking one token under a named preset yields "that preset minus the token"
// as a custom set, rather than reverting to whatever the custom list held.
TEST_F(SettingsAdapterTest, TogglingAnElementPromotesTheNamedPresetToCustom) {
    adapter.setDiagnosticsOverlayPreset(QStringLiteral("technical"));
    adapter.setDiagnosticsOverlayElement(QStringLiteral("fps"), false);

    EXPECT_EQ(adapter.diagnosticsOverlayPreset(), QStringLiteral("custom"));
    EXPECT_FALSE(adapter.diagnosticsOverlayFps());
    EXPECT_TRUE(adapter.diagnosticsOverlayDrop());
    EXPECT_TRUE(adapter.diagnosticsOverlayDrift());
    EXPECT_TRUE(adapter.diagnosticsOverlaySize());
}

TEST_F(SettingsAdapterTest, RecordingOverlayElementsPromoteIndependently) {
    adapter.setRecordingOverlayElement(QStringLiteral("source"), true);

    EXPECT_EQ(adapter.recordingOverlayPreset(), QStringLiteral("custom"));
    EXPECT_TRUE(adapter.recordingOverlaySourceName());
    EXPECT_TRUE(adapter.recordingOverlayElapsed());
    // The diagnostics HUD has its own preset and must not be dragged along.
    EXPECT_EQ(adapter.diagnosticsOverlayPreset(), QStringLiteral("health"));
}

TEST_F(SettingsAdapterTest, UnknownElementTokensAreIgnored) {
    const QString before = adapter.diagnosticsOverlayPreset();
    adapter.setDiagnosticsOverlayElement(QStringLiteral("bogus"), true);
    EXPECT_EQ(adapter.diagnosticsOverlayPreset(), before);
}

// Every option the picker offers has to be a token the resolver accepts back.
TEST_F(SettingsAdapterTest, PresetOptionValuesRoundTrip) {
    for (const QVariant& option : adapter.diagnosticsOverlayPresetOptions()) {
        const QString value = option.toMap().value(QStringLiteral("value")).toString();
        adapter.setDiagnosticsOverlayPreset(value);
        EXPECT_EQ(adapter.diagnosticsOverlayPreset(), value);
    }
}

TEST_F(SettingsAdapterTest, NonLocalUrlIsIgnoredForOutputFolder) {
    const QString before = adapter.outputFolder();

    adapter.setOutputFolderFromUrl(QUrl(QStringLiteral("https://example.invalid/share")));

    EXPECT_EQ(adapter.outputFolder(), before);
}

} // namespace
} // namespace exosnap::quick
