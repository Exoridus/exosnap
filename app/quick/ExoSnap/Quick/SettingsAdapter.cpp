#include "SettingsAdapter.h"

#include "QuickThemeTokens.h"
#include "models/FilenameBuilder.h"
#include "models/OutputPathPolicy.h"
#include "models/OutputPathValidator.h"
#include "ui/CodecLabels.h"

#include <capability/container_compat_registry.h>

#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>

namespace exosnap::quick {
namespace {

using capability::AudioCodec;
using capability::BitDepth;
using capability::ChromaSubsampling;
using capability::ColorRange;
using capability::Container;
using capability::VideoCodec;

// One entry of a capability-gated QML option list. `reason` carries the shared
// owner's explanation verbatim so the UI never invents its own wording for why
// an option is unavailable.
QVariant makeOption(int value, const QString& label, bool selectable = true, const QString& reason = QString()) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), value);
    entry.insert(QStringLiteral("label"), label);
    entry.insert(QStringLiteral("selectable"), selectable);
    entry.insert(QStringLiteral("reason"), reason);
    return entry;
}

QVariant makeOption(const QString& value, const QString& label) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), value);
    entry.insert(QStringLiteral("label"), label);
    entry.insert(QStringLiteral("selectable"), true);
    entry.insert(QStringLiteral("reason"), QString());
    return entry;
}

QString fromAnnotation(const capability::SupportAnnotation& annotation) {
    return QString::fromStdString(annotation.reason);
}

QString fromWide(const std::wstring& value) {
    return QString::fromWCharArray(value.c_str(), static_cast<int>(value.size()));
}

QString nvencPresetLabel(recorder_core::NvencPreset preset) {
    switch (preset) {
    case recorder_core::NvencPreset::P1:
        return QObject::tr("P1 — fastest");
    case recorder_core::NvencPreset::P2:
        return QStringLiteral("P2");
    case recorder_core::NvencPreset::P3:
        return QStringLiteral("P3");
    case recorder_core::NvencPreset::P4:
        return QObject::tr("P4 — balanced");
    case recorder_core::NvencPreset::P5:
        return QStringLiteral("P5");
    case recorder_core::NvencPreset::P6:
        return QStringLiteral("P6");
    case recorder_core::NvencPreset::P7:
        return QObject::tr("P7 — best quality");
    }
    return QStringLiteral("P4");
}

QString qualityPresetLabel(recorder_core::QualityPreset preset) {
    switch (preset) {
    case recorder_core::QualityPreset::Ultra:
        return QObject::tr("Ultra");
    case recorder_core::QualityPreset::High:
        return QObject::tr("High");
    case recorder_core::QualityPreset::Balanced:
        return QObject::tr("Balanced");
    case recorder_core::QualityPreset::Efficient:
        return QObject::tr("Efficient");
    case recorder_core::QualityPreset::Draft:
        return QObject::tr("Draft");
    }
    return QObject::tr("Balanced");
}

constexpr std::array kFrameRateLadder = {24u, 30u, 48u, 50u, 60u, 90u, 120u, 144u, 165u, 240u};

bool isSysKind(recorder_core::AudioSourceKind kind) noexcept {
    return kind == recorder_core::AudioSourceKind::Sys || kind == recorder_core::AudioSourceKind::SystemOutput;
}

} // namespace

SettingsAdapter::SettingsAdapter(QObject* parent) : QObject(parent) {
    rebuildOptions();
    rebuildDerivedText();
}

// ---------------------------------------------------------------------------
// Composition-root seams
// ---------------------------------------------------------------------------

void SettingsAdapter::setConfig(RecordingPresetConfig config) {
    config_ = SanitizePresetConfig(std::move(config));
    custom_resolution_pending_ = config_.output.resolution.mode == OutputResolutionMode::Custom;
    pending_custom_width_ = config_.output.resolution.custom_width;
    pending_custom_height_ = config_.output.resolution.custom_height;
    rebuildOptions();
    rebuildDerivedText();
    emit optionsChanged();
    emit configChanged();
}

const RecordingPresetConfig& SettingsAdapter::config() const noexcept {
    return config_;
}

void SettingsAdapter::setCapabilities(const capability::CapabilitySet& caps) {
    caps_ = caps;
    caps_set_ = true;
    // A capability delivery can invalidate a selection that was only permitted
    // by the static baseline (4:4:4 on a GPU without YUV444). Re-running the
    // shared sanitizer is what corrects it -- the adapter never rewrites a
    // field itself.
    config_ = SanitizePresetConfig(std::move(config_));
    rebuildOptions();
    rebuildDerivedText();
    emit optionsChanged();
    emit configChanged();
}

void SettingsAdapter::setAppSettings(const PersistedAppSettings& settings) {
    app_settings_ = settings;
    emit appSettingsChanged();
}

const PersistedAppSettings& SettingsAdapter::appSettings() const noexcept {
    return app_settings_;
}

void SettingsAdapter::setControlsLocked(bool locked) {
    if (controls_locked_ == locked) {
        return;
    }
    controls_locked_ = locked;
    emit controlsLockedChanged();
}

void SettingsAdapter::setMaxFrameRate(int max_fps) {
    if (max_frame_rate_ == max_fps) {
        return;
    }
    max_frame_rate_ = max_fps;
    if (max_fps > 0 && config_.video.frame_rate_num > static_cast<uint32_t>(max_fps)) {
        config_.video.frame_rate_num = static_cast<uint32_t>(max_fps);
        config_.video.frame_rate_den = 1;
        emit configEdited();
    }
    rebuildOptions();
    rebuildDerivedText();
    emit optionsChanged();
    emit configChanged();
}

void SettingsAdapter::setMicrophoneDevices(QVariantList devices) {
    microphone_devices_ = std::move(devices);
    emit microphoneDevicesChanged();
}

void SettingsAdapter::setWebcamDevices(QVariantList devices) {
    webcam_devices_ = std::move(devices);
    emit webcamDevicesChanged();
}

void SettingsAdapter::setMeters(double system, double app, double microphone) {
    if (qFuzzyCompare(system_meter_ + 1.0, system + 1.0) && qFuzzyCompare(app_meter_ + 1.0, app + 1.0) &&
        qFuzzyCompare(microphone_meter_ + 1.0, microphone + 1.0)) {
        return;
    }
    system_meter_ = system;
    app_meter_ = app;
    microphone_meter_ = microphone;
    emit metersChanged();
}

void SettingsAdapter::setHdrDisplayPresent(bool present) {
    if (hdr_display_present_ == present) {
        return;
    }
    hdr_display_present_ = present;
    rebuildOptions();
    emit optionsChanged();
}

void SettingsAdapter::setPresetState(QVariantList options, QString selected_id, bool dirty) {
    preset_options_ = std::move(options);
    selected_preset_id_ = std::move(selected_id);
    preset_dirty_ = dirty;
    preset_built_in_ = IsBuiltInPresetId(selected_preset_id_.toStdString());

    QString label = selected_preset_id_;
    for (const QVariant& entry : preset_options_) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("value")).toString() == selected_preset_id_) {
            label = map.value(QStringLiteral("label")).toString();
            break;
        }
    }
    selected_preset_name_ = label;
    preset_status_text_ = dirty ? tr("%1 · unsaved changes").arg(label) : label;
    emit presetsChanged();
}

void SettingsAdapter::setHotkeyRows(QVariantList rows) {
    hotkey_rows_ = std::move(rows);
    // A successful commit ends the capture; the owner re-pushes the rows.
    capturing_hotkey_action_ = -1;
    hotkey_error_action_ = -1;
    hotkey_error_text_.clear();
    emit hotkeysChanged();
}

void SettingsAdapter::setHotkeyError(int action, QString message) {
    hotkey_error_action_ = action;
    hotkey_error_text_ = std::move(message);
    capturing_hotkey_action_ = -1;
    emit hotkeysChanged();
}

void SettingsAdapter::beginHotkeyCapture(int action) {
    if (controls_locked_) {
        return;
    }
    capturing_hotkey_action_ = action;
    hotkey_error_action_ = -1;
    hotkey_error_text_.clear();
    emit hotkeysChanged();
}

void SettingsAdapter::cancelHotkeyCapture() {
    if (capturing_hotkey_action_ < 0) {
        return;
    }
    capturing_hotkey_action_ = -1;
    emit hotkeysChanged();
}

void SettingsAdapter::commitHotkeyCapture(int key, int modifiers) {
    if (capturing_hotkey_action_ < 0) {
        return;
    }
    emit hotkeyRebindRequested(capturing_hotkey_action_, key, modifiers);
}

void SettingsAdapter::clearHotkey(int action) {
    emit hotkeyClearRequested(action);
}

void SettingsAdapter::resetHotkey(int action) {
    emit hotkeyResetRequested(action);
}

// ---------------------------------------------------------------------------
// Webcam
// ---------------------------------------------------------------------------

