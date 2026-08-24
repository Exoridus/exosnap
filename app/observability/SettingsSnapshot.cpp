#include "observability/SettingsSnapshot.h"

#include "observability/ObservabilityJson.h"
#include "observability/ProtocolNames.h"

#include <QJsonArray>

namespace exosnap::observability {
namespace {

QString ResolutionModeName(OutputResolutionMode mode) {
    switch (mode) {
    case OutputResolutionMode::Native:
        return QStringLiteral("native");
    case OutputResolutionMode::UHD2160:
        return QStringLiteral("uhd2160");
    case OutputResolutionMode::QHD1440:
        return QStringLiteral("qhd1440");
    case OutputResolutionMode::FHD1080:
        return QStringLiteral("fhd1080");
    case OutputResolutionMode::HD720:
        return QStringLiteral("hd720");
    case OutputResolutionMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("native");
}

QString SplitModeName(SplitRecordingMode mode) {
    switch (mode) {
    case SplitRecordingMode::Off:
        return QStringLiteral("off");
    case SplitRecordingMode::Every15Min:
        return QStringLiteral("every15min");
    case SplitRecordingMode::Every30Min:
        return QStringLiteral("every30min");
    case SplitRecordingMode::Every60Min:
        return QStringLiteral("every60min");
    case SplitRecordingMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("off");
}

QString MicChannelModeName(exosnap::engine::MicChannelMode mode) {
    using exosnap::engine::MicChannelMode;
    switch (mode) {
    case MicChannelMode::Auto:
        return QStringLiteral("auto");
    case MicChannelMode::PreserveStereo:
        return QStringLiteral("preserveStereo");
    case MicChannelMode::MonoMix:
        return QStringLiteral("monoMix");
    case MicChannelMode::LeftToStereo:
        return QStringLiteral("leftToStereo");
    case MicChannelMode::RightToStereo:
        return QStringLiteral("rightToStereo");
    }
    return QStringLiteral("auto");
}

QString AudioSourceKindName(exosnap::engine::AudioSourceKind kind) {
    using exosnap::engine::AudioSourceKind;
    switch (kind) {
    case AudioSourceKind::App:
        return QStringLiteral("app");
    case AudioSourceKind::Mic:
        return QStringLiteral("mic");
    case AudioSourceKind::Sys:
        return QStringLiteral("sys");
    case AudioSourceKind::SystemOutput:
        return QStringLiteral("systemOutput");
    }
    return QStringLiteral("app");
}

double KeyframeIntervalSeconds(KeyframeIntervalMode mode) {
    switch (mode) {
    case KeyframeIntervalMode::Seconds2:
        return 2.0;
    case KeyframeIntervalMode::Seconds1:
        return 1.0;
    case KeyframeIntervalMode::Seconds0_5:
        return 0.5;
    }
    return 2.0;
}

QJsonObject VideoConfigJson(const RecordingPresetConfig& config) {
    const OutputSettingsModel& out = config.output;
    const VideoSettingsModel& video = config.video;

    QJsonObject json;
    json.insert(QStringLiteral("container"), ui::containerLabel(out.container));
    json.insert(QStringLiteral("videoCodec"), ui::videoCodecLabel(out.video_codec));
    json.insert(QStringLiteral("audioCodec"), ui::audioCodecLabel(out.audio_codec));

    json.insert(QStringLiteral("resolutionMode"), ResolutionModeName(out.resolution.mode));
    json.insert(QStringLiteral("width"), static_cast<double>(out.resolution.custom_width));
    json.insert(QStringLiteral("height"), static_cast<double>(out.resolution.custom_height));

    json.insert(QStringLiteral("frameRateNum"), static_cast<double>(video.frame_rate_num));
    json.insert(QStringLiteral("frameRateDen"), static_cast<double>(video.frame_rate_den));
    json.insert(QStringLiteral("cfr"), video.cfr);
    json.insert(QStringLiteral("framePacing"), FramePacingName(video.frame_pacing));

    json.insert(QStringLiteral("encoderPreset"), EncoderPresetName(out.nvenc_preset));
    json.insert(QStringLiteral("rateControl"), RateControlName(video.rate_control));
    json.insert(QStringLiteral("cq"), static_cast<double>(video.cq));
    json.insert(QStringLiteral("bitrateKbps"), static_cast<double>(video.bitrate_kbps));
    json.insert(QStringLiteral("keyframeIntervalSeconds"), KeyframeIntervalSeconds(video.keyframe_interval));

    json.insert(QStringLiteral("bitDepth"), BitDepthValue(out.bit_depth));
    json.insert(QStringLiteral("chroma"), ChromaName(out.chroma_subsampling));
    json.insert(QStringLiteral("colorRange"), ColorRangeName(out.color_range));
    json.insert(QStringLiteral("hdrMode"), HdrModeName(out.hdr_mode));
    json.insert(QStringLiteral("captureCursor"), video.capture_cursor);
    return json;
}

QJsonObject AudioConfigJson(const capability::AudioUiState& audio) {
    QJsonObject json;
    json.insert(QStringLiteral("sampleRate"), static_cast<double>(audio.audio_sample_rate));
    json.insert(QStringLiteral("channels"), static_cast<double>(audio.audio_channels));
    json.insert(QStringLiteral("bitDepth"), static_cast<double>(audio.audio_bit_depth));
    json.insert(QStringLiteral("bitrateKbps"), static_cast<double>(audio.audio_bitrate_kbps));
    json.insert(QStringLiteral("pcmFloat"), audio.audio_pcm_float);
    json.insert(QStringLiteral("flacCompressionLevel"), audio.flac_compression_level);
    json.insert(QStringLiteral("opusFrameDurationSamples"),
                static_cast<double>(exosnap::engine::OpusFrameSizeSamples(audio.opus_frame_duration)));
    json.insert(QStringLiteral("opusComplexity"), audio.opus_complexity);
    json.insert(QStringLiteral("limiterEnabled"), audio.limiter_enabled);
    json.insert(QStringLiteral("limiterCeilingDb"), static_cast<double>(audio.limiter_ceiling_db));
    json.insert(QStringLiteral("clockSlavingEnabled"), audio.clock_slaving_enabled);

    QJsonObject mic;
    mic.insert(QStringLiteral("channelMode"), MicChannelModeName(audio.mic_channel_mode));
    mic.insert(QStringLiteral("gainLinear"), static_cast<double>(audio.mic_gain_linear));
    mic.insert(QStringLiteral("hpfEnabled"), audio.mic_hpf_enabled);
    mic.insert(QStringLiteral("hpfCutoffHz"), static_cast<double>(audio.mic_hpf_cutoff_hz));
    mic.insert(QStringLiteral("gateEnabled"), audio.mic_gate_enabled);
    mic.insert(QStringLiteral("gateThresholdDb"), static_cast<double>(audio.mic_gate_threshold_db));
    mic.insert(QStringLiteral("agcEnabled"), audio.mic_agc_enabled);
    mic.insert(QStringLiteral("agcTargetDb"), static_cast<double>(audio.mic_agc_target_db));
    mic.insert(QStringLiteral("rnnoiseEnabled"), audio.mic_rnnoise_enabled);
    // The device id is a user-chosen endpoint, not a diagnosis. Reported as
    // whether one is pinned rather than as the id itself.
    mic.insert(QStringLiteral("devicePinned"), audio.selected_mic_device_id.has_value());
    json.insert(QStringLiteral("microphone"), mic);

    // The row order IS the product model (APP, SYS, MIC), and merge_with_above is
    // what makes a row share a track with the one above it. Serialized in order,
    // never as a set.
    QJsonArray rows;
    for (const exosnap::engine::AudioSourceRow& row : audio.source_rows) {
        QJsonObject entry;
        entry.insert(QStringLiteral("source"), AudioSourceKindName(row.kind));
        entry.insert(QStringLiteral("enabled"), row.enabled);
        entry.insert(QStringLiteral("mergeWithAbove"), row.merge_with_above);
        entry.insert(QStringLiteral("gainDb"), static_cast<double>(row.gain_db));
        entry.insert(QStringLiteral("muted"), row.muted);
        rows.append(entry);
    }
    json.insert(QStringLiteral("rows"), rows);
    return json;
}

QJsonObject SplitConfigJson(const SplitRecordingSettings& split) {
    QJsonObject json;
    json.insert(QStringLiteral("timeMode"), SplitModeName(split.mode));
    json.insert(QStringLiteral("customMinutes"), static_cast<double>(split.custom_minutes));
    json.insert(QStringLiteral("sizeMode"),
                split.size_mode == SplitSizeMode::Custom ? QStringLiteral("custom") : QStringLiteral("off"));
    json.insert(QStringLiteral("customSizeMb"), static_cast<double>(split.custom_size_mb));
    // The resolved thresholds the engine is handed, so a reader does not have to
    // re-implement the mode -> milliseconds/bytes mapping.
    json.insert(QStringLiteral("resolvedDurationMs"), Count(SplitDurationMs(split)));
    json.insert(QStringLiteral("resolvedSizeBytes"), Count(SplitSizeBytes(split)));
    return json;
}

// Output destination, privacy-bounded. The naming pattern is a template the user
// wrote and carries nothing personal; the folder is reported as its ROOT only.
// Which volume a recording lands on is the whole of what a diagnosis needs
// (`disk.outputTarget` in the pipeline snapshot is scrubbed the same way), and
// the rest of the path is the user's business.
QJsonObject OutputConfigJson(const OutputSettingsModel& out) {
    QJsonObject json;
    json.insert(QStringLiteral("namingPattern"), QString::fromStdWString(out.naming_pattern));
    json.insert(QStringLiteral("folderRoot"),
                out.output_folder.empty()
                    ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(QString::fromStdWString(out.output_folder.root_path().wstring())));
    json.insert(QStringLiteral("folderConfigured"), !out.output_folder.empty());
    return json;
}

QJsonObject WebcamConfigJson(const WebcamSettings& webcam) {
    QJsonObject json;
    json.insert(QStringLiteral("enabled"), webcam.enabled);
    json.insert(QStringLiteral("mirror"), webcam.mirror);
    json.insert(QStringLiteral("width"), webcam.width);
    json.insert(QStringLiteral("height"), webcam.height);
    json.insert(QStringLiteral("fps"), webcam.fps);
    json.insert(QStringLiteral("opacity"), static_cast<double>(webcam.opacity));
    json.insert(QStringLiteral("chromaKeyEnabled"), webcam.chroma_key.enabled);
    json.insert(QStringLiteral("devicePinned"), !webcam.device_id.empty());
    return json;
}

QString LoadOutcomeName(SettingsLoadOutcome outcome) {
    switch (outcome) {
    case SettingsLoadOutcome::Loaded:
        return QStringLiteral("loaded");
    case SettingsLoadOutcome::DefaultsNoFile:
        return QStringLiteral("defaultsNoFile");
    case SettingsLoadOutcome::ReadFailed:
        return QStringLiteral("readFailed");
    }
    return QStringLiteral("defaultsNoFile");
}

QJsonObject AppSettingsJson(const PersistedAppSettings& app) {
    QJsonObject json;
    json.insert(QStringLiteral("appearance"), app.appearance_id);
    json.insert(QStringLiteral("accent"), app.accent_id);
    json.insert(QStringLiteral("expertMode"), app.expert_mode_enabled);
    json.insert(QStringLiteral("minimizeToTray"), app.minimize_to_tray);
    json.insert(QStringLiteral("hideWindowFromCapture"), app.hide_window_from_capture);
    json.insert(QStringLiteral("showNotifications"), app.show_notifications);
    json.insert(QStringLiteral("openEditorWhenFinished"), app.open_editor_when_finished);
    json.insert(QStringLiteral("checkUpdatesOnStart"), app.check_updates_on_start);
    json.insert(QStringLiteral("updateChannel"), app.update_channel);
    json.insert(QStringLiteral("presentDiagnosticsOptIn"), app.present_diagnostics_optin);
    json.insert(QStringLiteral("developerLogLevel"), app.developer_log_level);

    QJsonObject overlays;
    overlays.insert(QStringLiteral("showRecordingOverlay"), app.show_recording_overlay);
    overlays.insert(QStringLiteral("recordingOverlayPreset"), app.recording_overlay_preset);
    overlays.insert(QStringLiteral("recordingOverlayCustomElements"), app.recording_overlay_custom_elements);
    overlays.insert(QStringLiteral("showDiagnosticsOverlay"), app.show_diagnostics_overlay);
    overlays.insert(QStringLiteral("diagnosticsOverlayPreset"), app.diagnostics_overlay_preset);
    overlays.insert(QStringLiteral("diagnosticsOverlayCustomElements"), app.diagnostics_overlay_custom_elements);
    overlays.insert(QStringLiteral("showQuickControls"), app.show_quick_controls);
    json.insert(QStringLiteral("overlays"), overlays);

    QJsonArray hotkeys;
    for (const QString& binding : app.hotkey_bindings)
        hotkeys.append(TextOrNull(binding));
    json.insert(QStringLiteral("hotkeyBindings"), hotkeys);
    return json;
}

// The reason a resolver adjustment exists, keyed by the field it touched. Used to
// annotate a requested/effective difference with the resolver's own explanation
// instead of a guess.
QString ReasonForField(const capability::ResolveResult& resolution, const QString& field) {
    for (const capability::Adjustment& adjustment : resolution.adjustments) {
        if (QString::fromStdString(adjustment.field) == field)
            return QString::fromStdString(adjustment.reason);
    }
    return {};
}

// A flat requested-vs-effective diff. Both sides are the SAME serialization, so
// a field that reads differently really is a different setting -- not a different
// spelling of the same one.
QJsonArray Differences(const QJsonObject& requested, const QJsonObject& effective,
                       const capability::ResolveResult& resolution, const QString& prefix) {
    QJsonArray differences;
    for (auto it = requested.begin(); it != requested.end(); ++it) {
        const QString key = it.key();
        const QJsonValue theirs = effective.value(key);
        if (it.value().isObject() && theirs.isObject()) {
            const QJsonArray nested =
                Differences(it.value().toObject(), theirs.toObject(), resolution, prefix + key + QLatin1Char('.'));
            for (const QJsonValue& entry : nested)
                differences.append(entry);
            continue;
        }
        if (it.value() == theirs)
            continue;
        QJsonObject difference;
        const QString path = prefix + key;
        difference.insert(QStringLiteral("field"), path);
        difference.insert(QStringLiteral("requested"), it.value());
        difference.insert(QStringLiteral("effective"), theirs);
        difference.insert(QStringLiteral("reason"), TextOrNull(ReasonForField(resolution, key)));
        differences.append(difference);
    }
    return differences;
}

QJsonObject ConstraintsJson(const capability::ResolveResult& resolution, bool probed) {
    QJsonObject json;
    // Before the capability probe lands there is no verdict to report, and an
    // empty adjustment list would read as "nothing had to be changed".
    json.insert(QStringLiteral("evaluated"), probed);
    json.insert(QStringLiteral("valid"), probed ? QJsonValue(resolution.succeeded) : QJsonValue(QJsonValue::Null));

    QJsonArray adjustments;
    for (const capability::Adjustment& adjustment : resolution.adjustments) {
        QJsonObject entry;
        entry.insert(QStringLiteral("field"), QString::fromStdString(adjustment.field));
        entry.insert(QStringLiteral("from"), QString::fromStdString(adjustment.from));
        entry.insert(QStringLiteral("to"), QString::fromStdString(adjustment.to));
        entry.insert(QStringLiteral("reason"), QString::fromStdString(adjustment.reason));
        adjustments.append(entry);
    }
    json.insert(QStringLiteral("adjustments"), adjustments);

    QJsonArray warnings;
    for (const capability::Warning& warning : resolution.warnings) {
        QJsonObject entry;
        entry.insert(QStringLiteral("code"), QString::fromStdString(warning.code));
        entry.insert(QStringLiteral("message"), QString::fromStdString(warning.message));
        warnings.append(entry);
    }
    json.insert(QStringLiteral("warnings"), warnings);

    QJsonArray invalid;
    for (const capability::InvalidReason& reason : resolution.invalidity) {
        QJsonObject entry;
        entry.insert(QStringLiteral("field"), QString::fromStdString(reason.field));
        entry.insert(QStringLiteral("message"), QString::fromStdString(reason.message));
        invalid.append(entry);
    }
    json.insert(QStringLiteral("invalid"), invalid);
    return json;
}

QJsonObject RunningJson(const exosnap::engine::EncoderInitInfo& init, bool live) {
    QJsonObject json;
    json.insert(QStringLiteral("valid"), init.valid);
    json.insert(QStringLiteral("live"), live);
    if (!init.valid)
        return json;
    json.insert(QStringLiteral("videoCodec"), ui::videoCodecLabel(init.codec));
    json.insert(QStringLiteral("encoderPreset"), EncoderPresetName(init.preset));
    json.insert(QStringLiteral("rateControl"), RateControlName(init.rc_mode));
    json.insert(QStringLiteral("cq"), static_cast<double>(init.cq));
    json.insert(QStringLiteral("targetBitrateKbps"), static_cast<double>(init.target_bitrate_kbps));
    json.insert(QStringLiteral("maxBitrateKbps"), static_cast<double>(init.max_bitrate_kbps));
    json.insert(QStringLiteral("gopLength"), static_cast<double>(init.gop_length));
    json.insert(QStringLiteral("bframes"), static_cast<double>(init.bframes));
    json.insert(QStringLiteral("lookaheadFrames"), static_cast<double>(init.lookahead_frames));
    json.insert(QStringLiteral("temporalAQ"), init.temporal_aq);
    json.insert(QStringLiteral("spatialAQ"), init.spatial_aq);
    json.insert(QStringLiteral("bitDepth"), BitDepthValue(init.bit_depth));
    json.insert(QStringLiteral("chroma"), ChromaName(init.chroma));
    json.insert(QStringLiteral("colorRange"), ColorRangeName(init.color_full_range));
    json.insert(QStringLiteral("hdrMode"), HdrModeName(init.hdr_mode));
    return json;
}

} // namespace

QJsonObject RecordingConfigToJson(const RecordingPresetConfig& config) {
    QJsonObject json;
    json.insert(QStringLiteral("video"), VideoConfigJson(config));
    json.insert(QStringLiteral("audio"), AudioConfigJson(config.audio));
    json.insert(QStringLiteral("split"), SplitConfigJson(config.output.split));
    json.insert(QStringLiteral("output"), OutputConfigJson(config.output));
    json.insert(QStringLiteral("webcam"), WebcamConfigJson(config.webcam));
    json.insert(QStringLiteral("countdownSeconds"), config.countdown_seconds);
    return json;
}

QJsonObject SettingsSnapshotToJson(const SettingsSnapshotInputs& inputs) {
    const QJsonObject requested = RecordingConfigToJson(inputs.requested);
    const QJsonObject effective = RecordingConfigToJson(inputs.effective);

    QJsonObject json;
    json.insert(QStringLiteral("requested"), requested);
    json.insert(QStringLiteral("effective"), effective);
    json.insert(QStringLiteral("running"), RunningJson(inputs.running, inputs.running_live));
    json.insert(QStringLiteral("differences"), Differences(requested, effective, inputs.resolution, QString()));
    json.insert(QStringLiteral("constraints"), ConstraintsJson(inputs.resolution, inputs.capabilities_probed));
    json.insert(QStringLiteral("app"), AppSettingsJson(inputs.app));

    // Whether a settings file is configured at all, not where it is. The one
    // thing a client has to be able to tell apart is "this process persists
    // settings" from "this process is running on defaults it will never write",
    // and that is a boolean.
    QJsonObject persistence;
    persistence.insert(QStringLiteral("settingsFileConfigured"), !inputs.settingsFilePath.isEmpty());
    persistence.insert(QStringLiteral("loadOutcome"), LoadOutcomeName(inputs.app.load_outcome));
    json.insert(QStringLiteral("persistence"), persistence);
    return json;
}

} // namespace exosnap::observability
