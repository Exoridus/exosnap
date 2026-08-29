#include "SettingsAdapter.h"

#include "QuickThemeTokens.h"

#include <QGuiApplication>
#include <QPalette>

#include <capability/capability_builder.h>

#include <QVariantMap>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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
// The dropdowns offer exactly what the engine accepts
//
// This is the boundary the defect sat on: the registry classified MP4 + AV1 and
// MP4 + PCM/FLAC as Experimental, CapabilitySet translated that to
// NotImplemented, and the adapter's own filter still offered them -- so the user
// could pick a combination the recorder then reconciled away. The capability
// matrix test pins the registry side; this pins the side that decides what the
// user sees, so a filter that goes back to consulting only the per-codec
// capability (QueryVideoCodec, which knows nothing about the container) fails
// here rather than in a recording.
//
// Options stay VISIBLE and carry their reason -- that is the established
// contract (see ProhibitedCodecOptionStaysVisibleButUnselectable); what must not
// happen is `selectable == true`.
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, Mp4OffersOnlyItsVettedVideoCodecs) {
    adapter.setContainer(static_cast<int>(Container::Mp4));
    ASSERT_EQ(adapter.container(), static_cast<int>(Container::Mp4));

    const QVariantList options = adapter.videoCodecOptions();

    for (const VideoCodec codec : {VideoCodec::H264, VideoCodec::Hevc}) {
        const QVariantMap entry = optionFor(options, static_cast<int>(codec));
        ASSERT_FALSE(entry.isEmpty()) << static_cast<int>(codec);
        EXPECT_TRUE(entry.value(QStringLiteral("selectable")).toBool()) << static_cast<int>(codec);
    }

    // AV1-in-MP4 is muxable and deliberately not offered in 0.9.
    const QVariantMap av1 = optionFor(options, static_cast<int>(VideoCodec::Av1));
    ASSERT_FALSE(av1.isEmpty());
    EXPECT_FALSE(av1.value(QStringLiteral("selectable")).toBool());
    EXPECT_FALSE(av1.value(QStringLiteral("reason")).toString().isEmpty());
}

TEST_F(SettingsAdapterTest, Mp4OffersAacAndNothingElse) {
    adapter.setContainer(static_cast<int>(Container::Mp4));

    const QVariantList options = adapter.audioCodecOptions();

    const QVariantMap aac = optionFor(options, static_cast<int>(AudioCodec::Aac));
    ASSERT_FALSE(aac.isEmpty());
    EXPECT_TRUE(aac.value(QStringLiteral("selectable")).toBool());

    // Opus is Prohibited, PCM and FLAC are Experimental. Three different registry
    // levels, one user-visible answer.
    for (const AudioCodec codec : {AudioCodec::Opus, AudioCodec::Pcm, AudioCodec::Flac}) {
        const QVariantMap entry = optionFor(options, static_cast<int>(codec));
        ASSERT_FALSE(entry.isEmpty()) << static_cast<int>(codec);
        EXPECT_FALSE(entry.value(QStringLiteral("selectable")).toBool()) << static_cast<int>(codec);
        EXPECT_FALSE(entry.value(QStringLiteral("reason")).toString().isEmpty()) << static_cast<int>(codec);
    }
}