bool SettingsAdapter::webcamEnabled() const noexcept {
    return config_.webcam.enabled;
}
const QVariantList& SettingsAdapter::webcamDeviceOptions() const noexcept {
    return webcam_devices_;
}
QString SettingsAdapter::webcamDeviceId() const {
    return QString::fromStdString(config_.webcam.device_id);
}
const QVariantList& SettingsAdapter::webcamResolutionOptions() const noexcept {
    return webcam_resolution_options_;
}
int SettingsAdapter::webcamResolution() const noexcept {
    return config_.webcam.height;
}
const QVariantList& SettingsAdapter::webcamFrameRateOptions() const noexcept {
    return webcam_frame_rate_options_;
}
int SettingsAdapter::webcamFrameRate() const noexcept {
    return config_.webcam.fps;
}
bool SettingsAdapter::webcamMirror() const noexcept {
    return config_.webcam.mirror;
}
double SettingsAdapter::webcamOpacity() const noexcept {
    return static_cast<double>(config_.webcam.opacity);
}
bool SettingsAdapter::webcamAvailable() const noexcept {
    return !webcam_devices_.isEmpty();
}
bool SettingsAdapter::chromaKeyEnabled() const noexcept {
    return config_.webcam.chroma_key.enabled;
}
const QVariantList& SettingsAdapter::chromaKeyColorOptions() const noexcept {
    return chroma_key_color_options_;
}
int SettingsAdapter::chromaKeyColorMode() const noexcept {
    return static_cast<int>(config_.webcam.chroma_key.color_mode);
}
int SettingsAdapter::chromaKeyTolerance() const noexcept {
    return qRound(config_.webcam.chroma_key.tolerance * 100.0f);
}
int SettingsAdapter::chromaKeySoftness() const noexcept {
    return qRound(config_.webcam.chroma_key.softness * 100.0f);
}
int SettingsAdapter::chromaKeySpill() const noexcept {
    return qRound(config_.webcam.chroma_key.spill_reduction * 100.0f);
}

