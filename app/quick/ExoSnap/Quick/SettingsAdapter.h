#pragma once

#include "models/AudioMeterScale.h"
#include "models/CrashReportPolicy.h"
#include "models/OutputPathValidator.h"
#include "models/OverlayContentPolicy.h"
#include "models/RecordingPreset.h"
#include "settings/AppSettingsStore.h"

#include <capability/capability_set.h>

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace exosnap::quick {

// Narrow QObject boundary for the Settings area.
//
// It owns the live RecordingPresetConfig the user is editing and the persisted
// application settings, and exposes them as typed properties plus capability-
// gated option lists. Every gate delegates to the existing owners --
// capability::CapabilitySet for codec/chroma/bit-depth/HDR support,
// SanitizePresetConfig / ReconcileContainerCodecs for reconciliation, and
// OutputPathPolicy / FilenameBuilder for path and pattern rules. No container,
// codec, capability, or persistence policy is expressed in QML or duplicated
// here; the adapter only maps between those owners and presentation state.
//
// The composition root (QuickApplication) seeds it, listens for configEdited()
// / appSettingsEdited() and owns persistence and propagation to the recording
// side. QML never sees a service, a store, or a coordinator.
class SettingsAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("SettingsAdapter is provided by the application")

  public:
    enum class OutputValidationTrigger { Startup, PathEdit, ApplicationActivation, OutputCardReveal };
    Q_ENUM(OutputValidationTrigger)

    enum class FocusTarget { OutputDestination };
    Q_ENUM(FocusTarget)

    using OutputFolderValidator = std::function<FolderValidationResult(const std::filesystem::path&)>;

    // ---- Global tier / lock -------------------------------------------------
    Q_PROPERTY(bool expertMode READ expertMode WRITE setExpertMode NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool controlsLocked READ controlsLocked NOTIFY controlsLockedChanged FINAL)
    // The adapter the encode is running on, as a fact rather than as a choice.
    // Empty until the capability probe has named one; the row that shows it says
    // so rather than inventing a placeholder.
    Q_PROPERTY(QString encodeAdapterName READ encodeAdapterName NOTIFY optionsChanged FINAL)

    // ---- Container & codecs -------------------------------------------------
    Q_PROPERTY(QVariantList containerOptions READ containerOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int container READ container WRITE setContainer NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList videoCodecOptions READ videoCodecOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int videoCodec READ videoCodec WRITE setVideoCodec NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList audioCodecOptions READ audioCodecOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int audioCodec READ audioCodec WRITE setAudioCodec NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList bitDepthOptions READ bitDepthOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int bitDepth READ bitDepth WRITE setBitDepth NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList chromaOptions READ chromaOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int chroma READ chroma WRITE setChroma NOTIFY configChanged FINAL)
    Q_PROPERTY(QString chromaHint READ chromaHint NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QVariantList colorRangeOptions READ colorRangeOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int colorRange READ colorRange WRITE setColorRange NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList hdrModeOptions READ hdrModeOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int hdrMode READ hdrMode WRITE setHdrMode NOTIFY configChanged FINAL)
    Q_PROPERTY(QString hdrHint READ hdrHint NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool hdrRelevant READ hdrRelevant NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QVariantList encoderPresetOptions READ encoderPresetOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int encoderPreset READ encoderPreset WRITE setEncoderPreset NOTIFY configChanged FINAL)
    Q_PROPERTY(QString formatSummary READ formatSummary NOTIFY configChanged FINAL)
    Q_PROPERTY(QString compatNotice READ compatNotice NOTIFY configChanged FINAL)
    Q_PROPERTY(bool compatOk READ compatOk NOTIFY configChanged FINAL)

    // ---- Quality & timing ---------------------------------------------------
    Q_PROPERTY(QVariantList qualityPresetOptions READ qualityPresetOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int qualityPreset READ qualityPreset WRITE setQualityPreset NOTIFY configChanged FINAL)
    Q_PROPERTY(int cq READ cq WRITE setCq NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList rateControlOptions READ rateControlOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int rateControl READ rateControl WRITE setRateControl NOTIFY configChanged FINAL)
    Q_PROPERTY(bool bitrateRelevant READ bitrateRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(QString nativeQuantizerHint READ nativeQuantizerHint NOTIFY configChanged FINAL)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList frameRateOptions READ frameRateOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY configChanged FINAL)
    Q_PROPERTY(int maxFrameRate READ maxFrameRate NOTIFY optionsChanged FINAL)
    Q_PROPERTY(QVariantList timingOptions READ timingOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(bool cfr READ cfr WRITE setCfr NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList framePacingOptions READ framePacingOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int framePacing READ framePacing WRITE setFramePacing NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList keyframeIntervalOptions READ keyframeIntervalOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int keyframeInterval READ keyframeInterval WRITE setKeyframeInterval NOTIFY configChanged FINAL)
    Q_PROPERTY(bool captureCursor READ captureCursor WRITE setCaptureCursor NOTIFY configChanged FINAL)

    // ---- Output -------------------------------------------------------------
    Q_PROPERTY(QString outputFolder READ outputFolder WRITE setOutputFolder NOTIFY configChanged FINAL)
    Q_PROPERTY(QString namingPattern READ namingPattern WRITE setNamingPattern NOTIFY configChanged FINAL)
    Q_PROPERTY(QString exampleFilename READ exampleFilename NOTIFY configChanged FINAL)
    Q_PROPERTY(QString outputSummary READ outputSummary NOTIFY configChanged FINAL)
    Q_PROPERTY(QString savesToText READ savesToText NOTIFY configChanged FINAL)
    Q_PROPERTY(QString folderValidation READ folderValidation NOTIFY configChanged FINAL)
    Q_PROPERTY(QString patternValidation READ patternValidation NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList filenameTokens READ filenameTokens CONSTANT FINAL)
    Q_PROPERTY(QVariantList resolutionOptions READ resolutionOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int resolutionMode READ resolutionMode WRITE setResolutionMode NOTIFY configChanged FINAL)
    Q_PROPERTY(bool customResolutionActive READ customResolutionActive NOTIFY configChanged FINAL)
    Q_PROPERTY(int customWidth READ customWidth WRITE setCustomWidth NOTIFY configChanged FINAL)
    Q_PROPERTY(int customHeight READ customHeight WRITE setCustomHeight NOTIFY configChanged FINAL)
    Q_PROPERTY(QString customResolutionValidation READ customResolutionValidation NOTIFY configChanged FINAL)
    Q_PROPERTY(bool splitByTimeEnabled READ splitByTimeEnabled WRITE setSplitByTimeEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList splitModeOptions READ splitModeOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int splitMode READ splitMode WRITE setSplitMode NOTIFY configChanged FINAL)
    Q_PROPERTY(bool splitCustomIntervalActive READ splitCustomIntervalActive NOTIFY configChanged FINAL)
    Q_PROPERTY(int splitCustomMinutes READ splitCustomMinutes WRITE setSplitCustomMinutes NOTIFY configChanged FINAL)
    Q_PROPERTY(bool splitBySizeEnabled READ splitBySizeEnabled WRITE setSplitBySizeEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(int splitCustomSizeMb READ splitCustomSizeMb WRITE setSplitCustomSizeMb NOTIFY configChanged FINAL)
    Q_PROPERTY(QString splitSummary READ splitSummary NOTIFY configChanged FINAL)

    // ---- Audio --------------------------------------------------------------
    Q_PROPERTY(bool appAudioVisible READ appAudioVisible NOTIFY configChanged FINAL)
    Q_PROPERTY(bool appAudioEnabled READ appAudioEnabled WRITE setAppAudioEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(bool appAudioSeparate READ appAudioSeparate WRITE setAppAudioSeparate NOTIFY configChanged FINAL)
    Q_PROPERTY(bool systemAudioEnabled READ systemAudioEnabled WRITE setSystemAudioEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(
        bool systemAudioSeparate READ systemAudioSeparate WRITE setSystemAudioSeparate NOTIFY configChanged FINAL)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled WRITE setMicrophoneEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(bool microphoneSeparate READ microphoneSeparate WRITE setMicrophoneSeparate NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList microphoneDeviceOptions READ microphoneDeviceOptions NOTIFY microphoneDevicesChanged FINAL)
    Q_PROPERTY(
        QString microphoneDeviceId READ microphoneDeviceId WRITE setMicrophoneDeviceId NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList micChannelModeOptions READ micChannelModeOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int micChannelMode READ micChannelMode WRITE setMicChannelMode NOTIFY configChanged FINAL)
    Q_PROPERTY(double micGainDb READ micGainDb WRITE setMicGainDb NOTIFY configChanged FINAL)
    Q_PROPERTY(int audioBitrateKbps READ audioBitrateKbps WRITE setAudioBitrateKbps NOTIFY configChanged FINAL)
    Q_PROPERTY(bool audioBitrateRelevant READ audioBitrateRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList audioSampleRateOptions READ audioSampleRateOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int audioSampleRate READ audioSampleRate WRITE setAudioSampleRate NOTIFY configChanged FINAL)
    Q_PROPERTY(bool audioSampleRateRelevant READ audioSampleRateRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList audioChannelsOptions READ audioChannelsOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int audioChannels READ audioChannels WRITE setAudioChannels NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList audioBitDepthOptions READ audioBitDepthOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int audioBitDepth READ audioBitDepth WRITE setAudioBitDepth NOTIFY configChanged FINAL)
    Q_PROPERTY(bool audioBitDepthRelevant READ audioBitDepthRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(
        int flacCompressionLevel READ flacCompressionLevel WRITE setFlacCompressionLevel NOTIFY configChanged FINAL)
    Q_PROPERTY(bool flacCompressionRelevant READ flacCompressionRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(bool limiterEnabled READ limiterEnabled WRITE setLimiterEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(double limiterCeilingDb READ limiterCeilingDb WRITE setLimiterCeilingDb NOTIFY configChanged FINAL)
    Q_PROPERTY(
        bool clockSlavingEnabled READ clockSlavingEnabled WRITE setClockSlavingEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList opusFrameDurationOptions READ opusFrameDurationOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int opusFrameDuration READ opusFrameDuration WRITE setOpusFrameDuration NOTIFY configChanged FINAL)
    Q_PROPERTY(int opusComplexity READ opusComplexity WRITE setOpusComplexity NOTIFY configChanged FINAL)
    Q_PROPERTY(bool opusControlsRelevant READ opusControlsRelevant NOTIFY configChanged FINAL)
    Q_PROPERTY(bool micHpfEnabled READ micHpfEnabled WRITE setMicHpfEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(double micHpfCutoffHz READ micHpfCutoffHz WRITE setMicHpfCutoffHz NOTIFY configChanged FINAL)
    Q_PROPERTY(bool micGateEnabled READ micGateEnabled WRITE setMicGateEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(double micGateThresholdDb READ micGateThresholdDb WRITE setMicGateThresholdDb NOTIFY configChanged FINAL)
    Q_PROPERTY(bool micAgcEnabled READ micAgcEnabled WRITE setMicAgcEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(double micAgcTargetDb READ micAgcTargetDb WRITE setMicAgcTargetDb NOTIFY configChanged FINAL)
    Q_PROPERTY(bool micRnnoiseEnabled READ micRnnoiseEnabled WRITE setMicRnnoiseEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(double systemMeter READ systemMeter NOTIFY metersChanged FINAL)
    Q_PROPERTY(double appMeter READ appMeter NOTIFY metersChanged FINAL)
    Q_PROPERTY(double microphoneMeter READ microphoneMeter NOTIFY metersChanged FINAL)
    // The same readings as decibels, which is what the rows print beside the
    // bar. Negative infinity means the source produced nothing at all -- not the
    // same statement as a level sitting at the floor.
    Q_PROPERTY(double systemMeterDb READ systemMeterDb NOTIFY metersChanged FINAL)
    Q_PROPERTY(double appMeterDb READ appMeterDb NOTIFY metersChanged FINAL)
    Q_PROPERTY(double microphoneMeterDb READ microphoneMeterDb NOTIFY metersChanged FINAL)
    Q_PROPERTY(QString micPostProcessingSummary READ micPostProcessingSummary NOTIFY configChanged FINAL)
    Q_PROPERTY(QString audioEncodingSummary READ audioEncodingSummary NOTIFY configChanged FINAL)
    Q_PROPERTY(QString audioSummary READ audioSummary NOTIFY configChanged FINAL)

    // ---- Notifications & overlays ------------------------------------------
    Q_PROPERTY(bool showRecordingOverlay READ showRecordingOverlay WRITE setShowRecordingOverlay NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool showDiagnosticsOverlay READ showDiagnosticsOverlay WRITE setShowDiagnosticsOverlay NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool showNotifications READ showNotifications WRITE setShowNotifications NOTIFY appSettingsChanged FINAL)

    // Overlay content. The preset properties carry the persisted token; the
    // element properties are the RESOLVED set the overlay will actually draw,
    // so a checkbox bound to one shows the truth under every preset instead of
    // a stale custom list. They are read-only for that reason — writing goes
    // through setOverlayElement(), which is what promotes an edit made while a
    // named preset is selected into Custom.
    Q_PROPERTY(QVariantList recordingOverlayPresetOptions READ recordingOverlayPresetOptions CONSTANT FINAL)
    Q_PROPERTY(QString recordingOverlayPreset READ recordingOverlayPreset WRITE setRecordingOverlayPreset NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool recordingOverlayElapsed READ recordingOverlayElapsed NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool recordingOverlayOutputSize READ recordingOverlayOutputSize NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool recordingOverlaySourceName READ recordingOverlaySourceName NOTIFY appSettingsChanged FINAL)

    Q_PROPERTY(QVariantList diagnosticsOverlayPresetOptions READ diagnosticsOverlayPresetOptions CONSTANT FINAL)
    Q_PROPERTY(QString diagnosticsOverlayPreset READ diagnosticsOverlayPreset WRITE setDiagnosticsOverlayPreset NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool diagnosticsOverlayFps READ diagnosticsOverlayFps NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool diagnosticsOverlayDrop READ diagnosticsOverlayDrop NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool diagnosticsOverlayDrift READ diagnosticsOverlayDrift NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool diagnosticsOverlaySize READ diagnosticsOverlaySize NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool diagnosticsOverlayMutedSources READ diagnosticsOverlayMutedSources NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool showQuickControls READ showQuickControls WRITE setShowQuickControls NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool hideWindowFromCapture READ hideWindowFromCapture WRITE setHideWindowFromCapture NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool openEditorWhenFinished READ openEditorWhenFinished WRITE setOpenEditorWhenFinished NOTIFY
                   appSettingsChanged FINAL)
    Q_PROPERTY(bool presentDiagnosticsOptIn READ presentDiagnosticsOptIn WRITE setPresentDiagnosticsOptIn NOTIFY
                   appSettingsChanged FINAL)

    // ---- Appearance ---------------------------------------------------------
    Q_PROPERTY(QVariantList appearanceOptions READ appearanceOptions CONSTANT FINAL)
    Q_PROPERTY(QString appearanceId READ appearanceId WRITE setAppearanceId NOTIFY appSettingsChanged FINAL)
    // Not CONSTANT: each entry carries the accent's swatch as it looks in the
    // CURRENT appearance, so the list has to be re-read when that changes.
    Q_PROPERTY(QVariantList accentOptions READ accentOptions NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(QString accentId READ accentId WRITE setAccentId NOTIFY appSettingsChanged FINAL)

    // ---- Developer ----------------------------------------------------------
    Q_PROPERTY(QVariantList logLevelOptions READ logLevelOptions CONSTANT FINAL)
    Q_PROPERTY(
        QString developerLogLevel READ developerLogLevel WRITE setDeveloperLogLevel NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(QVariantList crashReportPolicyOptions READ crashReportPolicyOptions CONSTANT FINAL)
    Q_PROPERTY(int crashReportPolicy READ crashReportPolicy WRITE setCrashReportPolicy NOTIFY appSettingsChanged FINAL)

    // ---- Updates ------------------------------------------------------------
    Q_PROPERTY(QVariantList updateChannelOptions READ updateChannelOptions CONSTANT FINAL)
    Q_PROPERTY(QString updateChannel READ updateChannel WRITE setUpdateChannel NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(bool autoUpdateCheck READ autoUpdateCheck WRITE setAutoUpdateCheck NOTIFY appSettingsChanged FINAL)
    Q_PROPERTY(QString updateState READ updateState NOTIFY updateStatusChanged FINAL)
    Q_PROPERTY(QString updateStatusText READ updateStatusText NOTIFY updateStatusChanged FINAL)
    Q_PROPERTY(QString updateActionText READ updateActionText NOTIFY updateStatusChanged FINAL)
    Q_PROPERTY(bool updateActionEnabled READ updateActionEnabled NOTIFY updateStatusChanged FINAL)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateStatusChanged FINAL)
    Q_PROPERTY(bool whatsNewAvailable READ whatsNewAvailable NOTIFY updateStatusChanged FINAL)
    // The offered release tag verbatim. Read by the card's "See what's new in
    // vX.Y" link, which the spec requires to name the version rather than to say
    // "What's new" about an unnamed one.
    Q_PROPERTY(QString updateAvailableVersion READ updateAvailableVersion NOTIFY updateStatusChanged FINAL)

    // ---- Webcam -------------------------------------------------------------
    Q_PROPERTY(bool webcamEnabled READ webcamEnabled WRITE setWebcamEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList webcamDeviceOptions READ webcamDeviceOptions NOTIFY webcamDevicesChanged FINAL)
    Q_PROPERTY(QString webcamDeviceId READ webcamDeviceId WRITE setWebcamDeviceId NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList webcamResolutionOptions READ webcamResolutionOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int webcamResolution READ webcamResolution WRITE setWebcamResolution NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList webcamFrameRateOptions READ webcamFrameRateOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int webcamFrameRate READ webcamFrameRate WRITE setWebcamFrameRate NOTIFY configChanged FINAL)
    Q_PROPERTY(bool webcamMirror READ webcamMirror WRITE setWebcamMirror NOTIFY configChanged FINAL)
    Q_PROPERTY(double webcamOpacity READ webcamOpacity WRITE setWebcamOpacity NOTIFY configChanged FINAL)
    Q_PROPERTY(bool webcamAvailable READ webcamAvailable NOTIFY webcamDevicesChanged FINAL)
    Q_PROPERTY(bool chromaKeyEnabled READ chromaKeyEnabled WRITE setChromaKeyEnabled NOTIFY configChanged FINAL)
    Q_PROPERTY(QVariantList chromaKeyColorOptions READ chromaKeyColorOptions NOTIFY optionsChanged FINAL)
    Q_PROPERTY(int chromaKeyColorMode READ chromaKeyColorMode WRITE setChromaKeyColorMode NOTIFY configChanged FINAL)
    Q_PROPERTY(int chromaKeyTolerance READ chromaKeyTolerance WRITE setChromaKeyTolerance NOTIFY configChanged FINAL)
    Q_PROPERTY(int chromaKeySoftness READ chromaKeySoftness WRITE setChromaKeySoftness NOTIFY configChanged FINAL)
    Q_PROPERTY(int chromaKeySpill READ chromaKeySpill WRITE setChromaKeySpill NOTIFY configChanged FINAL)

    // ---- Hotkeys ------------------------------------------------------------
    Q_PROPERTY(QVariantList hotkeyRows READ hotkeyRows NOTIFY hotkeysChanged FINAL)
    Q_PROPERTY(int capturingHotkeyAction READ capturingHotkeyAction NOTIFY hotkeysChanged FINAL)
    Q_PROPERTY(QString hotkeyErrorText READ hotkeyErrorText NOTIFY hotkeysChanged FINAL)
    Q_PROPERTY(int hotkeyErrorAction READ hotkeyErrorAction NOTIFY hotkeysChanged FINAL)

    // ---- Presets ------------------------------------------------------------
    Q_PROPERTY(QVariantList presetOptions READ presetOptions NOTIFY presetsChanged FINAL)
    Q_PROPERTY(QString selectedPresetId READ selectedPresetId NOTIFY presetsChanged FINAL)
    Q_PROPERTY(QString selectedPresetName READ selectedPresetName NOTIFY presetsChanged FINAL)
    Q_PROPERTY(bool presetDirty READ presetDirty NOTIFY presetsChanged FINAL)
    Q_PROPERTY(bool presetBuiltIn READ presetBuiltIn NOTIFY presetsChanged FINAL)
    Q_PROPERTY(QString presetStatusText READ presetStatusText NOTIFY presetsChanged FINAL)

  public:
    explicit SettingsAdapter(QObject* parent = nullptr);

    void setOutputFolderValidator(OutputFolderValidator validator);
    void requestOutputValidation(OutputValidationTrigger trigger);
    void applyOutputFolderValidation(FolderValidationResult result);
    void requestSettingsFocus(FocusTarget target);

    // ---- Composition-root seams (never called from QML) ---------------------
    void setConfig(RecordingPresetConfig config);
    [[nodiscard]] const RecordingPresetConfig& config() const noexcept;
    void setCapabilities(const capability::CapabilitySet& caps);
    void setAppSettings(const PersistedAppSettings& settings);
    [[nodiscard]] const PersistedAppSettings& appSettings() const noexcept;
    void setControlsLocked(bool locked);
    void setMaxFrameRate(int max_fps);
    void setMicrophoneDevices(QVariantList devices);
    void setWebcamDevices(QVariantList devices);
    // Dock-level (0..1) meter values, forwarded from the same computation that
    // drives the Record page so both areas can never disagree.
    // In dBFS, not in meter positions: this adapter owns the conversion so the
    // bar and the number can never disagree.
    void setMeters(double system_dbfs, double app_dbfs, double microphone_dbfs);
    void setHdrDisplayPresent(bool present);
    void setPresetState(QVariantList options, QString selected_id, bool dirty);
    // rows: { action, label, binding, isDefault } per hotkey action.
    void setHotkeyRows(QVariantList rows);
    void setHotkeyError(int action, QString message);
    void setUpdateStatus(const QString& state, const QString& available_version, const QString& last_checked,
                         const QString& detail = QString());

    // ---- Property readers ---------------------------------------------------
    [[nodiscard]] bool expertMode() const noexcept;
    [[nodiscard]] bool controlsLocked() const noexcept;
    [[nodiscard]] QString encodeAdapterName() const;

    [[nodiscard]] const QVariantList& containerOptions() const noexcept;
    [[nodiscard]] int container() const noexcept;
    [[nodiscard]] const QVariantList& videoCodecOptions() const noexcept;
    [[nodiscard]] int videoCodec() const noexcept;
    [[nodiscard]] const QVariantList& audioCodecOptions() const noexcept;
    [[nodiscard]] int audioCodec() const noexcept;
    [[nodiscard]] const QVariantList& bitDepthOptions() const noexcept;
    [[nodiscard]] int bitDepth() const noexcept;
    [[nodiscard]] const QVariantList& chromaOptions() const noexcept;
    [[nodiscard]] int chroma() const noexcept;
    [[nodiscard]] const QString& chromaHint() const noexcept;
    [[nodiscard]] const QVariantList& colorRangeOptions() const noexcept;
    [[nodiscard]] int colorRange() const noexcept;
    [[nodiscard]] const QVariantList& hdrModeOptions() const noexcept;
    [[nodiscard]] int hdrMode() const noexcept;
    [[nodiscard]] const QString& hdrHint() const noexcept;
    [[nodiscard]] bool hdrRelevant() const noexcept;
    [[nodiscard]] const QVariantList& encoderPresetOptions() const noexcept;
    [[nodiscard]] int encoderPreset() const noexcept;
    [[nodiscard]] const QString& formatSummary() const noexcept;
    [[nodiscard]] const QString& compatNotice() const noexcept;
    [[nodiscard]] bool compatOk() const noexcept;

    [[nodiscard]] const QVariantList& qualityPresetOptions() const noexcept;
    [[nodiscard]] int qualityPreset() const noexcept;
    [[nodiscard]] int cq() const noexcept;
    [[nodiscard]] const QVariantList& rateControlOptions() const noexcept;
    [[nodiscard]] int rateControl() const noexcept;
    [[nodiscard]] bool bitrateRelevant() const noexcept;
    // The quantizer the selected codec is actually configured with for the
    // current CQ, named in that codec's own domain. The Expert CQ field shows
    // it because the CQ number itself is a product scale, not the encoder's.
    [[nodiscard]] QString nativeQuantizerHint() const;
    [[nodiscard]] int bitrateKbps() const noexcept;
    [[nodiscard]] const QVariantList& frameRateOptions() const noexcept;
    [[nodiscard]] int frameRate() const noexcept;
    [[nodiscard]] int maxFrameRate() const noexcept;
    [[nodiscard]] const QVariantList& timingOptions() const noexcept;
    [[nodiscard]] bool cfr() const noexcept;
    [[nodiscard]] const QVariantList& framePacingOptions() const noexcept;
    [[nodiscard]] int framePacing() const noexcept;
    [[nodiscard]] const QVariantList& keyframeIntervalOptions() const noexcept;
    [[nodiscard]] int keyframeInterval() const noexcept;
    [[nodiscard]] bool captureCursor() const noexcept;

    [[nodiscard]] QString outputFolder() const;
    [[nodiscard]] QString namingPattern() const;
    [[nodiscard]] const QString& exampleFilename() const noexcept;
    [[nodiscard]] const QString& outputSummary() const noexcept;
    [[nodiscard]] const QString& savesToText() const noexcept;
    [[nodiscard]] const QString& folderValidation() const noexcept;
    [[nodiscard]] const QString& patternValidation() const noexcept;
    [[nodiscard]] QVariantList filenameTokens() const;
    [[nodiscard]] const QVariantList& resolutionOptions() const noexcept;
    [[nodiscard]] int resolutionMode() const noexcept;
    [[nodiscard]] bool customResolutionActive() const noexcept;
    [[nodiscard]] int customWidth() const noexcept;
    [[nodiscard]] int customHeight() const noexcept;
    [[nodiscard]] const QString& customResolutionValidation() const noexcept;
    [[nodiscard]] bool splitByTimeEnabled() const noexcept;
    [[nodiscard]] const QVariantList& splitModeOptions() const noexcept;
    [[nodiscard]] int splitMode() const noexcept;
    [[nodiscard]] bool splitCustomIntervalActive() const noexcept;
    [[nodiscard]] int splitCustomMinutes() const noexcept;
    [[nodiscard]] bool splitBySizeEnabled() const noexcept;
    [[nodiscard]] int splitCustomSizeMb() const noexcept;
    [[nodiscard]] const QString& splitSummary() const noexcept;

    [[nodiscard]] bool appAudioVisible() const noexcept;
    [[nodiscard]] bool appAudioEnabled() const noexcept;
    [[nodiscard]] bool appAudioSeparate() const noexcept;
    [[nodiscard]] bool systemAudioEnabled() const noexcept;
    [[nodiscard]] bool systemAudioSeparate() const noexcept;
    [[nodiscard]] bool microphoneEnabled() const noexcept;
    [[nodiscard]] bool microphoneSeparate() const noexcept;
    [[nodiscard]] const QVariantList& microphoneDeviceOptions() const noexcept;
    [[nodiscard]] QString microphoneDeviceId() const;
    [[nodiscard]] const QVariantList& micChannelModeOptions() const noexcept;
    [[nodiscard]] int micChannelMode() const noexcept;
    [[nodiscard]] double micGainDb() const noexcept;
    [[nodiscard]] int audioBitrateKbps() const noexcept;
    [[nodiscard]] bool audioBitrateRelevant() const noexcept;
    [[nodiscard]] const QVariantList& audioSampleRateOptions() const noexcept;
    [[nodiscard]] int audioSampleRate() const noexcept;
    [[nodiscard]] bool audioSampleRateRelevant() const noexcept;
    [[nodiscard]] const QVariantList& audioChannelsOptions() const noexcept;
    [[nodiscard]] int audioChannels() const noexcept;
    [[nodiscard]] const QVariantList& audioBitDepthOptions() const noexcept;
    [[nodiscard]] int audioBitDepth() const noexcept;
    [[nodiscard]] bool audioBitDepthRelevant() const noexcept;
    [[nodiscard]] int flacCompressionLevel() const noexcept;
    [[nodiscard]] bool flacCompressionRelevant() const noexcept;
    [[nodiscard]] bool limiterEnabled() const noexcept;
    [[nodiscard]] double limiterCeilingDb() const noexcept;
    [[nodiscard]] bool clockSlavingEnabled() const noexcept;
    [[nodiscard]] const QVariantList& opusFrameDurationOptions() const noexcept;
    [[nodiscard]] int opusFrameDuration() const noexcept;
    [[nodiscard]] int opusComplexity() const noexcept;
    [[nodiscard]] bool opusControlsRelevant() const noexcept;
    [[nodiscard]] bool micHpfEnabled() const noexcept;
    [[nodiscard]] double micHpfCutoffHz() const noexcept;
    [[nodiscard]] bool micGateEnabled() const noexcept;
    [[nodiscard]] double micGateThresholdDb() const noexcept;
    [[nodiscard]] bool micAgcEnabled() const noexcept;
    [[nodiscard]] double micAgcTargetDb() const noexcept;
    [[nodiscard]] bool micRnnoiseEnabled() const noexcept;
    [[nodiscard]] double systemMeter() const noexcept;
    [[nodiscard]] double appMeter() const noexcept;
    [[nodiscard]] double microphoneMeter() const noexcept;
    [[nodiscard]] double systemMeterDb() const noexcept;
    [[nodiscard]] double appMeterDb() const noexcept;
    [[nodiscard]] double microphoneMeterDb() const noexcept;
    [[nodiscard]] const QString& micPostProcessingSummary() const noexcept;
    [[nodiscard]] const QString& audioEncodingSummary() const noexcept;
    [[nodiscard]] const QString& audioSummary() const noexcept;

    [[nodiscard]] bool showRecordingOverlay() const noexcept;
    [[nodiscard]] bool showDiagnosticsOverlay() const noexcept;
    [[nodiscard]] bool showNotifications() const noexcept;
    [[nodiscard]] bool showQuickControls() const noexcept;
    [[nodiscard]] bool minimizeToTray() const noexcept;
    [[nodiscard]] bool hideWindowFromCapture() const noexcept;
    [[nodiscard]] bool openEditorWhenFinished() const noexcept;
    [[nodiscard]] bool presentDiagnosticsOptIn() const noexcept;

    [[nodiscard]] QVariantList recordingOverlayPresetOptions() const;
    [[nodiscard]] QString recordingOverlayPreset() const;
    [[nodiscard]] bool recordingOverlayElapsed() const;
    [[nodiscard]] bool recordingOverlayOutputSize() const;
    [[nodiscard]] bool recordingOverlaySourceName() const;
    [[nodiscard]] QVariantList diagnosticsOverlayPresetOptions() const;
    [[nodiscard]] QString diagnosticsOverlayPreset() const;
    [[nodiscard]] bool diagnosticsOverlayFps() const;
    [[nodiscard]] bool diagnosticsOverlayDrop() const;
    [[nodiscard]] bool diagnosticsOverlayDrift() const;
    [[nodiscard]] bool diagnosticsOverlaySize() const;
    [[nodiscard]] bool diagnosticsOverlayMutedSources() const;

    // Toggling a single element. The current RESOLVED set is the starting point,
    // so unticking one token under Technical yields "Technical minus that token"
    // as a Custom set, rather than reverting to whatever the custom list held
    // before. `token` is a models::OverlayContentPolicy element token; an
    // unknown one is ignored.
    Q_INVOKABLE void setRecordingOverlayElement(const QString& token, bool enabled);
    Q_INVOKABLE void setDiagnosticsOverlayElement(const QString& token, bool enabled);

    [[nodiscard]] QVariantList appearanceOptions() const;
    [[nodiscard]] QString appearanceId() const;
    [[nodiscard]] QVariantList accentOptions() const;
    [[nodiscard]] QString accentId() const;
    [[nodiscard]] QVariantList logLevelOptions() const;
    [[nodiscard]] QString developerLogLevel() const;
    [[nodiscard]] QVariantList crashReportPolicyOptions() const;
    [[nodiscard]] int crashReportPolicy() const noexcept;

    [[nodiscard]] QVariantList updateChannelOptions() const;
    [[nodiscard]] QString updateChannel() const;
    [[nodiscard]] bool autoUpdateCheck() const noexcept;
    [[nodiscard]] const QString& updateState() const noexcept;
    [[nodiscard]] const QString& updateStatusText() const noexcept;
    [[nodiscard]] const QString& updateActionText() const noexcept;
    [[nodiscard]] bool updateActionEnabled() const noexcept;
    [[nodiscard]] bool updateAvailable() const noexcept;
    // The offered release tag verbatim, empty when nothing is offered. Already
    // held for the card's copy; exposed because it is the string that becomes
    // the updater's pinned target version.
    [[nodiscard]] const QString& updateAvailableVersion() const noexcept;
    [[nodiscard]] bool whatsNewAvailable() const noexcept;

    [[nodiscard]] bool webcamEnabled() const noexcept;
    [[nodiscard]] const QVariantList& webcamDeviceOptions() const noexcept;
    [[nodiscard]] QString webcamDeviceId() const;
    [[nodiscard]] const QVariantList& webcamResolutionOptions() const noexcept;
    [[nodiscard]] int webcamResolution() const noexcept;
    [[nodiscard]] const QVariantList& webcamFrameRateOptions() const noexcept;
    [[nodiscard]] int webcamFrameRate() const noexcept;
    [[nodiscard]] bool webcamMirror() const noexcept;
    [[nodiscard]] double webcamOpacity() const noexcept;
    [[nodiscard]] bool webcamAvailable() const noexcept;
    [[nodiscard]] bool chromaKeyEnabled() const noexcept;
    [[nodiscard]] const QVariantList& chromaKeyColorOptions() const noexcept;
    [[nodiscard]] int chromaKeyColorMode() const noexcept;
    [[nodiscard]] int chromaKeyTolerance() const noexcept;
    [[nodiscard]] int chromaKeySoftness() const noexcept;
    [[nodiscard]] int chromaKeySpill() const noexcept;
    [[nodiscard]] const QVariantList& hotkeyRows() const noexcept;
    [[nodiscard]] int capturingHotkeyAction() const noexcept;
    [[nodiscard]] const QString& hotkeyErrorText() const noexcept;
    [[nodiscard]] int hotkeyErrorAction() const noexcept;
    [[nodiscard]] const QVariantList& presetOptions() const noexcept;
    [[nodiscard]] const QString& selectedPresetId() const noexcept;
    [[nodiscard]] const QString& selectedPresetName() const noexcept;
    [[nodiscard]] bool presetDirty() const noexcept;
    [[nodiscard]] bool presetBuiltIn() const noexcept;
    [[nodiscard]] const QString& presetStatusText() const noexcept;

    // ---- Property writers ---------------------------------------------------
    void setExpertMode(bool enabled);
    void setContainer(int value);
    void setVideoCodec(int value);
    void setAudioCodec(int value);
    void setBitDepth(int value);
    void setChroma(int value);
    void setColorRange(int value);
    void setHdrMode(int value);
    void setEncoderPreset(int value);
    void setQualityPreset(int value);
    void setCq(int value);
    void setRateControl(int value);
    void setBitrateKbps(int value);
    void setFrameRate(int value);
    void setCfr(bool value);
    void setFramePacing(int value);
    void setKeyframeInterval(int value);
    void setCaptureCursor(bool value);
    void setOutputFolder(const QString& value);
    void setNamingPattern(const QString& value);
    void setResolutionMode(int value);
    void setCustomWidth(int value);
    void setCustomHeight(int value);
    void setSplitByTimeEnabled(bool value);
    void setSplitMode(int value);
    void setSplitCustomMinutes(int value);
    void setSplitBySizeEnabled(bool value);
    void setSplitCustomSizeMb(int value);
    void setAppAudioEnabled(bool value);
    void setAppAudioSeparate(bool value);
    void setSystemAudioEnabled(bool value);
    void setSystemAudioSeparate(bool value);
    void setMicrophoneEnabled(bool value);
    void setMicrophoneSeparate(bool value);
    void setMicrophoneDeviceId(const QString& value);
    void setMicChannelMode(int value);
    void setMicGainDb(double value);
    void setAudioBitrateKbps(int value);
    void setAudioSampleRate(int value);
    void setAudioChannels(int value);
    void setAudioBitDepth(int value);
    void setFlacCompressionLevel(int value);
    void setLimiterEnabled(bool value);
    void setLimiterCeilingDb(double value);
    void setClockSlavingEnabled(bool value);
    void setOpusFrameDuration(int value);
    void setOpusComplexity(int value);
    void setMicHpfEnabled(bool value);
    void setMicHpfCutoffHz(double value);
    void setMicGateEnabled(bool value);
    void setMicGateThresholdDb(double value);
    void setMicAgcEnabled(bool value);
    void setMicAgcTargetDb(double value);
    void setMicRnnoiseEnabled(bool value);
    void setWebcamEnabled(bool value);
    void setWebcamDeviceId(const QString& value);
    void setWebcamResolution(int value);
    void setWebcamFrameRate(int value);
    void setWebcamMirror(bool value);
    void setWebcamOpacity(double value);
    void setChromaKeyEnabled(bool value);
    void setChromaKeyColorMode(int value);
    void setChromaKeyTolerance(int value);
    void setChromaKeySoftness(int value);
    void setChromaKeySpill(int value);
    void setShowRecordingOverlay(bool value);
    void setShowDiagnosticsOverlay(bool value);
    void setShowNotifications(bool value);
    void setRecordingOverlayPreset(const QString& value);
    void setDiagnosticsOverlayPreset(const QString& value);
    void setShowQuickControls(bool value);
    void setMinimizeToTray(bool value);
    void setHideWindowFromCapture(bool value);
    void setOpenEditorWhenFinished(bool value);
    void setPresentDiagnosticsOptIn(bool value);
    void setAppearanceId(const QString& value);
    void setAccentId(const QString& value);
    void setDeveloperLogLevel(const QString& value);
    void setCrashReportPolicy(int value);
    void setUpdateChannel(const QString& value);
    void setAutoUpdateCheck(bool value);

    // ---- Commands -----------------------------------------------------------
    Q_INVOKABLE void selectPreset(const QString& id);
    Q_INVOKABLE void savePresetAs(const QString& name);
    Q_INVOKABLE void renamePreset(const QString& name);
    Q_INVOKABLE void deletePreset();
    Q_INVOKABLE void resetChanges();
    // Paths arrive as file: URLs from QtQuick.Dialogs' native dialogs. The URL
    // -> path conversion stays in C++ so QML never assembles a filesystem path,
    // and the shipping Quick frontend never links Qt Widgets for a file dialog.
    Q_INVOKABLE void exportPresetToUrl(const QUrl& url);
    Q_INVOKABLE void importPresetsFromUrl(const QUrl& url);
    Q_INVOKABLE void setOutputFolderFromUrl(const QUrl& url);
    Q_INVOKABLE void rescanAudioDevices();
    // Hotkey rebinding. The adapter only carries intent; validation, conflict
    // detection and Win32 registration stay in GlobalHotkeyService.
    Q_INVOKABLE void beginHotkeyCapture(int action);
    Q_INVOKABLE void cancelHotkeyCapture();
    Q_INVOKABLE void commitHotkeyCapture(int key, int modifiers);
    Q_INVOKABLE void clearHotkey(int action);
    Q_INVOKABLE void resetHotkey(int action);
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void runUpdatePrimaryAction();
    Q_INVOKABLE void showWhatsNew();
    Q_INVOKABLE void openDiagnostics();
    // True when `name` is empty after trimming or collides with an existing
    // preset label other than `exclude_id`'s. Uniqueness folding delegates to
    // the shared FoldPresetName rule, so both frontends reject the same names.
    Q_INVOKABLE bool presetNameRejected(const QString& name, const QString& exclude_id) const;

  signals:
    void configChanged();
    void optionsChanged();
    void appSettingsChanged();
    void controlsLockedChanged();
    void microphoneDevicesChanged();
    void webcamDevicesChanged();
    void metersChanged();
    void updateStatusChanged();
    void presetsChanged();
    void hotkeysChanged();

    void hotkeyRebindRequested(int action, int key, int modifiers);
    void hotkeyClearRequested(int action);
    void hotkeyResetRequested(int action);

    // Emitted after any user edit to the live recording configuration. The
    // composition root persists it and mirrors it into the recording side.
    void configEdited();
    // Emitted after any user edit to the persisted application settings.
    void appSettingsEdited();

    void presetSelected(QString id);
    void savePresetAsRequested(QString name);
    void renamePresetRequested(QString name);
    void deletePresetRequested();
    void resetChangesRequested();
    void exportPresetRequested(QString path);
    void importPresetsRequested(QString path);
    void audioRescanRequested();
    void checkForUpdatesRequested();
    void updatePrimaryActionRequested();
    void whatsNewRequested();
    void diagnosticsRequested();
    void outputValidationRequested(OutputValidationTrigger trigger);
    void outputValidationFinished(FolderValidationResult result);
    void settingsFocusRequested(FocusTarget target);

  private:
    // Re-runs shared reconciliation + sanitization over the live config, then
    // rebuilds the capability-gated option lists and derived presentation text.
    void applyConfigEdit();
    void rebuildOptions();
    void rebuildDerivedText();
    void commitAppSettingsEdit();
    [[nodiscard]] models::RecordingOverlayContent resolvedRecordingOverlayContent() const;
    [[nodiscard]] models::DiagnosticsOverlayContent resolvedDiagnosticsOverlayContent() const;
    [[nodiscard]] exosnap::engine::AudioSourceRow* findRow(exosnap::engine::AudioSourceKind kind);
    [[nodiscard]] const exosnap::engine::AudioSourceRow* findRow(exosnap::engine::AudioSourceKind kind) const;
    void setRowEnabled(exosnap::engine::AudioSourceKind kind, bool enabled);
    void setRowSeparate(exosnap::engine::AudioSourceKind kind, bool separate);

    RecordingPresetConfig config_;
    PersistedAppSettings app_settings_;
    capability::CapabilitySet caps_;
    bool caps_set_ = false;
    bool controls_locked_ = false;
    bool hdr_display_present_ = false;
    int max_frame_rate_ = 0;

    // SanitizeOutputResolution rejects an incomplete custom size by snapping the
    // mode back to Native, so "Custom selected, dimensions not entered yet" is
    // not representable in the model. These three fields hold that transient
    // editing state in the presentation layer; the sanitizer stays the single
    // owner of which sizes are actually valid.
    bool custom_resolution_pending_ = false;
    uint32_t pending_custom_width_ = 0;
    uint32_t pending_custom_height_ = 0;

    QVariantList container_options_;
    QVariantList video_codec_options_;
    QVariantList audio_codec_options_;
    QVariantList bit_depth_options_;
    QVariantList chroma_options_;
    QVariantList color_range_options_;
    QVariantList hdr_mode_options_;
    QVariantList encoder_preset_options_;
    QVariantList quality_preset_options_;
    QVariantList rate_control_options_;
    QVariantList frame_rate_options_;
    QVariantList timing_options_;
    QVariantList frame_pacing_options_;
    QVariantList keyframe_interval_options_;
    QVariantList resolution_options_;
    QVariantList split_mode_options_;
    QVariantList mic_channel_mode_options_;
    QVariantList audio_sample_rate_options_;
    QVariantList audio_channels_options_;
    QVariantList audio_bit_depth_options_;
    QVariantList opus_frame_duration_options_;
    QVariantList microphone_devices_;
    QVariantList webcam_devices_;
    QVariantList webcam_resolution_options_;
    QVariantList webcam_frame_rate_options_;
    QVariantList chroma_key_color_options_;

    QString chroma_hint_;
    QString hdr_hint_;
    QString format_summary_;
    QString compat_notice_;
    QString example_filename_;
    QString output_summary_;
    QString saves_to_text_;
    QString folder_validation_;
    QString pattern_validation_;
    QString custom_resolution_validation_;
    QString split_summary_;
    QString mic_post_processing_summary_;
    QString audio_encoding_summary_;
    QString audio_summary_;
    OutputFolderValidator output_folder_validator_ = ValidateOutputFolder;
    std::atomic<uint64_t> output_validation_revision_{0};

    QVariantList hotkey_rows_;
    int capturing_hotkey_action_ = -1;
    int hotkey_error_action_ = -1;
    QString hotkey_error_text_;

    QVariantList preset_options_;
    QString selected_preset_id_;
    QString selected_preset_name_;
    QString preset_status_text_;
    bool preset_dirty_ = false;
    bool preset_built_in_ = false;

    // Stored as decibels; the 0..1 positions are derived on read. Keeping both
    // as state would let them drift apart, which is the defect this replaced.
    double system_meter_db_ = -std::numeric_limits<double>::infinity();
    double app_meter_db_ = -std::numeric_limits<double>::infinity();
    double microphone_meter_db_ = -std::numeric_limits<double>::infinity();

    QString update_state_ = QStringLiteral("uptodate");
    QString update_status_text_;
    QString update_action_text_;
    QString update_available_version_;
    bool update_action_enabled_ = true;
    bool whats_new_available_ = false;
};

} // namespace exosnap::quick