// The counter-check: MKV must keep offering AV1, or the two cases above would
// pass just as well against a filter that rejects everything.
TEST_F(SettingsAdapterTest, MatroskaStillOffersAv1AndOpus) {
    adapter.setContainer(static_cast<int>(Container::Matroska));

    const QVariantMap av1 = optionFor(adapter.videoCodecOptions(), static_cast<int>(VideoCodec::Av1));
    ASSERT_FALSE(av1.isEmpty());
    EXPECT_TRUE(av1.value(QStringLiteral("selectable")).toBool());

    const QVariantMap opus = optionFor(adapter.audioCodecOptions(), static_cast<int>(AudioCodec::Opus));
    ASSERT_FALSE(opus.isEmpty());
    EXPECT_TRUE(opus.value(QStringLiteral("selectable")).toBool());
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

// The two window-presence switches, which are app settings rather than recording
// configuration: they change what the WINDOW does, and nothing about the file.
TEST_F(SettingsAdapterTest, WindowPresenceSwitchesDefaultOffAndRoundTrip) {
    EXPECT_FALSE(adapter.minimizeToTray());
    EXPECT_FALSE(adapter.hideWindowFromCapture());

    SignalCounter config_spy(adapter, &SettingsAdapter::configEdited);
    SignalCounter app_spy(adapter, &SettingsAdapter::appSettingsEdited);

    adapter.setMinimizeToTray(true);
    adapter.setHideWindowFromCapture(true);

    EXPECT_EQ(config_spy.count(), 0);
    EXPECT_EQ(app_spy.count(), 2);
    EXPECT_TRUE(adapter.appSettings().minimize_to_tray);
    EXPECT_TRUE(adapter.appSettings().hide_window_from_capture);

    // Idempotent, so a re-published settings struct does not look like an edit.
    adapter.setMinimizeToTray(true);
    EXPECT_EQ(app_spy.count(), 2);
}

// ---------------------------------------------------------------------------
// Quality / frame-rate mapping
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, QualityPresetMapsToCanonicalCq) {

    adapter.setQualityPreset(static_cast<int>(exosnap::engine::QualityPreset::Low));

    EXPECT_EQ(adapter.cq(), static_cast<int>(exosnap::engine::CanonicalCq(exosnap::engine::QualityPreset::Low)));
    EXPECT_EQ(adapter.qualityPreset(), static_cast<int>(exosnap::engine::QualityPreset::Low));
}

TEST_F(SettingsAdapterTest, QualityTierLabelsCarryNoCqNumber) {
    // The CQ number is an ExoSnap scale that each codec maps onto its own
    // quantizer. Printing it in the Default ladder presented that abstraction as
    // the value the encoder receives.
    for (const QVariant& entry : adapter.qualityPresetOptions()) {
        const QString label = entry.toMap().value(QStringLiteral("label")).toString();
        EXPECT_FALSE(label.contains(QStringLiteral("CQ"))) << label.toStdString();
    }
}

TEST_F(SettingsAdapterTest, NativeQuantizerHintNamesTheSelectedCodecsOwnParameter) {
    adapter.setCq(19);

    adapter.setVideoCodec(static_cast<int>(VideoCodec::Av1));
    EXPECT_EQ(adapter.nativeQuantizerHint(), QStringLiteral("AV1 qindex 65 of 255"));

    adapter.setVideoCodec(static_cast<int>(VideoCodec::H264));
    EXPECT_EQ(adapter.nativeQuantizerHint(), QStringLiteral("H.264 QP 19 of 51"));

    adapter.setVideoCodec(static_cast<int>(VideoCodec::Hevc));
    EXPECT_EQ(adapter.nativeQuantizerHint(), QStringLiteral("HEVC QP 19 of 51"));
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
    adapter.setRateControl(static_cast<int>(exosnap::engine::RateControlMode::ConstantQuality));
    EXPECT_FALSE(adapter.bitrateRelevant());

    adapter.setRateControl(static_cast<int>(exosnap::engine::RateControlMode::VariableBitrate));
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

    EXPECT_EQ(adapter.audioSummary(), QStringLiteral("System audio · Microphone"));
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
    // The badge states the condition only. Repeating the name next to the
    // selector that already shows it reads as a second preset field.
    EXPECT_FALSE(adapter.presetStatusText().contains(adapter.selectedPresetName()));
    EXPECT_FALSE(adapter.presetStatusText().isEmpty());

    adapter.setPresetState(options, QStringLiteral("preset.abc"), false);
    EXPECT_FALSE(adapter.presetDirty());
    EXPECT_TRUE(adapter.presetStatusText().isEmpty());
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
// Appearance and accent
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, AppearanceAndAccentOptionsOnlyOfferShippedIds) {
    const QVariantList appearances = adapter.appearanceOptions();
    const QVariantList accents = adapter.accentOptions();
    ASSERT_FALSE(appearances.isEmpty());
    ASSERT_FALSE(accents.isEmpty());

    // Every offered id must resolve to itself; an invented id would silently
    // fall back and leave the picker showing a value it never set.
    for (const QVariant& entry : appearances) {
        const QString id = entry.toMap().value(QStringLiteral("value")).toString();
        QuickThemeTokens tokens;
        tokens.setAppearance(id, QStringLiteral("aqua"));
        EXPECT_EQ(tokens.appearanceId(), id);
    }
    for (const QVariant& entry : accents) {
        const QString id = entry.toMap().value(QStringLiteral("value")).toString();
        QuickThemeTokens tokens;
        tokens.setAppearance(QStringLiteral("dark"), id);
        EXPECT_EQ(tokens.accentId(), id);
    }
}