void SettingsAdapter::setWebcamEnabled(bool value) {
    if (config_.webcam.enabled == value) {
        return;
    }
    config_.webcam.enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setWebcamDeviceId(const QString& value) {
    const std::string device_id = value.toStdString();
    if (config_.webcam.device_id == device_id) {
        return;
    }
    config_.webcam.device_id = device_id;
    applyConfigEdit();
}

void SettingsAdapter::setWebcamResolution(int value) {
    // The option value is the height; the paired width keeps 16:9 except for the
    // 4:3 VGA entry, matching what the capture backend requests.
    const int height = value;
    const int width = height == 480 ? 640 : height * 16 / 9;
    if (config_.webcam.height == height && config_.webcam.width == width) {
        return;
    }
    config_.webcam.height = height;
    config_.webcam.width = width;
    applyConfigEdit();
}

void SettingsAdapter::setWebcamFrameRate(int value) {
    if (config_.webcam.fps == value) {
        return;
    }
    config_.webcam.fps = value;
    applyConfigEdit();
}

void SettingsAdapter::setWebcamMirror(bool value) {
    if (config_.webcam.mirror == value) {
        return;
    }
    config_.webcam.mirror = value;
    applyConfigEdit();
}

void SettingsAdapter::setWebcamOpacity(double value) {
    const auto opacity = static_cast<float>(std::clamp(value, 0.0, 1.0));
    if (std::abs(config_.webcam.opacity - opacity) < 1e-4f) {
        return;
    }
    config_.webcam.opacity = opacity;
    applyConfigEdit();
}

void SettingsAdapter::setChromaKeyEnabled(bool value) {
    if (config_.webcam.chroma_key.enabled == value) {
        return;
    }
    config_.webcam.chroma_key.enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setChromaKeyColorMode(int value) {
    const auto mode = static_cast<WebcamChromaKeyColorMode>(value);
    if (config_.webcam.chroma_key.color_mode == mode) {
        return;
    }
    config_.webcam.chroma_key.color_mode = mode;
    applyConfigEdit();
}

void SettingsAdapter::setChromaKeyTolerance(int value) {
    const auto tolerance = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
    if (std::abs(config_.webcam.chroma_key.tolerance - tolerance) < 1e-4f) {
        return;
    }
    config_.webcam.chroma_key.tolerance = tolerance;
    applyConfigEdit();
}

void SettingsAdapter::setChromaKeySoftness(int value) {
    const auto softness = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
    if (std::abs(config_.webcam.chroma_key.softness - softness) < 1e-4f) {
        return;
    }
    config_.webcam.chroma_key.softness = softness;
    applyConfigEdit();
}

void SettingsAdapter::setChromaKeySpill(int value) {
    const auto spill = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
    if (std::abs(config_.webcam.chroma_key.spill_reduction - spill) < 1e-4f) {
        return;
    }
    config_.webcam.chroma_key.spill_reduction = spill;
    applyConfigEdit();
}

const QVariantList& SettingsAdapter::hotkeyRows() const noexcept {
    return hotkey_rows_;
}
int SettingsAdapter::capturingHotkeyAction() const noexcept {
    return capturing_hotkey_action_;
}
const QString& SettingsAdapter::hotkeyErrorText() const noexcept {
    return hotkey_error_text_;
}
int SettingsAdapter::hotkeyErrorAction() const noexcept {
    return hotkey_error_action_;
}

void SettingsAdapter::setUpdateStatus(const QString& state, const QString& available_version,
                                      const QString& last_checked, const QString& detail) {
    update_state_ = state;
    update_available_version_ = available_version;
    whats_new_available_ = state == QLatin1String("available") && !available_version.isEmpty();

    if (state == QLatin1String("checking")) {
        update_status_text_ = tr("Checking for updates…");
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = false;
    } else if (state == QLatin1String("available")) {
        update_status_text_ = tr("Update %1 is available.").arg(available_version);
        update_action_text_ = tr("Update to %1").arg(available_version);
        update_action_enabled_ = true;
    } else if (state == QLatin1String("error")) {
        update_status_text_ = detail.isEmpty() ? tr("Update check failed.") : detail;
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = true;
    } else if (state == QLatin1String("unchecked")) {
        // Distinct from "up to date": nothing has been checked yet this session,
        // and claiming otherwise would be an assertion the app cannot make.
        update_status_text_ = tr("No update check has run yet.");
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = true;
    } else if (state == QLatin1String("scoop")) {
        // Notify-only: the staged swap never touches a Scoop tree.
        update_status_text_ = available_version.isEmpty()
                                  ? tr("Managed by Scoop — update with `scoop update exosnap`.")
                                  : tr("Version %1 is available. This install is managed by Scoop — update with "
                                       "`scoop update exosnap`.")
                                        .arg(available_version);
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = true;
    } else if (state == QLatin1String("updater-running")) {
        update_status_text_ =
            tr("Updater is running — ExoSnap will close to finish installing %1.").arg(available_version);
        update_action_text_ = tr("Updater running…");
        update_action_enabled_ = false;
    } else if (state == QLatin1String("pending")) {
        update_status_text_ =
            tr("Update %1 is staged. Restart ExoSnap to finish installing it.").arg(available_version);
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = true;
    } else if (state == QLatin1String("verify-reinstall")) {
        // ADR 0055: the offered version IS the running one, on purpose.
        update_status_text_ = tr("Verification reinstall of %1 is ready.").arg(available_version);
        update_action_text_ = tr("Reinstall %1").arg(available_version);
        update_action_enabled_ = true;
    } else {
        update_status_text_ =
            last_checked.isEmpty() ? tr("Up to date.") : tr("Up to date · last checked %1").arg(last_checked);
        update_action_text_ = tr("Check for updates");
        update_action_enabled_ = true;
    }
    emit updateStatusChanged();
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

void SettingsAdapter::applyConfigEdit() {
    // Container/codec reconciliation and per-field clamping are engine-side
    // rules; running them here means a QML edit can never persist a
    // combination the recording side would reject.
    ReconcileContainerCodecs(config_.output);
    config_ = SanitizePresetConfig(std::move(config_));
    rebuildOptions();
    rebuildDerivedText();
    emit optionsChanged();
    emit configChanged();
    emit configEdited();
}

void SettingsAdapter::commitAppSettingsEdit() {
    emit appSettingsChanged();
    emit appSettingsEdited();
}

void SettingsAdapter::rebuildOptions() {
    const auto& out = config_.output;

    container_options_.clear();
    for (const Container value : capability::AllContainers()) {
        const auto annotation = caps_set_ ? caps_.QueryContainer(value) : capability::SupportAnnotation{};
        const bool selectable = !caps_set_ || capability::IsSelectable(annotation);
        container_options_.append(
            makeOption(static_cast<int>(value), ui::containerLabel(value), selectable, fromAnnotation(annotation)));
    }

    video_codec_options_.clear();
    for (const VideoCodec value : capability::AllVideoCodecs()) {
        const auto compat = capability::ContainerCompatRegistry::Query(out.container, value, out.audio_codec);
        const auto annotation = caps_set_ ? caps_.QueryVideoCodec(value) : capability::SupportAnnotation{};
        const bool selectable = capability::IsContainerCompatSelectable(compat.level) &&
                                (!caps_set_ || capability::IsSelectable(annotation));
        const QString reason = capability::IsContainerCompatSelectable(compat.level)
                                   ? fromAnnotation(annotation)
                                   : QString::fromUtf8(compat.reason.data(), static_cast<int>(compat.reason.size()));
        video_codec_options_.append(
            makeOption(static_cast<int>(value), ui::videoCodecLabel(value), selectable, reason));
    }

    audio_codec_options_.clear();
    for (const AudioCodec value : capability::AllAudioCodecs()) {
        const auto compat = capability::ContainerCompatRegistry::Query(out.container, out.video_codec, value);
        const auto annotation = caps_set_ ? caps_.QueryAudioCodec(value) : capability::SupportAnnotation{};
        const bool selectable = capability::IsContainerCompatSelectable(compat.level) &&
                                (!caps_set_ || capability::IsSelectable(annotation));
        const QString reason = capability::IsContainerCompatSelectable(compat.level)
                                   ? fromAnnotation(annotation)
                                   : QString::fromUtf8(compat.reason.data(), static_cast<int>(compat.reason.size()));
        audio_codec_options_.append(
            makeOption(static_cast<int>(value), ui::audioCodecLabel(value), selectable, reason));
    }

    bit_depth_options_.clear();
    for (const BitDepth value : capability::AllBitDepths()) {
        const auto annotation =
            caps_set_ ? caps_.QueryCombo(out.container, out.video_codec, out.audio_codec, out.chroma_subsampling, value)
                      : capability::SupportAnnotation{};
        const bool selectable = value == BitDepth::Bit8 || (caps_set_ && capability::IsSelectable(annotation));
        const QString label = value == BitDepth::Bit8 ? tr("8-bit") : tr("10-bit");
        bit_depth_options_.append(makeOption(static_cast<int>(value), label, selectable, fromAnnotation(annotation)));
    }

    chroma_options_.clear();
    chroma_hint_.clear();
    for (const ChromaSubsampling value : {ChromaSubsampling::Cs420, ChromaSubsampling::Cs444}) {
        const auto annotation =
            caps_set_ ? caps_.QueryCombo(out.container, out.video_codec, out.audio_codec, value, out.bit_depth)
                      : capability::SupportAnnotation{};
        const bool selectable =
            value == ChromaSubsampling::Cs420 || (caps_set_ && capability::IsSelectable(annotation));
        const QString label = value == ChromaSubsampling::Cs420 ? QStringLiteral("4:2:0") : QStringLiteral("4:4:4");
        chroma_options_.append(makeOption(static_cast<int>(value), label, selectable, fromAnnotation(annotation)));
        if (value == ChromaSubsampling::Cs444 && !selectable) {
            chroma_hint_ = caps_set_ ? fromAnnotation(caps_.QueryChroma444(out.video_codec))
                                     : tr("4:4:4 requires 8-bit H.264 or HEVC.");
            if (chroma_hint_.isEmpty()) {
                chroma_hint_ = tr("4:4:4 is not available for this codec and bit depth.");
            }
        }
    }

    color_range_options_.clear();
    color_range_options_.append(makeOption(static_cast<int>(ColorRange::Limited), tr("Limited (TV)")));
    color_range_options_.append(makeOption(static_cast<int>(ColorRange::Full), tr("Full (PC)")));

    hdr_mode_options_.clear();
    hdr_hint_.clear();
    {
        const auto hdr10 = caps_set_ ? caps_.QueryHdr10Native(out.video_codec) : capability::SupportAnnotation{};
        const bool hdr10_selectable = caps_set_ && capability::IsSelectable(hdr10);
        hdr_mode_options_.append(
            makeOption(static_cast<int>(recorder_core::HdrMode::TonemapSdr), tr("Tone-map to SDR")));
        hdr_mode_options_.append(makeOption(static_cast<int>(recorder_core::HdrMode::Hdr10), tr("Record native HDR10"),
                                            hdr10_selectable, fromAnnotation(hdr10)));
        if (!hdr10_selectable) {
            hdr_hint_ = fromAnnotation(hdr10);
            if (hdr_hint_.isEmpty()) {
                hdr_hint_ = tr("Native HDR10 requires HEVC or AV1.");
            }
        }
    }

    encoder_preset_options_.clear();
    for (int i = static_cast<int>(recorder_core::NvencPreset::P1);
         i <= static_cast<int>(recorder_core::NvencPreset::P7); ++i) {
        encoder_preset_options_.append(makeOption(i, nvencPresetLabel(static_cast<recorder_core::NvencPreset>(i))));
    }

    quality_preset_options_.clear();
    for (const recorder_core::QualityPreset preset :
         {recorder_core::QualityPreset::Ultra, recorder_core::QualityPreset::High,
          recorder_core::QualityPreset::Balanced, recorder_core::QualityPreset::Efficient,
          recorder_core::QualityPreset::Draft}) {
        quality_preset_options_.append(
            makeOption(static_cast<int>(preset),
                       tr("%1 · CQ %2").arg(qualityPresetLabel(preset)).arg(recorder_core::CanonicalCq(preset))));
    }

    rate_control_options_.clear();
    for (const recorder_core::RateControlMode mode :
         {recorder_core::RateControlMode::ConstantQuality, recorder_core::RateControlMode::VariableBitrate,
          recorder_core::RateControlMode::ConstantBitrate}) {
        const auto annotation = caps_set_ ? caps_.QueryRateControlMode(mode) : capability::SupportAnnotation{};
        const bool selectable = !caps_set_ || capability::IsSelectable(annotation);
        QString label;
        switch (mode) {
        case recorder_core::RateControlMode::ConstantQuality:
            label = tr("Constant quality");
            break;
        case recorder_core::RateControlMode::VariableBitrate:
            label = tr("Variable bitrate");
            break;
        case recorder_core::RateControlMode::ConstantBitrate:
            label = tr("Constant bitrate");
            break;
        case recorder_core::RateControlMode::Lossless:
            break;
        }
        rate_control_options_.append(makeOption(static_cast<int>(mode), label, selectable, fromAnnotation(annotation)));
    }

    frame_rate_options_.clear();
    for (const uint32_t fps : kFrameRateLadder) {
        if (max_frame_rate_ > 0 && fps > static_cast<uint32_t>(max_frame_rate_)) {
            continue;
        }
        frame_rate_options_.append(makeOption(static_cast<int>(fps), tr("%1 fps").arg(fps)));
    }
    if (frame_rate_options_.isEmpty()) {
        frame_rate_options_.append(
            makeOption(static_cast<int>(config_.video.frame_rate_num), tr("%1 fps").arg(config_.video.frame_rate_num)));
    }

    timing_options_.clear();
    timing_options_.append(makeOption(1, tr("Constant frame rate")));
    timing_options_.append(makeOption(0, tr("Variable frame rate")));

    frame_pacing_options_.clear();
    frame_pacing_options_.append(makeOption(static_cast<int>(recorder_core::FramePacingMode::Smooth), tr("Smooth")));
    frame_pacing_options_.append(makeOption(static_cast<int>(recorder_core::FramePacingMode::Newest), tr("Newest")));

    keyframe_interval_options_.clear();
    keyframe_interval_options_.append(makeOption(static_cast<int>(KeyframeIntervalMode::Seconds2), tr("2 s")));
    keyframe_interval_options_.append(makeOption(static_cast<int>(KeyframeIntervalMode::Seconds1), tr("1 s")));
    keyframe_interval_options_.append(makeOption(static_cast<int>(KeyframeIntervalMode::Seconds0_5), tr("0.5 s")));

    resolution_options_.clear();
    for (const OutputResolutionMode mode :
         {OutputResolutionMode::Native, OutputResolutionMode::UHD2160, OutputResolutionMode::QHD1440,
          OutputResolutionMode::FHD1080, OutputResolutionMode::HD720, OutputResolutionMode::Custom}) {
        resolution_options_.append(makeOption(static_cast<int>(mode), fromWide(OutputResolutionModeName(mode))));
    }

    split_mode_options_.clear();
    for (const SplitRecordingMode mode : {SplitRecordingMode::Every15Min, SplitRecordingMode::Every30Min,
                                          SplitRecordingMode::Every60Min, SplitRecordingMode::Custom}) {
        split_mode_options_.append(makeOption(static_cast<int>(mode), fromWide(SplitRecordingModeName(mode))));
    }

    mic_channel_mode_options_.clear();
    mic_channel_mode_options_.append(makeOption(static_cast<int>(recorder_core::MicChannelMode::Auto), tr("Auto")));
    mic_channel_mode_options_.append(
        makeOption(static_cast<int>(recorder_core::MicChannelMode::PreserveStereo), tr("Preserve stereo")));
    mic_channel_mode_options_.append(
        makeOption(static_cast<int>(recorder_core::MicChannelMode::MonoMix), tr("Mono mix")));
    mic_channel_mode_options_.append(
        makeOption(static_cast<int>(recorder_core::MicChannelMode::LeftToStereo), tr("Left channel to stereo")));
    mic_channel_mode_options_.append(
        makeOption(static_cast<int>(recorder_core::MicChannelMode::RightToStereo), tr("Right channel to stereo")));

    audio_sample_rate_options_.clear();
    for (const uint32_t rate : {44100u, 48000u, 96000u}) {
        audio_sample_rate_options_.append(
            makeOption(static_cast<int>(rate), tr("%1 kHz").arg(QString::number(rate / 1000.0, 'g', 4))));
    }

    audio_channels_options_.clear();
    audio_channels_options_.append(makeOption(1, tr("Mono")));
    audio_channels_options_.append(makeOption(2, tr("Stereo")));

    audio_bit_depth_options_.clear();
    audio_bit_depth_options_.append(makeOption(16, tr("16-bit")));
    audio_bit_depth_options_.append(makeOption(24, tr("24-bit")));
    if (out.audio_codec == AudioCodec::Pcm) {
        audio_bit_depth_options_.append(makeOption(32, tr("32-bit")));
    }

    opus_frame_duration_options_.clear();
    opus_frame_duration_options_.append(
        makeOption(static_cast<int>(recorder_core::OpusFrameDuration::Ms20), tr("20 ms")));
    opus_frame_duration_options_.append(
        makeOption(static_cast<int>(recorder_core::OpusFrameDuration::Ms10), tr("10 ms")));
    opus_frame_duration_options_.append(
        makeOption(static_cast<int>(recorder_core::OpusFrameDuration::Ms5), tr("5 ms")));
    opus_frame_duration_options_.append(
        makeOption(static_cast<int>(recorder_core::OpusFrameDuration::Ms2_5), tr("2.5 ms")));

    // Webcam capture formats. Values are the pixel height; the adapter pairs it
    // with the matching width so QML never carries a resolution table.
    webcam_resolution_options_.clear();
    webcam_resolution_options_.append(makeOption(480, QStringLiteral("640 \xC3\x97 480")));
    webcam_resolution_options_.append(makeOption(720, QStringLiteral("1280 \xC3\x97 720")));
    webcam_resolution_options_.append(makeOption(1080, QStringLiteral("1920 \xC3\x97 1080")));

    webcam_frame_rate_options_.clear();
    for (const int fps : {24, 30, 60}) {
        webcam_frame_rate_options_.append(makeOption(fps, tr("%1 fps").arg(fps)));
    }

    chroma_key_color_options_.clear();
    chroma_key_color_options_.append(makeOption(static_cast<int>(WebcamChromaKeyColorMode::Green), tr("Green")));
    chroma_key_color_options_.append(makeOption(static_cast<int>(WebcamChromaKeyColorMode::Blue), tr("Blue")));
    chroma_key_color_options_.append(makeOption(static_cast<int>(WebcamChromaKeyColorMode::Magenta), tr("Magenta")));
}

void SettingsAdapter::rebuildDerivedText() {
    const auto& out = config_.output;
    const auto& video = config_.video;

    format_summary_ =
        tr("%1 · %2 · %3 · %4 %5")
            .arg(ui::containerLabel(out.container), ui::videoCodecLabel(out.video_codec),
                 ui::audioCodecLabel(out.audio_codec), ui::frameRateLabel(video.frame_rate_num, video.frame_rate_den),
                 video.cfr ? tr("CFR") : tr("VFR"));

    const auto compat = capability::ContainerCompatRegistry::Query(out.container, out.video_codec, out.audio_codec);
    compat_notice_ = compat.level == capability::ContainerCompatLevel::Recommended
                         ? QString()
                         : QString::fromUtf8(compat.reason.data(), static_cast<int>(compat.reason.size()));

    example_filename_ = fromWide(BuildFilename(out.naming_pattern, out.container, std::time(nullptr)));
    saves_to_text_ = out.output_folder.empty() ? QString() : QString::fromStdWString(out.output_folder.wstring());

    const FolderValidationResult folder_result = ValidateOutputFolder(out.output_folder);
    folder_validation_ =
        folder_result == FolderValidationResult::Ok ? QString() : fromWide(FolderValidationMessage(folder_result));

    const NormalizedFilenamePattern pattern = NormalizeFilenamePatternInput(out.naming_pattern);
    pattern_validation_ = pattern.result == FilenamePatternPolicyResult::Ok
                              ? QString()
                              : fromWide(FilenamePatternPolicyMessage(pattern.result));

    custom_resolution_validation_.clear();
    if (customResolutionActive()) {
        const uint32_t w = pending_custom_width_;
        const uint32_t h = pending_custom_height_;
        if (w == 0 || h == 0) {
            custom_resolution_validation_ = tr("Enter a width and a height.");
        } else if ((w % 2u) != 0u || (h % 2u) != 0u) {
            custom_resolution_validation_ = tr("Width and height must be even numbers.");
        }
    }

    const SplitRecordingSettings& split = out.split;
    QStringList split_parts;
    if (split.mode != SplitRecordingMode::Off) {
        split_parts.append(split.mode == SplitRecordingMode::Custom
                               ? tr("every %1 min").arg(split.custom_minutes)
                               : fromWide(SplitRecordingModeName(split.mode)).toLower());
    }
    if (split.size_mode != SplitSizeMode::Off) {
        split_parts.append(tr("every %1 MB").arg(split.custom_size_mb));
    }
    split_summary_ = split_parts.isEmpty() ? tr("Single file") : tr("New file %1").arg(split_parts.join(tr(" or ")));

    const auto& audio = config_.audio;
    QStringList stages;
    if (audio.mic_hpf_enabled) {
        stages.append(tr("high-pass"));
    }
    if (audio.mic_gate_enabled) {
        stages.append(tr("gate"));
    }
    if (audio.mic_agc_enabled) {
        stages.append(tr("AGC"));
    }
    if (audio.mic_rnnoise_enabled) {
        stages.append(tr("RNNoise"));
    }
    mic_post_processing_summary_ = stages.isEmpty() ? tr("Off") : stages.join(QStringLiteral(" · "));

    QStringList sources;
    if (audio.IsAppEnabled()) {
        sources.append(QStringLiteral("APP"));
    }
    if (audio.IsSysEnabled()) {
        sources.append(QStringLiteral("SYS"));
    }
    if (audio.IsMicEnabled()) {
        sources.append(QStringLiteral("MIC"));
    }
    audio_summary_ = sources.isEmpty() ? tr("No audio") : sources.join(QStringLiteral(" · "));
}

// ---------------------------------------------------------------------------
// Audio row helpers
// ---------------------------------------------------------------------------

recorder_core::AudioSourceRow* SettingsAdapter::findRow(recorder_core::AudioSourceKind kind) {
    for (auto& row : config_.audio.source_rows) {
        if (row.kind == kind || (isSysKind(kind) && isSysKind(row.kind))) {
            return &row;
        }
    }
    return nullptr;
}

const recorder_core::AudioSourceRow* SettingsAdapter::findRow(recorder_core::AudioSourceKind kind) const {
    return const_cast<SettingsAdapter*>(this)->findRow(kind);
}

void SettingsAdapter::setRowEnabled(recorder_core::AudioSourceKind kind, bool enabled) {
    if (recorder_core::AudioSourceRow* row = findRow(kind)) {
        if (row->enabled == enabled) {
            return;
        }
        row->enabled = enabled;
    } else {
        if (!enabled) {
            return;
        }
        recorder_core::AudioSourceRow new_row;
        new_row.kind = kind;
        new_row.enabled = true;
        config_.audio.source_rows.push_back(new_row);
    }
    applyConfigEdit();
}

void SettingsAdapter::setRowSeparate(recorder_core::AudioSourceKind kind, bool separate) {
    recorder_core::AudioSourceRow* row = findRow(kind);
    if (row == nullptr || row->merge_with_above == !separate) {
        return;
    }
    row->merge_with_above = !separate;
    applyConfigEdit();
}

// ---------------------------------------------------------------------------
// Readers
// ---------------------------------------------------------------------------

bool SettingsAdapter::expertMode() const noexcept {
    return app_settings_.expert_mode_enabled;
}
bool SettingsAdapter::controlsLocked() const noexcept {
    return controls_locked_;
}
const QVariantList& SettingsAdapter::containerOptions() const noexcept {
    return container_options_;
}
int SettingsAdapter::container() const noexcept {
    return static_cast<int>(config_.output.container);
}
const QVariantList& SettingsAdapter::videoCodecOptions() const noexcept {
    return video_codec_options_;
}
int SettingsAdapter::videoCodec() const noexcept {
    return static_cast<int>(config_.output.video_codec);
}
const QVariantList& SettingsAdapter::audioCodecOptions() const noexcept {
    return audio_codec_options_;
}
int SettingsAdapter::audioCodec() const noexcept {
    return static_cast<int>(config_.output.audio_codec);
}
const QVariantList& SettingsAdapter::bitDepthOptions() const noexcept {
    return bit_depth_options_;
}
int SettingsAdapter::bitDepth() const noexcept {
    return static_cast<int>(config_.output.bit_depth);
}
const QVariantList& SettingsAdapter::chromaOptions() const noexcept {
    return chroma_options_;
}
int SettingsAdapter::chroma() const noexcept {
    return static_cast<int>(config_.output.chroma_subsampling);
}
const QString& SettingsAdapter::chromaHint() const noexcept {
    return chroma_hint_;
}
const QVariantList& SettingsAdapter::colorRangeOptions() const noexcept {
    return color_range_options_;
}
int SettingsAdapter::colorRange() const noexcept {
    return static_cast<int>(config_.output.color_range);
}
const QVariantList& SettingsAdapter::hdrModeOptions() const noexcept {
    return hdr_mode_options_;
}
int SettingsAdapter::hdrMode() const noexcept {
    return static_cast<int>(config_.output.hdr_mode);
}
const QString& SettingsAdapter::hdrHint() const noexcept {
    return hdr_hint_;
}
bool SettingsAdapter::hdrRelevant() const noexcept {
    return hdr_display_present_;
}
const QVariantList& SettingsAdapter::encoderPresetOptions() const noexcept {
    return encoder_preset_options_;
}
int SettingsAdapter::encoderPreset() const noexcept {
    return static_cast<int>(config_.output.nvenc_preset);
}
const QString& SettingsAdapter::formatSummary() const noexcept {
    return format_summary_;
}
const QString& SettingsAdapter::compatNotice() const noexcept {
    return compat_notice_;
}
bool SettingsAdapter::compatOk() const noexcept {
    return compat_notice_.isEmpty();
}
const QVariantList& SettingsAdapter::qualityPresetOptions() const noexcept {
    return quality_preset_options_;
}
int SettingsAdapter::qualityPreset() const noexcept {
    return static_cast<int>(recorder_core::NearestQualityPreset(config_.video.cq));
}
int SettingsAdapter::cq() const noexcept {
    return static_cast<int>(config_.video.cq);
}
const QVariantList& SettingsAdapter::rateControlOptions() const noexcept {
    return rate_control_options_;
}
int SettingsAdapter::rateControl() const noexcept {
    return static_cast<int>(config_.video.rate_control);
}
bool SettingsAdapter::bitrateRelevant() const noexcept {
    return config_.video.rate_control == recorder_core::RateControlMode::VariableBitrate ||
           config_.video.rate_control == recorder_core::RateControlMode::ConstantBitrate;
}
int SettingsAdapter::bitrateKbps() const noexcept {
    return static_cast<int>(config_.video.bitrate_kbps);
}
const QVariantList& SettingsAdapter::frameRateOptions() const noexcept {
    return frame_rate_options_;
}
int SettingsAdapter::frameRate() const noexcept {
    return static_cast<int>(config_.video.frame_rate_num);
}
int SettingsAdapter::maxFrameRate() const noexcept {
    return max_frame_rate_;
}
const QVariantList& SettingsAdapter::timingOptions() const noexcept {
    return timing_options_;
}
bool SettingsAdapter::cfr() const noexcept {
    return config_.video.cfr;
}
const QVariantList& SettingsAdapter::framePacingOptions() const noexcept {
    return frame_pacing_options_;
}
int SettingsAdapter::framePacing() const noexcept {
    return static_cast<int>(config_.video.frame_pacing);
}
const QVariantList& SettingsAdapter::keyframeIntervalOptions() const noexcept {
    return keyframe_interval_options_;
}
int SettingsAdapter::keyframeInterval() const noexcept {
    return static_cast<int>(config_.video.keyframe_interval);
}
bool SettingsAdapter::captureCursor() const noexcept {
    return config_.video.capture_cursor;
}
QString SettingsAdapter::outputFolder() const {
    return QString::fromStdWString(config_.output.output_folder.wstring());
}
QString SettingsAdapter::namingPattern() const {
    return fromWide(config_.output.naming_pattern);
}
const QString& SettingsAdapter::exampleFilename() const noexcept {
    return example_filename_;
}
const QString& SettingsAdapter::savesToText() const noexcept {
    return saves_to_text_;
}
const QString& SettingsAdapter::folderValidation() const noexcept {
    return folder_validation_;
}
const QString& SettingsAdapter::patternValidation() const noexcept {
    return pattern_validation_;
}

QVariantList SettingsAdapter::filenameTokens() const {
    QVariantList tokens;
    for (const auto& token : {QStringLiteral("{datetime}"), QStringLiteral("{date}"), QStringLiteral("{time}"),
                              QStringLiteral("{app}"), QStringLiteral("{title}"), QStringLiteral("{display}"),
                              QStringLiteral("{codec}"), QStringLiteral("{container}")}) {
        tokens.append(token);
    }
    return tokens;
}

const QVariantList& SettingsAdapter::resolutionOptions() const noexcept {
    return resolution_options_;
}
int SettingsAdapter::resolutionMode() const noexcept {
    // While a custom size is still incomplete the sanitized model reads Native;
    // the dropdown must keep showing what the user actually selected.
    return static_cast<int>(custom_resolution_pending_ ? OutputResolutionMode::Custom : config_.output.resolution.mode);
}
bool SettingsAdapter::customResolutionActive() const noexcept {
    return custom_resolution_pending_ || config_.output.resolution.mode == OutputResolutionMode::Custom;
}
int SettingsAdapter::customWidth() const noexcept {
    return static_cast<int>(pending_custom_width_);
}
int SettingsAdapter::customHeight() const noexcept {
    return static_cast<int>(pending_custom_height_);
}
const QString& SettingsAdapter::customResolutionValidation() const noexcept {
    return custom_resolution_validation_;
}
bool SettingsAdapter::splitByTimeEnabled() const noexcept {
    return config_.output.split.mode != SplitRecordingMode::Off;
}
const QVariantList& SettingsAdapter::splitModeOptions() const noexcept {
    return split_mode_options_;
}
int SettingsAdapter::splitMode() const noexcept {
    return static_cast<int>(config_.output.split.mode);
}
bool SettingsAdapter::splitCustomIntervalActive() const noexcept {
    return config_.output.split.mode == SplitRecordingMode::Custom;
}
int SettingsAdapter::splitCustomMinutes() const noexcept {
    return static_cast<int>(config_.output.split.custom_minutes);
}
bool SettingsAdapter::splitBySizeEnabled() const noexcept {
    return config_.output.split.size_mode != SplitSizeMode::Off;
}
int SettingsAdapter::splitCustomSizeMb() const noexcept {
    return static_cast<int>(config_.output.split.custom_size_mb);
}
const QString& SettingsAdapter::splitSummary() const noexcept {
    return split_summary_;
}

bool SettingsAdapter::appAudioVisible() const noexcept {
    return config_.audio.target_kind == capability::CaptureTargetKind::Window;
}
bool SettingsAdapter::appAudioEnabled() const noexcept {
    return config_.audio.IsAppEnabled();
}
bool SettingsAdapter::appAudioSeparate() const noexcept {
    const auto* row = findRow(recorder_core::AudioSourceKind::App);
    return row == nullptr || !row->merge_with_above;
}
bool SettingsAdapter::systemAudioEnabled() const noexcept {
    return config_.audio.IsSysEnabled();
}
bool SettingsAdapter::systemAudioSeparate() const noexcept {
    const auto* row = findRow(recorder_core::AudioSourceKind::Sys);
    return row == nullptr || !row->merge_with_above;
}
bool SettingsAdapter::microphoneEnabled() const noexcept {
    return config_.audio.IsMicEnabled();
}
bool SettingsAdapter::microphoneSeparate() const noexcept {
    const auto* row = findRow(recorder_core::AudioSourceKind::Mic);
    return row == nullptr || !row->merge_with_above;
}
const QVariantList& SettingsAdapter::microphoneDeviceOptions() const noexcept {
    return microphone_devices_;
}
QString SettingsAdapter::microphoneDeviceId() const {
    return config_.audio.selected_mic_device_id.has_value()
               ? QString::fromStdString(*config_.audio.selected_mic_device_id)
               : QString();
}
const QVariantList& SettingsAdapter::micChannelModeOptions() const noexcept {
    return mic_channel_mode_options_;
}
int SettingsAdapter::micChannelMode() const noexcept {
    return static_cast<int>(config_.audio.mic_channel_mode);
}
double SettingsAdapter::micGainDb() const noexcept {
    return config_.audio.mic_gain_linear <= 0.0f
               ? -60.0
               : 20.0 * std::log10(static_cast<double>(config_.audio.mic_gain_linear));
}
int SettingsAdapter::audioBitrateKbps() const noexcept {
    return static_cast<int>(config_.audio.audio_bitrate_kbps);
}
bool SettingsAdapter::audioBitrateRelevant() const noexcept {
    return config_.output.audio_codec == AudioCodec::Opus || config_.output.audio_codec == AudioCodec::Aac;
}
const QVariantList& SettingsAdapter::audioSampleRateOptions() const noexcept {
    return audio_sample_rate_options_;
}
int SettingsAdapter::audioSampleRate() const noexcept {
    return static_cast<int>(config_.audio.audio_sample_rate);
}
bool SettingsAdapter::audioSampleRateRelevant() const noexcept {
    // Opus is locked to its native 48 kHz rate; offering the control there
    // would advertise a choice the encoder does not honour.
    return config_.output.audio_codec != AudioCodec::Opus;
}
const QVariantList& SettingsAdapter::audioChannelsOptions() const noexcept {
    return audio_channels_options_;
}
int SettingsAdapter::audioChannels() const noexcept {
    return static_cast<int>(config_.audio.audio_channels);
}
const QVariantList& SettingsAdapter::audioBitDepthOptions() const noexcept {
    return audio_bit_depth_options_;
}
int SettingsAdapter::audioBitDepth() const noexcept {
    return static_cast<int>(config_.audio.audio_bit_depth);
}
bool SettingsAdapter::audioBitDepthRelevant() const noexcept {
    return config_.output.audio_codec == AudioCodec::Pcm || config_.output.audio_codec == AudioCodec::Flac;
}
int SettingsAdapter::flacCompressionLevel() const noexcept {
    return config_.audio.flac_compression_level;
}
bool SettingsAdapter::flacCompressionRelevant() const noexcept {
    return config_.output.audio_codec == AudioCodec::Flac;
}
bool SettingsAdapter::limiterEnabled() const noexcept {
    return config_.audio.limiter_enabled;
}
double SettingsAdapter::limiterCeilingDb() const noexcept {
    return static_cast<double>(config_.audio.limiter_ceiling_db);
}
bool SettingsAdapter::clockSlavingEnabled() const noexcept {
    return config_.audio.clock_slaving_enabled;
}
const QVariantList& SettingsAdapter::opusFrameDurationOptions() const noexcept {
    return opus_frame_duration_options_;
}
int SettingsAdapter::opusFrameDuration() const noexcept {
    return static_cast<int>(config_.audio.opus_frame_duration);
}
int SettingsAdapter::opusComplexity() const noexcept {
    return config_.audio.opus_complexity;
}
bool SettingsAdapter::opusControlsRelevant() const noexcept {
    return config_.output.audio_codec == AudioCodec::Opus;
}
bool SettingsAdapter::micHpfEnabled() const noexcept {
    return config_.audio.mic_hpf_enabled;
}
double SettingsAdapter::micHpfCutoffHz() const noexcept {
    return static_cast<double>(config_.audio.mic_hpf_cutoff_hz);
}
bool SettingsAdapter::micGateEnabled() const noexcept {
    return config_.audio.mic_gate_enabled;
}
double SettingsAdapter::micGateThresholdDb() const noexcept {
    return static_cast<double>(config_.audio.mic_gate_threshold_db);
}
bool SettingsAdapter::micAgcEnabled() const noexcept {
    return config_.audio.mic_agc_enabled;
}
double SettingsAdapter::micAgcTargetDb() const noexcept {
    return static_cast<double>(config_.audio.mic_agc_target_db);
}
bool SettingsAdapter::micRnnoiseEnabled() const noexcept {
    return config_.audio.mic_rnnoise_enabled;
}
double SettingsAdapter::systemMeter() const noexcept {
    return system_meter_;
}
double SettingsAdapter::appMeter() const noexcept {
    return app_meter_;
}
double SettingsAdapter::microphoneMeter() const noexcept {
    return microphone_meter_;
}
const QString& SettingsAdapter::micPostProcessingSummary() const noexcept {
    return mic_post_processing_summary_;
}
const QString& SettingsAdapter::audioSummary() const noexcept {
    return audio_summary_;
}

bool SettingsAdapter::showRecordingOverlay() const noexcept {
    return app_settings_.show_recording_overlay;
}
bool SettingsAdapter::showDiagnosticsOverlay() const noexcept {
    return app_settings_.show_diagnostics_overlay;
}
bool SettingsAdapter::showNotifications() const noexcept {
    return app_settings_.show_notifications;
}
bool SettingsAdapter::showQuickControls() const noexcept {
    return app_settings_.show_quick_controls;
}
bool SettingsAdapter::keepRunningInTray() const noexcept {
    return app_settings_.keep_running_in_tray;
}
bool SettingsAdapter::openEditorWhenFinished() const noexcept {
    return app_settings_.open_editor_when_finished;
}
bool SettingsAdapter::presentDiagnosticsOptIn() const noexcept {
    return app_settings_.present_diagnostics_optin;
}

// ---- Overlay content ------------------------------------------------------
//
// Every accessor below resolves through models::OverlayContentPolicy rather than
// caching a struct. The persisted pair (preset, custom tokens) is small and the
// resolution is pure, so a cache would only add a second place that can be stale
// — and the QML side reads these on a property notify, not per frame.

QVariantList SettingsAdapter::recordingOverlayPresetOptions() const {
    QVariantList options;
    options.append(makeOption(models::TokenFor(models::RecordingOverlayPreset::Minimal), tr("Minimal")));
    options.append(makeOption(models::TokenFor(models::RecordingOverlayPreset::Custom), tr("Custom")));
    return options;
}

QString SettingsAdapter::recordingOverlayPreset() const {
    // Round-tripped through the policy so an unrecognised persisted value is
    // reported as the preset that is actually in effect, not as itself.
    return models::TokenFor(models::RecordingOverlayPresetFromToken(app_settings_.recording_overlay_preset));
}

QVariantList SettingsAdapter::diagnosticsOverlayPresetOptions() const {
    QVariantList options;
    options.append(makeOption(models::TokenFor(models::DiagnosticsOverlayPreset::Health), tr("Health")));
    options.append(makeOption(models::TokenFor(models::DiagnosticsOverlayPreset::Technical), tr("Technical")));
    options.append(makeOption(models::TokenFor(models::DiagnosticsOverlayPreset::Custom), tr("Custom")));
    return options;
}

QString SettingsAdapter::diagnosticsOverlayPreset() const {
    return models::TokenFor(models::DiagnosticsOverlayPresetFromToken(app_settings_.diagnostics_overlay_preset));
}

models::RecordingOverlayContent SettingsAdapter::resolvedRecordingOverlayContent() const {
    return models::ResolveRecordingOverlayContent(
        models::RecordingOverlayPresetFromToken(app_settings_.recording_overlay_preset),
        app_settings_.recording_overlay_custom_elements);
}

models::DiagnosticsOverlayContent SettingsAdapter::resolvedDiagnosticsOverlayContent() const {
    return models::ResolveDiagnosticsOverlayContent(
        models::DiagnosticsOverlayPresetFromToken(app_settings_.diagnostics_overlay_preset),
        app_settings_.diagnostics_overlay_custom_elements);
}

bool SettingsAdapter::recordingOverlayElapsed() const {
    return resolvedRecordingOverlayContent().elapsed;
}
bool SettingsAdapter::recordingOverlayOutputSize() const {
    return resolvedRecordingOverlayContent().output_size;
}
bool SettingsAdapter::recordingOverlaySourceName() const {
    return resolvedRecordingOverlayContent().source_name;
}

bool SettingsAdapter::diagnosticsOverlayFps() const {
    return resolvedDiagnosticsOverlayContent().fps;
}
bool SettingsAdapter::diagnosticsOverlayDrop() const {
    return resolvedDiagnosticsOverlayContent().drop;
}
bool SettingsAdapter::diagnosticsOverlayDrift() const {
    return resolvedDiagnosticsOverlayContent().drift;
}
bool SettingsAdapter::diagnosticsOverlaySize() const {
    return resolvedDiagnosticsOverlayContent().size;
}
bool SettingsAdapter::diagnosticsOverlayMutedSources() const {
    return resolvedDiagnosticsOverlayContent().muted_sources;
}

void SettingsAdapter::setRecordingOverlayElement(const QString& token, bool enabled) {
    models::RecordingOverlayContent content = resolvedRecordingOverlayContent();
    if (token == models::TokenFor(models::RecordingOverlayElement::Elapsed)) {
        content.elapsed = enabled;
    } else if (token == models::TokenFor(models::RecordingOverlayElement::OutputSize)) {
        content.output_size = enabled;
    } else if (token == models::TokenFor(models::RecordingOverlayElement::SourceName)) {
        content.source_name = enabled;
    } else {
        return;
    }

    const QString tokens = models::TokensForRecordingOverlayContent(content);
    const QString preset = models::TokenFor(models::RecordingOverlayPreset::Custom);
    if (app_settings_.recording_overlay_custom_elements == tokens && app_settings_.recording_overlay_preset == preset) {
        return;
    }
    app_settings_.recording_overlay_custom_elements = tokens;
    // An element edit IS the definition of a custom set. Leaving the preset on
    // Minimal would persist a list the resolver then ignores, so the next read
    // would silently undo what the user just ticked.
    app_settings_.recording_overlay_preset = preset;
    commitAppSettingsEdit();
}

void SettingsAdapter::setDiagnosticsOverlayElement(const QString& token, bool enabled) {
    models::DiagnosticsOverlayContent content = resolvedDiagnosticsOverlayContent();
    if (token == models::TokenFor(models::DiagnosticsOverlayElement::Fps)) {
        content.fps = enabled;
    } else if (token == models::TokenFor(models::DiagnosticsOverlayElement::Drop)) {
        content.drop = enabled;
    } else if (token == models::TokenFor(models::DiagnosticsOverlayElement::Drift)) {
        content.drift = enabled;
    } else if (token == models::TokenFor(models::DiagnosticsOverlayElement::Size)) {
        content.size = enabled;
    } else if (token == models::TokenFor(models::DiagnosticsOverlayElement::MutedSources)) {
        content.muted_sources = enabled;
    } else {
        return;
    }

    const QString tokens = models::TokensForDiagnosticsOverlayContent(content);
    const QString preset = models::TokenFor(models::DiagnosticsOverlayPreset::Custom);
    if (app_settings_.diagnostics_overlay_custom_elements == tokens &&
        app_settings_.diagnostics_overlay_preset == preset) {
        return;
    }
    app_settings_.diagnostics_overlay_custom_elements = tokens;
    app_settings_.diagnostics_overlay_preset = preset;
    commitAppSettingsEdit();
}

QVariantList SettingsAdapter::appearanceOptions() const {
    // Read from the canonical tables rather than restating ids here -- a
    // hand-written list silently offers values that do not exist.
    return QuickThemeTokens::appearanceOptions();
}

QString SettingsAdapter::appearanceId() const {
    return app_settings_.appearance_id;
}

QVariantList SettingsAdapter::accentOptions() const {
    return QuickThemeTokens::accentOptions(app_settings_.appearance_id);
}

QString SettingsAdapter::accentId() const {
    return app_settings_.accent_id;
}

QVariantList SettingsAdapter::logLevelOptions() const {
    QVariantList options;
    for (const auto& level : {QStringLiteral("Off"), QStringLiteral("Error"), QStringLiteral("Warning"),
                              QStringLiteral("Info"), QStringLiteral("Debug")}) {
        options.append(makeOption(level, level));
    }
    return options;
}

QString SettingsAdapter::developerLogLevel() const {
    return app_settings_.developer_log_level;
}

QVariantList SettingsAdapter::crashReportPolicyOptions() const {
    QVariantList options;
    options.append(makeOption(static_cast<int>(CrashReportPolicy::AskEveryTime), tr("Ask every time")));
    options.append(makeOption(static_cast<int>(CrashReportPolicy::AlwaysSend), tr("Always send")));
    options.append(makeOption(static_cast<int>(CrashReportPolicy::NeverSend), tr("Never send")));
    return options;
}

int SettingsAdapter::crashReportPolicy() const noexcept {
    return static_cast<int>(app_settings_.crash_report_policy);
}

QVariantList SettingsAdapter::updateChannelOptions() const {
    QVariantList options;
    options.append(makeOption(QStringLiteral("Stable"), tr("Stable")));
    options.append(makeOption(QStringLiteral("Preview"), tr("Preview")));
    return options;
}

QString SettingsAdapter::updateChannel() const {
    return app_settings_.update_channel;
}
bool SettingsAdapter::autoUpdateCheck() const noexcept {
    return app_settings_.check_updates_on_start;
}
const QString& SettingsAdapter::updateState() const noexcept {
    return update_state_;
}
const QString& SettingsAdapter::updateStatusText() const noexcept {
    return update_status_text_;
}
const QString& SettingsAdapter::updateActionText() const noexcept {
    return update_action_text_;
}
bool SettingsAdapter::updateActionEnabled() const noexcept {
    return update_action_enabled_ && !controls_locked_;
}
bool SettingsAdapter::updateAvailable() const noexcept {
    return update_state_ == QLatin1String("available");
}
bool SettingsAdapter::whatsNewAvailable() const noexcept {
    return whats_new_available_;
}
const QVariantList& SettingsAdapter::presetOptions() const noexcept {
    return preset_options_;
}
const QString& SettingsAdapter::selectedPresetId() const noexcept {
    return selected_preset_id_;
}
const QString& SettingsAdapter::selectedPresetName() const noexcept {
    return selected_preset_name_;
}
bool SettingsAdapter::presetDirty() const noexcept {
    return preset_dirty_;
}
bool SettingsAdapter::presetBuiltIn() const noexcept {
    return preset_built_in_;
}
const QString& SettingsAdapter::presetStatusText() const noexcept {
    return preset_status_text_;
}

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------

void SettingsAdapter::setExpertMode(bool enabled) {
    if (app_settings_.expert_mode_enabled == enabled) {
        return;
    }
    app_settings_.expert_mode_enabled = enabled;
    commitAppSettingsEdit();
}

void SettingsAdapter::setContainer(int value) {
    const auto container = static_cast<Container>(value);
    if (config_.output.container == container) {
        return;
    }
    config_.output.container = container;
    applyConfigEdit();
}

void SettingsAdapter::setVideoCodec(int value) {
    const auto codec = static_cast<VideoCodec>(value);
    if (config_.output.video_codec == codec) {
        return;
    }
    config_.output.video_codec = codec;
    applyConfigEdit();
}

void SettingsAdapter::setAudioCodec(int value) {
    const auto codec = static_cast<AudioCodec>(value);
    if (config_.output.audio_codec == codec) {
        return;
    }
    config_.output.audio_codec = codec;
    applyConfigEdit();
}

void SettingsAdapter::setBitDepth(int value) {
    const auto depth = static_cast<BitDepth>(value);
    if (config_.output.bit_depth == depth) {
        return;
    }
    config_.output.bit_depth = depth;
    applyConfigEdit();
}

void SettingsAdapter::setChroma(int value) {
    const auto chroma_value = static_cast<ChromaSubsampling>(value);
    if (config_.output.chroma_subsampling == chroma_value) {
        return;
    }
    config_.output.chroma_subsampling = chroma_value;
    applyConfigEdit();
}

void SettingsAdapter::setColorRange(int value) {
    const auto range = static_cast<ColorRange>(value);
    if (config_.output.color_range == range) {
        return;
    }
    config_.output.color_range = range;
    applyConfigEdit();
}

void SettingsAdapter::setHdrMode(int value) {
    const auto mode = static_cast<recorder_core::HdrMode>(value);
    if (config_.output.hdr_mode == mode) {
        return;
    }
    config_.output.hdr_mode = mode;
    applyConfigEdit();
}

void SettingsAdapter::setEncoderPreset(int value) {
    const auto preset = static_cast<recorder_core::NvencPreset>(value);
    if (config_.output.nvenc_preset == preset) {
        return;
    }
    config_.output.nvenc_preset = preset;
    applyConfigEdit();
}

void SettingsAdapter::setQualityPreset(int value) {
    const uint32_t canonical = recorder_core::CanonicalCq(static_cast<recorder_core::QualityPreset>(value));
    if (config_.video.cq == canonical) {
        return;
    }
    config_.video.cq = canonical;
    applyConfigEdit();
}

void SettingsAdapter::setCq(int value) {
    const auto clamped = static_cast<uint32_t>(
        std::clamp(value, static_cast<int>(recorder_core::kCqMin), static_cast<int>(recorder_core::kCqMax)));
    if (config_.video.cq == clamped) {
        return;
    }
    config_.video.cq = clamped;
    applyConfigEdit();
}

void SettingsAdapter::setRateControl(int value) {
    const auto mode = static_cast<recorder_core::RateControlMode>(value);
    if (config_.video.rate_control == mode) {
        return;
    }
    config_.video.rate_control = mode;
    applyConfigEdit();
}

void SettingsAdapter::setBitrateKbps(int value) {
    const auto clamped = static_cast<uint32_t>(std::max(1, value));
    if (config_.video.bitrate_kbps == clamped) {
        return;
    }
    config_.video.bitrate_kbps = clamped;
    applyConfigEdit();
}

void SettingsAdapter::setFrameRate(int value) {
    int fps = std::max(1, value);
    if (max_frame_rate_ > 0) {
        fps = std::min(fps, max_frame_rate_);
    }
    if (config_.video.frame_rate_num == static_cast<uint32_t>(fps) && config_.video.frame_rate_den == 1) {
        return;
    }
    config_.video.frame_rate_num = static_cast<uint32_t>(fps);
    config_.video.frame_rate_den = 1;
    applyConfigEdit();
}

void SettingsAdapter::setCfr(bool value) {
    if (config_.video.cfr == value) {
        return;
    }
    config_.video.cfr = value;
    applyConfigEdit();
}

void SettingsAdapter::setFramePacing(int value) {
    const auto mode = static_cast<recorder_core::FramePacingMode>(value);
    if (config_.video.frame_pacing == mode) {
        return;
    }
    config_.video.frame_pacing = mode;
    applyConfigEdit();
}

void SettingsAdapter::setKeyframeInterval(int value) {
    const auto mode = static_cast<KeyframeIntervalMode>(value);
    if (config_.video.keyframe_interval == mode) {
        return;
    }
    config_.video.keyframe_interval = mode;
    applyConfigEdit();
}

void SettingsAdapter::setCaptureCursor(bool value) {
    if (config_.video.capture_cursor == value) {
        return;
    }
    config_.video.capture_cursor = value;
    applyConfigEdit();
}

void SettingsAdapter::setOutputFolder(const QString& value) {
    const NormalizedOutputFolder normalized = NormalizeOutputFolderInput(value.toStdWString());
    const std::filesystem::path resolved = normalized.result == OutputFolderPolicyResult::Ok
                                               ? normalized.resolved_path
                                               : std::filesystem::path(value.toStdWString());
    if (config_.output.output_folder == resolved) {
        return;
    }
    config_.output.output_folder = resolved;
    applyConfigEdit();
}

void SettingsAdapter::setNamingPattern(const QString& value) {
    const NormalizedFilenamePattern normalized = NormalizeFilenamePatternInput(value.toStdWString());
    const std::wstring pattern =
        normalized.result == FilenamePatternPolicyResult::Ok ? normalized.normalized_pattern : value.toStdWString();
    if (config_.output.naming_pattern == pattern) {
        return;
    }
    config_.output.naming_pattern = pattern;
    applyConfigEdit();
}

void SettingsAdapter::setResolutionMode(int value) {
    const auto mode = static_cast<OutputResolutionMode>(value);
    if (value == resolutionMode()) {
        return;
    }
    custom_resolution_pending_ = mode == OutputResolutionMode::Custom;
    config_.output.resolution.mode = mode;
    if (custom_resolution_pending_) {
        config_.output.resolution.custom_width = pending_custom_width_;
        config_.output.resolution.custom_height = pending_custom_height_;
    }
    applyConfigEdit();
}

void SettingsAdapter::setCustomWidth(int value) {
    const auto width = static_cast<uint32_t>(std::max(0, value));
    if (pending_custom_width_ == width) {
        return;
    }
    pending_custom_width_ = width;
    if (custom_resolution_pending_) {
        config_.output.resolution.mode = OutputResolutionMode::Custom;
        config_.output.resolution.custom_width = pending_custom_width_;
        config_.output.resolution.custom_height = pending_custom_height_;
    }
    applyConfigEdit();
}

void SettingsAdapter::setCustomHeight(int value) {
    const auto height = static_cast<uint32_t>(std::max(0, value));
    if (pending_custom_height_ == height) {
        return;
    }
    pending_custom_height_ = height;
    if (custom_resolution_pending_) {
        config_.output.resolution.mode = OutputResolutionMode::Custom;
        config_.output.resolution.custom_width = pending_custom_width_;
        config_.output.resolution.custom_height = pending_custom_height_;
    }
    applyConfigEdit();
}

void SettingsAdapter::setSplitByTimeEnabled(bool value) {
    const bool currently_on = config_.output.split.mode != SplitRecordingMode::Off;
    if (currently_on == value) {
        return;
    }
    config_.output.split.mode = value ? SplitRecordingMode::Every30Min : SplitRecordingMode::Off;
    applyConfigEdit();
}

void SettingsAdapter::setSplitMode(int value) {
    const auto mode = static_cast<SplitRecordingMode>(value);
    if (config_.output.split.mode == mode) {
        return;
    }
    config_.output.split.mode = mode;
    applyConfigEdit();
}

void SettingsAdapter::setSplitCustomMinutes(int value) {
    const auto minutes = static_cast<uint32_t>(std::max(0, value));
    if (config_.output.split.custom_minutes == minutes) {
        return;
    }
    config_.output.split.custom_minutes = minutes;
    applyConfigEdit();
}

void SettingsAdapter::setSplitBySizeEnabled(bool value) {
    const bool currently_on = config_.output.split.size_mode != SplitSizeMode::Off;
    if (currently_on == value) {
        return;
    }
    config_.output.split.size_mode = value ? SplitSizeMode::Custom : SplitSizeMode::Off;
    applyConfigEdit();
}

void SettingsAdapter::setSplitCustomSizeMb(int value) {
    const auto size = static_cast<uint32_t>(std::max(0, value));
    if (config_.output.split.custom_size_mb == size) {
        return;
    }
    config_.output.split.custom_size_mb = size;
    applyConfigEdit();
}

void SettingsAdapter::setAppAudioEnabled(bool value) {
    setRowEnabled(recorder_core::AudioSourceKind::App, value);
}
void SettingsAdapter::setAppAudioSeparate(bool value) {
    setRowSeparate(recorder_core::AudioSourceKind::App, value);
}
void SettingsAdapter::setSystemAudioEnabled(bool value) {
    setRowEnabled(recorder_core::AudioSourceKind::Sys, value);
}
void SettingsAdapter::setSystemAudioSeparate(bool value) {
    setRowSeparate(recorder_core::AudioSourceKind::Sys, value);
}
void SettingsAdapter::setMicrophoneEnabled(bool value) {
    setRowEnabled(recorder_core::AudioSourceKind::Mic, value);
}
void SettingsAdapter::setMicrophoneSeparate(bool value) {
    setRowSeparate(recorder_core::AudioSourceKind::Mic, value);
}

void SettingsAdapter::setMicrophoneDeviceId(const QString& value) {
    std::optional<std::string> device_id;
    if (!value.isEmpty()) {
        device_id = value.toStdString();
    }
    if (config_.audio.selected_mic_device_id == device_id) {
        return;
    }
    config_.audio.selected_mic_device_id = device_id;
    applyConfigEdit();
}

void SettingsAdapter::setMicChannelMode(int value) {
    const auto mode = static_cast<recorder_core::MicChannelMode>(value);
    if (config_.audio.mic_channel_mode == mode) {
        return;
    }
    config_.audio.mic_channel_mode = mode;
    applyConfigEdit();
}

void SettingsAdapter::setMicGainDb(double value) {
    const auto linear = static_cast<float>(std::pow(10.0, std::clamp(value, -60.0, 24.0) / 20.0));
    if (std::abs(config_.audio.mic_gain_linear - linear) < 1e-4f) {
        return;
    }
    config_.audio.mic_gain_linear = linear;
    applyConfigEdit();
}

void SettingsAdapter::setAudioBitrateKbps(int value) {
    const auto bitrate = static_cast<uint32_t>(std::max(0, value));
    if (config_.audio.audio_bitrate_kbps == bitrate) {
        return;
    }
    config_.audio.audio_bitrate_kbps = bitrate;
    applyConfigEdit();
}

void SettingsAdapter::setAudioSampleRate(int value) {
    const auto rate = static_cast<uint32_t>(std::max(0, value));
    if (config_.audio.audio_sample_rate == rate) {
        return;
    }
    config_.audio.audio_sample_rate = rate;
    applyConfigEdit();
}

void SettingsAdapter::setAudioChannels(int value) {
    const auto channels = static_cast<uint32_t>(std::clamp(value, 1, 2));
    if (config_.audio.audio_channels == channels) {
        return;
    }
    config_.audio.audio_channels = channels;
    applyConfigEdit();
}

void SettingsAdapter::setAudioBitDepth(int value) {
    const auto depth = static_cast<uint32_t>(std::max(0, value));
    if (config_.audio.audio_bit_depth == depth) {
        return;
    }
    config_.audio.audio_bit_depth = depth;
    applyConfigEdit();
}

void SettingsAdapter::setFlacCompressionLevel(int value) {
    const int level = std::clamp(value, 0, 8);
    if (config_.audio.flac_compression_level == level) {
        return;
    }
    config_.audio.flac_compression_level = level;
    applyConfigEdit();
}

void SettingsAdapter::setLimiterEnabled(bool value) {
    if (config_.audio.limiter_enabled == value) {
        return;
    }
    config_.audio.limiter_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setLimiterCeilingDb(double value) {
    const auto ceiling = static_cast<float>(std::min(0.0, value));
    if (std::abs(config_.audio.limiter_ceiling_db - ceiling) < 1e-4f) {
        return;
    }
    config_.audio.limiter_ceiling_db = ceiling;
    applyConfigEdit();
}

void SettingsAdapter::setClockSlavingEnabled(bool value) {
    if (config_.audio.clock_slaving_enabled == value) {
        return;
    }
    config_.audio.clock_slaving_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setOpusFrameDuration(int value) {
    const auto duration = static_cast<recorder_core::OpusFrameDuration>(value);
    if (config_.audio.opus_frame_duration == duration) {
        return;
    }
    config_.audio.opus_frame_duration = duration;
    applyConfigEdit();
}

void SettingsAdapter::setOpusComplexity(int value) {
    const int complexity = std::clamp(value, 0, 10);
    if (config_.audio.opus_complexity == complexity) {
        return;
    }
    config_.audio.opus_complexity = complexity;
    applyConfigEdit();
}

void SettingsAdapter::setMicHpfEnabled(bool value) {
    if (config_.audio.mic_hpf_enabled == value) {
        return;
    }
    config_.audio.mic_hpf_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setMicHpfCutoffHz(double value) {
    const auto cutoff = static_cast<float>(value);
    if (std::abs(config_.audio.mic_hpf_cutoff_hz - cutoff) < 1e-3f) {
        return;
    }
    config_.audio.mic_hpf_cutoff_hz = cutoff;
    applyConfigEdit();
}

void SettingsAdapter::setMicGateEnabled(bool value) {
    if (config_.audio.mic_gate_enabled == value) {
        return;
    }
    config_.audio.mic_gate_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setMicGateThresholdDb(double value) {
    const auto threshold = static_cast<float>(value);
    if (std::abs(config_.audio.mic_gate_threshold_db - threshold) < 1e-3f) {
        return;
    }
    config_.audio.mic_gate_threshold_db = threshold;
    applyConfigEdit();
}

void SettingsAdapter::setMicAgcEnabled(bool value) {
    if (config_.audio.mic_agc_enabled == value) {
        return;
    }
    config_.audio.mic_agc_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setMicAgcTargetDb(double value) {
    const auto target = static_cast<float>(value);
    if (std::abs(config_.audio.mic_agc_target_db - target) < 1e-3f) {
        return;
    }
    config_.audio.mic_agc_target_db = target;
    applyConfigEdit();
}

void SettingsAdapter::setMicRnnoiseEnabled(bool value) {
    if (config_.audio.mic_rnnoise_enabled == value) {
        return;
    }
    config_.audio.mic_rnnoise_enabled = value;
    applyConfigEdit();
}

void SettingsAdapter::setShowRecordingOverlay(bool value) {
    if (app_settings_.show_recording_overlay == value) {
        return;
    }
    app_settings_.show_recording_overlay = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setShowDiagnosticsOverlay(bool value) {
    if (app_settings_.show_diagnostics_overlay == value) {
        return;
    }
    app_settings_.show_diagnostics_overlay = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setShowNotifications(bool value) {
    if (app_settings_.show_notifications == value) {
        return;
    }
    app_settings_.show_notifications = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setRecordingOverlayPreset(const QString& value) {
    // Normalised before the compare, so selecting the preset that is already in
    // effect never rewrites the settings file with a differently-spelled token.
    const QString normalized = models::TokenFor(models::RecordingOverlayPresetFromToken(value));
    if (app_settings_.recording_overlay_preset == normalized) {
        return;
    }
    app_settings_.recording_overlay_preset = normalized;
    commitAppSettingsEdit();
}

void SettingsAdapter::setDiagnosticsOverlayPreset(const QString& value) {
    const QString normalized = models::TokenFor(models::DiagnosticsOverlayPresetFromToken(value));
    if (app_settings_.diagnostics_overlay_preset == normalized) {
        return;
    }
    app_settings_.diagnostics_overlay_preset = normalized;
    commitAppSettingsEdit();
}

void SettingsAdapter::setShowQuickControls(bool value) {
    if (app_settings_.show_quick_controls == value) {
        return;
    }
    app_settings_.show_quick_controls = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setKeepRunningInTray(bool value) {
    if (app_settings_.keep_running_in_tray == value) {
        return;
    }
    app_settings_.keep_running_in_tray = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setOpenEditorWhenFinished(bool value) {
    if (app_settings_.open_editor_when_finished == value) {
        return;
    }
    app_settings_.open_editor_when_finished = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setPresentDiagnosticsOptIn(bool value) {
    if (app_settings_.present_diagnostics_optin == value) {
        return;
    }
    app_settings_.present_diagnostics_optin = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setAppearanceId(const QString& value) {
    if (app_settings_.appearance_id == value) {
        return;
    }
    app_settings_.appearance_id = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setAccentId(const QString& value) {
    if (app_settings_.accent_id == value) {
        return;
    }
    app_settings_.accent_id = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setDeveloperLogLevel(const QString& value) {
    if (app_settings_.developer_log_level == value) {
        return;
    }
    app_settings_.developer_log_level = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setCrashReportPolicy(int value) {
    const auto policy = static_cast<CrashReportPolicy>(value);
    if (app_settings_.crash_report_policy == policy) {
        return;
    }
    app_settings_.crash_report_policy = policy;
    commitAppSettingsEdit();
}

void SettingsAdapter::setUpdateChannel(const QString& value) {
    if (app_settings_.update_channel == value) {
        return;
    }
    app_settings_.update_channel = value;
    commitAppSettingsEdit();
}

void SettingsAdapter::setAutoUpdateCheck(bool value) {
    if (app_settings_.check_updates_on_start == value) {
        return;
    }
    app_settings_.check_updates_on_start = value;
    commitAppSettingsEdit();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void SettingsAdapter::selectPreset(const QString& id) {
    emit presetSelected(id);
}
void SettingsAdapter::savePresetAs(const QString& name) {
    emit savePresetAsRequested(name);
}
void SettingsAdapter::renamePreset(const QString& name) {
    emit renamePresetRequested(name);
}
void SettingsAdapter::deletePreset() {
    emit deletePresetRequested();
}
void SettingsAdapter::resetChanges() {
    emit resetChangesRequested();
}
void SettingsAdapter::exportPresetToUrl(const QUrl& url) {
    if (!url.isLocalFile()) {
        return;
    }
    emit exportPresetRequested(url.toLocalFile());
}
void SettingsAdapter::importPresetsFromUrl(const QUrl& url) {
    if (!url.isLocalFile()) {
        return;
    }
    emit importPresetsRequested(url.toLocalFile());
}
void SettingsAdapter::setOutputFolderFromUrl(const QUrl& url) {
    if (!url.isLocalFile()) {
        return;
    }
    setOutputFolder(url.toLocalFile());
}
void SettingsAdapter::rescanAudioDevices() {
    emit audioRescanRequested();
}
void SettingsAdapter::checkForUpdates() {
    emit checkForUpdatesRequested();
}
void SettingsAdapter::runUpdatePrimaryAction() {
    emit updatePrimaryActionRequested();
}
void SettingsAdapter::showWhatsNew() {
    emit whatsNewRequested();
}
void SettingsAdapter::openDiagnostics() {
    emit diagnosticsRequested();
}

bool SettingsAdapter::presetNameRejected(const QString& name, const QString& exclude_id) const {
    const std::string folded = FoldPresetName(name.toStdString());
    if (folded.empty()) {
        return true;
    }
    for (const QVariant& entry : preset_options_) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("value")).toString() == exclude_id) {
            continue;
        }
        if (FoldPresetName(map.value(QStringLiteral("label")).toString().toStdString()) == folded) {
            return true;
        }
    }
    return false;
}

} // namespace exosnap::quick