TEST_F(SettingsAdapterTest, TheWidgetsPaletteFollowsTheAppearance) {
    // The tray menu is a QMenu: Qt.labs.platform's Menu is native only on macOS,
    // iOS, Android and GTK+ Linux, and falls back to Qt Widgets everywhere else.
    // A QMenu takes its colours from the application palette, so an unset palette
    // is a light menu underneath a dark application.
    QuickThemeTokens dark;
    dark.setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    const QPalette dark_palette = dark.widgetsPalette();
    EXPECT_EQ(dark_palette.color(QPalette::Window), dark.surfaceRaised());
    EXPECT_EQ(dark_palette.color(QPalette::WindowText), dark.text());
    EXPECT_EQ(dark_palette.color(QPalette::Highlight), dark.accent());
    EXPECT_EQ(dark_palette.color(QPalette::HighlightedText), dark.accentInk());
    EXPECT_EQ(dark_palette.color(QPalette::Disabled, QPalette::Text), dark.textDim());

    QuickThemeTokens light;
    light.setAppearance(QStringLiteral("light"), QStringLiteral("aqua"));
    const QPalette light_palette = light.widgetsPalette();
    EXPECT_EQ(light_palette.color(QPalette::Window), light.surfaceRaised());
    EXPECT_NE(light_palette.color(QPalette::Window), dark_palette.color(QPalette::Window))
        << "the menu did not follow the appearance change";
}

TEST_F(SettingsAdapterTest, UnknownAppearanceOrAccentFallsBackToTheShippedDefault) {
    QuickThemeTokens tokens;
    tokens.setAppearance(QStringLiteral("does-not-exist"), QStringLiteral("also-not-real"));

    EXPECT_EQ(tokens.appearanceId(), QStringLiteral("dark"));
    EXPECT_EQ(tokens.accentId(), QStringLiteral("aqua"));
    EXPECT_TRUE(tokens.background().isValid());
    EXPECT_TRUE(tokens.accent().isValid());
}

TEST_F(SettingsAdapterTest, EveryAppearanceAccentPairResolvesEveryToken) {
    for (const QVariant& appearance_entry : SettingsAdapter{}.appearanceOptions()) {
        const QString appearance = appearance_entry.toMap().value(QStringLiteral("value")).toString();
        for (const QVariant& accent_entry : QuickThemeTokens::accentOptions(appearance)) {
            const QString accent = accent_entry.toMap().value(QStringLiteral("value")).toString();
            const std::string pair = (appearance + QLatin1Char('+') + accent).toStdString();
            QuickThemeTokens tokens;
            tokens.setAppearance(appearance, accent);

            EXPECT_TRUE(tokens.background().isValid()) << pair;
            EXPECT_TRUE(tokens.surface().isValid()) << pair;
            EXPECT_TRUE(tokens.surfaceRaised().isValid()) << pair;
            EXPECT_TRUE(tokens.surfaceHover().isValid()) << pair;
            EXPECT_TRUE(tokens.text().isValid()) << pair;
            EXPECT_TRUE(tokens.textSecondary().isValid()) << pair;
            EXPECT_TRUE(tokens.accent().isValid()) << pair;
            EXPECT_TRUE(tokens.accentInk().isValid()) << pair;
            EXPECT_TRUE(tokens.warningSurface().isValid()) << pair;
            // Line tokens carry alpha; they must still parse from their rgba() form.
            EXPECT_TRUE(tokens.line().isValid()) << pair;
            EXPECT_TRUE(tokens.lineStrong().isValid()) << pair;
        }
    }
}

// Every surface rung must be a DIFFERENT colour. Both light themes this model
// replaces set the raised-control surface and the hover surface to pure white,
// which is why the light UI read as flat: a control, the card holding it and
// that card's hover state were one colour, so hover did nothing at all.
TEST_F(SettingsAdapterTest, EveryAppearanceHasFourDistinctSurfaceRungs) {
    for (const QVariant& entry : SettingsAdapter{}.appearanceOptions()) {
        const QString appearance = entry.toMap().value(QStringLiteral("value")).toString();
        const std::string named = appearance.toStdString();
        QuickThemeTokens tokens;
        tokens.setAppearance(appearance, QStringLiteral("aqua"));

        const QList<QColor> rungs{tokens.background(), tokens.surface(), tokens.surfaceRaised(), tokens.surfaceHover()};
        for (qsizetype i = 0; i < rungs.size(); ++i) {
            for (qsizetype j = i + 1; j < rungs.size(); ++j) {
                EXPECT_NE(rungs[i].name(), rungs[j].name())
                    << named << ": surface rungs " << i << " and " << j << " are the same colour";
            }
        }
    }
}

// The accent is a highlight, never a state. A user who picks an accent must
// still be able to tell selection from recording, caution and ready — which is
// why the curated list is all cool hues and the semantic colours are stored on
// the appearance rather than derived from the accent.
//
// Two thresholds, because the two confusions are not equally costly.
// Error and caution mean "something is wrong": an accent close enough to either
// makes an ordinary selection read as a problem, so the whole warm arc is out
// of bounds. Ready/success is the benign one, and the product's own identity
// colour — Studio Mint — has always sat within 30 degrees of it; that pair is
// separated by saturation and by context (a status dot versus a selected tab)
// rather than by hue, and it has shipped and been reviewed that way. Pinning it
// at a lower bound records the real constraint instead of a number the shipped
// palette does not meet.
TEST_F(SettingsAdapterTest, NoAccentCollidesWithASemanticColour) {
    const auto hue_distance = [](const QColor& a, const QColor& b) {
        const int raw = std::abs(a.toHsv().hue() - b.toHsv().hue());
        return std::min(raw, 360 - raw);
    };

    for (const QVariant& appearance_entry : SettingsAdapter{}.appearanceOptions()) {
        const QString appearance = appearance_entry.toMap().value(QStringLiteral("value")).toString();
        for (const QVariant& accent_entry : QuickThemeTokens::accentOptions(appearance)) {
            const QString accent = accent_entry.toMap().value(QStringLiteral("value")).toString();
            const std::string pair = (appearance + QLatin1Char('+') + accent).toStdString();
            QuickThemeTokens tokens;
            tokens.setAppearance(appearance, accent);
            ASSERT_GE(tokens.accent().toHsv().hue(), 0) << pair << ": the accent has no hue to compare";

            EXPECT_GE(hue_distance(tokens.accent(), tokens.error()), 40)
                << pair << ": selection would read as a recording or error state";
            EXPECT_GE(hue_distance(tokens.accent(), tokens.warning()), 40)
                << pair << ": selection would read as a caution state";
            EXPECT_GE(hue_distance(tokens.accent(), tokens.success()), 20)
                << pair << ": selection would read as a ready state";
        }
    }
}

// ---------------------------------------------------------------------------
// Theme migration
// ---------------------------------------------------------------------------

TEST_F(SettingsAdapterTest, EveryLegacyThemeIdMigratesToALiveAppearanceAndAccent) {
    // The exact mapping is a product promise: an existing install keeps the
    // colour it had rather than snapping to the default. `light-paper` lands on
    // Sky, not on the default Aqua, because its accent token WAS petrol blue.
    const QList<std::array<QString, 3>> expected{
        {QStringLiteral("dark-default"), QStringLiteral("dark"), QStringLiteral("aqua")},
        {QStringLiteral("dark-indigo"), QStringLiteral("dark"), QStringLiteral("violet")},
        {QStringLiteral("light-paper"), QStringLiteral("light"), QStringLiteral("sky")},
        {QStringLiteral("light-slate"), QStringLiteral("light"), QStringLiteral("violet")},
    };

    for (const auto& row : expected) {
        const std::string named = row[0].toStdString();
        EXPECT_EQ(QuickThemeTokens::migratedAppearanceId(row[0]), row[1]) << named;
        EXPECT_EQ(QuickThemeTokens::migratedAccentId(row[0]), row[2]) << named;

        // And the pair it names must actually resolve, or the migration would
        // hand the UI a value the tables do not carry.
        QuickThemeTokens tokens;
        tokens.setAppearance(row[1], row[2]);
        EXPECT_EQ(tokens.appearanceId(), row[1]) << named;
        EXPECT_EQ(tokens.accentId(), row[2]) << named;
    }
}

TEST_F(SettingsAdapterTest, AnUnknownOrEmptyLegacyThemeIdMigratesToTheDefault) {
    for (const QString& id : {QString(), QStringLiteral("dark-teal"), QStringLiteral("dark")}) {
        EXPECT_EQ(QuickThemeTokens::migratedAppearanceId(id), QStringLiteral("dark"));
        EXPECT_EQ(QuickThemeTokens::migratedAccentId(id), QStringLiteral("aqua"));
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

// A capture attempt that hit a conflict leaves the row's error visible; clearing
// that row's binding afterward must drop the warning along with it, or the row
// reads as unset-but-still-conflicting.
TEST_F(SettingsAdapterTest, ClearingAHotkeyDropsItsStaleConflictWarning) {
    adapter.beginHotkeyCapture(1);
    adapter.setHotkeyError(1, QStringLiteral("Alt+F4 is reserved by Windows."));
    ASSERT_EQ(adapter.hotkeyErrorAction(), 1);
    ASSERT_FALSE(adapter.hotkeyErrorText().isEmpty());

    adapter.clearHotkey(1);

    EXPECT_NE(adapter.hotkeyErrorAction(), 1);
    EXPECT_TRUE(adapter.hotkeyErrorText().isEmpty());
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

TEST_F(SettingsAdapterTest, OutputValidationRequestsCoverEveryLifecycleTrigger) {
    std::vector<SettingsAdapter::OutputValidationTrigger> triggers;
    QObject::connect(&adapter, &SettingsAdapter::outputValidationRequested, &adapter,
                     [&](SettingsAdapter::OutputValidationTrigger trigger) { triggers.push_back(trigger); });

    adapter.requestOutputValidation(SettingsAdapter::OutputValidationTrigger::Startup);
    adapter.setOutputFolder(adapter.outputFolder() + QStringLiteral("_edited"));
    adapter.requestOutputValidation(SettingsAdapter::OutputValidationTrigger::ApplicationActivation);
    adapter.requestOutputValidation(SettingsAdapter::OutputValidationTrigger::OutputCardReveal);

    EXPECT_EQ(triggers,
              (std::vector<SettingsAdapter::OutputValidationTrigger>{
                  SettingsAdapter::OutputValidationTrigger::Startup, SettingsAdapter::OutputValidationTrigger::PathEdit,
                  SettingsAdapter::OutputValidationTrigger::ApplicationActivation,
                  SettingsAdapter::OutputValidationTrigger::OutputCardReveal}));
}

TEST_F(SettingsAdapterTest, OutputValidationRunsOffTheRequestingThread) {
    const std::thread::id requesting_thread = std::this_thread::get_id();
    std::thread::id validation_thread;
    std::mutex mutex;
    std::condition_variable completed;
    bool finished = false;
    adapter.setOutputFolderValidator([&](const std::filesystem::path&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            validation_thread = std::this_thread::get_id();
            finished = true;
        }
        completed.notify_one();
        return FolderValidationResult::Ok;
    });

    adapter.requestOutputValidation(SettingsAdapter::OutputValidationTrigger::Startup);

    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(completed.wait_for(lock, std::chrono::seconds(2), [&] { return finished; }));
    EXPECT_NE(validation_thread, requesting_thread);
}

TEST_F(SettingsAdapterTest, OutputDestinationFocusRequestUsesTypedTarget) {
    std::optional<SettingsAdapter::FocusTarget> requested;
    QObject::connect(&adapter, &SettingsAdapter::settingsFocusRequested, &adapter,
                     [&](SettingsAdapter::FocusTarget target) { requested = target; });

    adapter.requestSettingsFocus(SettingsAdapter::FocusTarget::OutputDestination);

    ASSERT_TRUE(requested.has_value());
    EXPECT_EQ(*requested, SettingsAdapter::FocusTarget::OutputDestination);
}

TEST_F(SettingsAdapterTest, ApplyingOutputValidationPublishesTheInlineFolderMessage) {
    adapter.applyOutputFolderValidation(FolderValidationResult::NotWritable);

    EXPECT_EQ(adapter.folderValidation(),
              QString::fromStdWString(FolderValidationMessage(FolderValidationResult::NotWritable)));

    adapter.applyOutputFolderValidation(FolderValidationResult::Ok);
    EXPECT_TRUE(adapter.folderValidation().isEmpty());
}

} // namespace
} // namespace exosnap::quick
