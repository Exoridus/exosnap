#include "RecordingPresetStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringView>
#include <QVector>

#include "settings/ConfigPaths.h"

#include <algorithm>
#include <optional>
#include <set>

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Enum serializers (self-contained; mirrors AppSettingsStore converters)
// ---------------------------------------------------------------------------

QString ContainerToString(capability::Container v) {
    switch (v) {
    case capability::Container::Matroska:
        return QStringLiteral("mkv");
    case capability::Container::Mp4:
        return QStringLiteral("mp4");
    case capability::Container::WebM:
        return QStringLiteral("webm");
    }
    return QStringLiteral("mkv");
}

std::optional<capability::Container> ContainerFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("mkv"))
        return capability::Container::Matroska;
    if (n == QStringLiteral("mp4"))
        return capability::Container::Mp4;
    if (n == QStringLiteral("webm"))
        return capability::Container::WebM;
    return std::nullopt;
}

QString VideoCodecToString(capability::VideoCodec v) {
    switch (v) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("h264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("hevc");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("av1");
    }
    return QStringLiteral("h264");
}

std::optional<capability::VideoCodec> VideoCodecFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("h264"))
        return capability::VideoCodec::H264Nvenc;
    if (n == QStringLiteral("hevc"))
        return capability::VideoCodec::HevcNvenc;
    if (n == QStringLiteral("av1"))
        return capability::VideoCodec::Av1Nvenc;
    return std::nullopt;
}

QString AudioCodecToString(capability::AudioCodec v) {
    switch (v) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("aac");
    case capability::AudioCodec::Opus:
        return QStringLiteral("opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("pcm");
    }
    return QStringLiteral("aac");
}

std::optional<capability::AudioCodec> AudioCodecFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("opus"))
        return capability::AudioCodec::Opus;
    if (n == QStringLiteral("aac"))
        return capability::AudioCodec::AacMf;
    if (n == QStringLiteral("pcm"))
        return capability::AudioCodec::Pcm;
    return std::nullopt;
}

QString NvencQualityPresetToString(recorder_core::NvencQualityPreset v) {
    switch (v) {
    case recorder_core::NvencQualityPreset::High:
        return QStringLiteral("high");
    case recorder_core::NvencQualityPreset::Balanced:
        return QStringLiteral("balanced");
    case recorder_core::NvencQualityPreset::Small:
        return QStringLiteral("small");
    }
    return QStringLiteral("balanced");
}

std::optional<recorder_core::NvencQualityPreset> NvencQualityPresetFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("high"))
        return recorder_core::NvencQualityPreset::High;
    if (n == QStringLiteral("balanced"))
        return recorder_core::NvencQualityPreset::Balanced;
    if (n == QStringLiteral("small"))
        return recorder_core::NvencQualityPreset::Small;
    return std::nullopt;
}

QString RateControlModeToString(recorder_core::RateControlMode v) {
    switch (v) {
    case recorder_core::RateControlMode::ConstantQuality:
        return QStringLiteral("cq");
    case recorder_core::RateControlMode::VariableBitrate:
        return QStringLiteral("vbr");
    case recorder_core::RateControlMode::ConstantBitrate:
        return QStringLiteral("cbr");
    case recorder_core::RateControlMode::Lossless:
        return QStringLiteral("lossless");
    }
    return QStringLiteral("cq");
}

std::optional<recorder_core::RateControlMode> RateControlModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("cq"))
        return recorder_core::RateControlMode::ConstantQuality;
    if (n == QStringLiteral("vbr"))
        return recorder_core::RateControlMode::VariableBitrate;
    if (n == QStringLiteral("cbr"))
        return recorder_core::RateControlMode::ConstantBitrate;
    if (n == QStringLiteral("lossless"))
        return recorder_core::RateControlMode::Lossless;
    return std::nullopt;
}

QString MicChannelModeToString(recorder_core::MicChannelMode v) {
    switch (v) {
    case recorder_core::MicChannelMode::Auto:
        return QStringLiteral("auto");
    case recorder_core::MicChannelMode::PreserveStereo:
        return QStringLiteral("preserve");
    case recorder_core::MicChannelMode::MonoMix:
        return QStringLiteral("mono_mix");
    case recorder_core::MicChannelMode::LeftToStereo:
        return QStringLiteral("left");
    case recorder_core::MicChannelMode::RightToStereo:
        return QStringLiteral("right");
    }
    return QStringLiteral("auto");
}

std::optional<recorder_core::MicChannelMode> MicChannelModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("auto"))
        return recorder_core::MicChannelMode::Auto;
    if (n == QStringLiteral("preserve"))
        return recorder_core::MicChannelMode::PreserveStereo;
    if (n == QStringLiteral("mono_mix"))
        return recorder_core::MicChannelMode::MonoMix;
    if (n == QStringLiteral("left"))
        return recorder_core::MicChannelMode::LeftToStereo;
    if (n == QStringLiteral("right"))
        return recorder_core::MicChannelMode::RightToStereo;
    return std::nullopt;
}

QString CaptureTargetKindToString(capability::CaptureTargetKind v) {
    switch (v) {
    case capability::CaptureTargetKind::Window:
        return QStringLiteral("window");
    case capability::CaptureTargetKind::Display:
        return QStringLiteral("display");
    }
    return QStringLiteral("display");
}

std::optional<capability::CaptureTargetKind> CaptureTargetKindFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("window"))
        return capability::CaptureTargetKind::Window;
    if (n == QStringLiteral("display"))
        return capability::CaptureTargetKind::Display;
    return std::nullopt;
}

QString AudioSourceKindToString(recorder_core::AudioSourceKind v) {
    switch (v) {
    case recorder_core::AudioSourceKind::App:
        return QStringLiteral("app");
    case recorder_core::AudioSourceKind::Mic:
        return QStringLiteral("mic");
    case recorder_core::AudioSourceKind::Sys:
        return QStringLiteral("sys");
    case recorder_core::AudioSourceKind::SystemOutput:
        return QStringLiteral("system_output");
    }
    return QStringLiteral("sys");
}

std::optional<recorder_core::AudioSourceKind> AudioSourceKindFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("app"))
        return recorder_core::AudioSourceKind::App;
    if (n == QStringLiteral("mic"))
        return recorder_core::AudioSourceKind::Mic;
    if (n == QStringLiteral("sys"))
        return recorder_core::AudioSourceKind::Sys;
    if (n == QStringLiteral("system_output"))
        return recorder_core::AudioSourceKind::SystemOutput;
    return std::nullopt;
}

QString WebcamChromaKeyColorModeToString(WebcamChromaKeyColorMode v) {
    switch (v) {
    case WebcamChromaKeyColorMode::Green:
        return QStringLiteral("green");
    case WebcamChromaKeyColorMode::Blue:
        return QStringLiteral("blue");
    case WebcamChromaKeyColorMode::Magenta:
        return QStringLiteral("magenta");
    case WebcamChromaKeyColorMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("green");
}

std::optional<WebcamChromaKeyColorMode> WebcamChromaKeyColorModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("green"))
        return WebcamChromaKeyColorMode::Green;
    if (n == QStringLiteral("blue"))
        return WebcamChromaKeyColorMode::Blue;
    if (n == QStringLiteral("magenta"))
        return WebcamChromaKeyColorMode::Magenta;
    if (n == QStringLiteral("custom"))
        return WebcamChromaKeyColorMode::Custom;
    return std::nullopt;
}

QString PresetCaptureKindToString(PresetCaptureKind v) {
    switch (v) {
    case PresetCaptureKind::Display:
        return QStringLiteral("display");
    case PresetCaptureKind::Window:
        return QStringLiteral("window");
    case PresetCaptureKind::Region:
        return QStringLiteral("region");
    }
    return QStringLiteral("display");
}

std::optional<PresetCaptureKind> PresetCaptureKindFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("display"))
        return PresetCaptureKind::Display;
    if (n == QStringLiteral("window"))
        return PresetCaptureKind::Window;
    if (n == QStringLiteral("region"))
        return PresetCaptureKind::Region;
    return std::nullopt;
}

QString OutputResolutionModeToString(OutputResolutionMode v) {
    switch (v) {
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

std::optional<OutputResolutionMode> OutputResolutionModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("native"))
        return OutputResolutionMode::Native;
    if (n == QStringLiteral("uhd2160") || n == QStringLiteral("4k"))
        return OutputResolutionMode::UHD2160;
    if (n == QStringLiteral("qhd1440") || n == QStringLiteral("1440p"))
        return OutputResolutionMode::QHD1440;
    if (n == QStringLiteral("fhd1080") || n == QStringLiteral("1080p"))
        return OutputResolutionMode::FHD1080;
    if (n == QStringLiteral("hd720") || n == QStringLiteral("720p"))
        return OutputResolutionMode::HD720;
    if (n == QStringLiteral("custom"))
        return OutputResolutionMode::Custom;
    return std::nullopt;
}

QString SplitRecordingModeToString(SplitRecordingMode v) {
    switch (v) {
    case SplitRecordingMode::Off:
        return QStringLiteral("off");
    case SplitRecordingMode::Every15Min:
        return QStringLiteral("every15");
    case SplitRecordingMode::Every30Min:
        return QStringLiteral("every30");
    case SplitRecordingMode::Every60Min:
        return QStringLiteral("every60");
    case SplitRecordingMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("off");
}

std::optional<SplitRecordingMode> SplitRecordingModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("off"))
        return SplitRecordingMode::Off;
    if (n == QStringLiteral("every15"))
        return SplitRecordingMode::Every15Min;
    if (n == QStringLiteral("every30"))
        return SplitRecordingMode::Every30Min;
    if (n == QStringLiteral("every60"))
        return SplitRecordingMode::Every60Min;
    if (n == QStringLiteral("custom"))
        return SplitRecordingMode::Custom;
    return std::nullopt;
}

QString OutputFitModeToString(recorder_core::OutputFitMode v) {
    switch (v) {
    case recorder_core::OutputFitMode::Contain:
        return QStringLiteral("contain");
    }
    return QStringLiteral("contain");
}

std::optional<recorder_core::OutputFitMode> OutputFitModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("contain") || n == QStringLiteral("fit"))
        return recorder_core::OutputFitMode::Contain;
    return std::nullopt;
}

QString OpusFrameDurationToString(recorder_core::OpusFrameDuration v) {
    switch (v) {
    case recorder_core::OpusFrameDuration::Ms20:
        return QStringLiteral("20ms");
    case recorder_core::OpusFrameDuration::Ms10:
        return QStringLiteral("10ms");
    case recorder_core::OpusFrameDuration::Ms5:
        return QStringLiteral("5ms");
    case recorder_core::OpusFrameDuration::Ms2_5:
        return QStringLiteral("2.5ms");
    }
    return QStringLiteral("20ms");
}

std::optional<recorder_core::OpusFrameDuration> OpusFrameDurationFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("20ms"))
        return recorder_core::OpusFrameDuration::Ms20;
    if (n == QStringLiteral("10ms"))
        return recorder_core::OpusFrameDuration::Ms10;
    if (n == QStringLiteral("5ms"))
        return recorder_core::OpusFrameDuration::Ms5;
    if (n == QStringLiteral("2.5ms"))
        return recorder_core::OpusFrameDuration::Ms2_5;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Per-item save / load helpers
// ---------------------------------------------------------------------------

// Save a single RecordingPreset into the QSettings array item at the current
// array index.  The caller has already called settings.setArrayIndex(i).
void SavePresetItem(QSettings& settings, const RecordingPreset& preset) {
    settings.setValue(QStringLiteral("id"), QString::fromStdString(preset.id));
    settings.setValue(QStringLiteral("name"), QString::fromStdString(preset.name));

    // --- Capture ---
    const auto& cap = preset.config.capture;
    settings.setValue(QStringLiteral("capture_kind"), PresetCaptureKindToString(cap.kind));
    settings.setValue(QStringLiteral("capture_display_key"), QString::fromStdString(cap.display_key));
    settings.setValue(QStringLiteral("capture_window_key"), QString::fromStdString(cap.window_key));
    settings.setValue(QStringLiteral("capture_has_region"), cap.has_region);
    settings.setValue(QStringLiteral("capture_region_x"), cap.region.x);
    settings.setValue(QStringLiteral("capture_region_y"), cap.region.y);
    settings.setValue(QStringLiteral("capture_region_w"), cap.region.width);
    settings.setValue(QStringLiteral("capture_region_h"), cap.region.height);
    settings.setValue(QStringLiteral("capture_region_display_key"), QString::fromStdString(cap.region_display_key));

    // --- Output ---
    const auto& out = preset.config.output;
    settings.setValue(QStringLiteral("out_folder"), QString::fromStdWString(out.output_folder.wstring()));
    settings.setValue(QStringLiteral("out_naming_pattern"), QString::fromStdWString(out.naming_pattern));
    settings.setValue(QStringLiteral("out_container"), ContainerToString(out.container));
    settings.setValue(QStringLiteral("out_video_codec"), VideoCodecToString(out.video_codec));
    settings.setValue(QStringLiteral("out_audio_codec"), AudioCodecToString(out.audio_codec));
    settings.setValue(QStringLiteral("out_resolution_mode"), OutputResolutionModeToString(out.resolution.mode));
    settings.setValue(QStringLiteral("out_custom_width"), static_cast<int>(out.resolution.custom_width));
    settings.setValue(QStringLiteral("out_custom_height"), static_cast<int>(out.resolution.custom_height));
    settings.setValue(QStringLiteral("out_fit_mode"), OutputFitModeToString(out.resolution.fit));
    settings.setValue(QStringLiteral("out_split_mode"), SplitRecordingModeToString(out.split.mode));
    settings.setValue(QStringLiteral("out_split_custom_minutes"), static_cast<int>(out.split.custom_minutes));

    // --- Video ---
    const auto& vid = preset.config.video;
    settings.setValue(QStringLiteral("vid_quality"), NvencQualityPresetToString(vid.quality));
    settings.setValue(QStringLiteral("vid_rate_control"), RateControlModeToString(vid.rate_control));
    settings.setValue(QStringLiteral("vid_bitrate_kbps"), static_cast<int>(vid.bitrate_kbps));
    settings.setValue(QStringLiteral("vid_cfr"), vid.cfr);
    settings.setValue(QStringLiteral("vid_capture_cursor"), vid.capture_cursor);
    settings.setValue(QStringLiteral("vid_frame_rate_num"), static_cast<int>(vid.frame_rate_num));
    settings.setValue(QStringLiteral("vid_frame_rate_den"), static_cast<int>(vid.frame_rate_den));

    // --- Audio ---
    const auto& aud = preset.config.audio;
    settings.setValue(QStringLiteral("aud_target_kind"), CaptureTargetKindToString(aud.target_kind));
    settings.setValue(QStringLiteral("aud_mic_channel_mode"), MicChannelModeToString(aud.mic_channel_mode));
    settings.setValue(QStringLiteral("aud_selected_mic_device_id"),
                      aud.selected_mic_device_id.has_value() ? QString::fromStdString(*aud.selected_mic_device_id)
                                                             : QString());
    // Store as double for lossless round-trip.
    settings.setValue(QStringLiteral("aud_mic_gain_linear"), static_cast<double>(aud.mic_gain_linear));
    settings.setValue(QStringLiteral("aud_has_window_pid"), aud.selected_window_pid.has_value());
    settings.setValue(QStringLiteral("aud_window_pid"),
                      aud.selected_window_pid.has_value() ? static_cast<uint>(aud.selected_window_pid.value()) : 0u);
    settings.setValue(QStringLiteral("aud_row_count"), static_cast<int>(aud.source_rows.size()));
    for (int i = 0; i < static_cast<int>(aud.source_rows.size()); ++i) {
        const auto& row = aud.source_rows[static_cast<std::size_t>(i)];
        settings.setValue(QStringLiteral("aud_row_%1_kind").arg(i), AudioSourceKindToString(row.kind));
        settings.setValue(QStringLiteral("aud_row_%1_enabled").arg(i), row.enabled);
        settings.setValue(QStringLiteral("aud_row_%1_merge").arg(i), row.merge_with_above);
    }
    // Audio encoding params (ADR 0019).
    settings.setValue(QStringLiteral("aud_audio_bitrate_kbps"), static_cast<int>(aud.audio_bitrate_kbps));
    settings.setValue(QStringLiteral("aud_opus_frame_duration"), OpusFrameDurationToString(aud.opus_frame_duration));
    settings.setValue(QStringLiteral("aud_opus_complexity"), aud.opus_complexity);

    // --- Webcam ---
    const auto& wc = preset.config.webcam;
    settings.setValue(QStringLiteral("wc_enabled"), wc.enabled);
    settings.setValue(QStringLiteral("wc_device_id"), QString::fromStdString(wc.device_id));
    settings.setValue(QStringLiteral("wc_width"), wc.width);
    settings.setValue(QStringLiteral("wc_height"), wc.height);
    settings.setValue(QStringLiteral("wc_fps"), wc.fps);
    settings.setValue(QStringLiteral("wc_overlay_x"), static_cast<double>(wc.overlay.x_norm));
    settings.setValue(QStringLiteral("wc_overlay_y"), static_cast<double>(wc.overlay.y_norm));
    settings.setValue(QStringLiteral("wc_overlay_w"), static_cast<double>(wc.overlay.w_norm));
    settings.setValue(QStringLiteral("wc_overlay_h"), static_cast<double>(wc.overlay.h_norm));
    settings.setValue(QStringLiteral("wc_overlay_user_placed"), wc.overlay_user_placed);
    settings.setValue(QStringLiteral("wc_aspect_ratio_locked"), wc.aspect_ratio_locked);
    settings.setValue(QStringLiteral("wc_mirror"), wc.mirror);
    settings.setValue(QStringLiteral("wc_chroma_enabled"), wc.chroma_key.enabled);
    settings.setValue(QStringLiteral("wc_chroma_color_mode"),
                      WebcamChromaKeyColorModeToString(wc.chroma_key.color_mode));
    settings.setValue(QStringLiteral("wc_chroma_custom_r"), static_cast<int>(wc.chroma_key.custom_r));
    settings.setValue(QStringLiteral("wc_chroma_custom_g"), static_cast<int>(wc.chroma_key.custom_g));
    settings.setValue(QStringLiteral("wc_chroma_custom_b"), static_cast<int>(wc.chroma_key.custom_b));
    settings.setValue(QStringLiteral("wc_chroma_tolerance"), static_cast<double>(wc.chroma_key.tolerance));
    settings.setValue(QStringLiteral("wc_chroma_softness"), static_cast<double>(wc.chroma_key.softness));
    settings.setValue(QStringLiteral("wc_chroma_spill"), static_cast<double>(wc.chroma_key.spill_reduction));

    // --- Countdown ---
    settings.setValue(QStringLiteral("countdown_seconds"), preset.config.countdown_seconds);
}

// Parse a single array item at the current array index into a RecordingPreset.
// Returns nullopt if the item is malformed (empty id or other invariant failure).
std::optional<RecordingPreset> LoadPresetItem(QSettings& settings) {
    RecordingPreset preset;

    preset.id = settings.value(QStringLiteral("id")).toString().trimmed().toStdString();
    if (preset.id.empty()) {
        return std::nullopt; // Malformed — skip.
    }
    preset.name = settings.value(QStringLiteral("name")).toString().toStdString();

    // --- Capture ---
    auto& cap = preset.config.capture;
    {
        const auto kind = PresetCaptureKindFromString(settings.value(QStringLiteral("capture_kind")).toString());
        cap.kind = kind.value_or(PresetCaptureKind::Display);
    }
    cap.display_key = settings.value(QStringLiteral("capture_display_key")).toString().toStdString();
    cap.window_key = settings.value(QStringLiteral("capture_window_key")).toString().toStdString();
    cap.has_region = settings.value(QStringLiteral("capture_has_region"), false).toBool();
    cap.region.x = settings.value(QStringLiteral("capture_region_x"), 0).toInt();
    cap.region.y = settings.value(QStringLiteral("capture_region_y"), 0).toInt();
    cap.region.width = settings.value(QStringLiteral("capture_region_w"), 0).toInt();
    cap.region.height = settings.value(QStringLiteral("capture_region_h"), 0).toInt();
    cap.region_display_key = settings.value(QStringLiteral("capture_region_display_key")).toString().toStdString();

    // --- Output ---
    auto& out = preset.config.output;
    {
        const QString folder = settings.value(QStringLiteral("out_folder")).toString().trimmed();
        if (!folder.isEmpty()) {
            out.output_folder = std::filesystem::path(folder.toStdWString());
        }
    }
    {
        const QString pat = settings.value(QStringLiteral("out_naming_pattern")).toString();
        if (!pat.isEmpty()) {
            out.naming_pattern = pat.toStdWString();
        }
    }
    {
        const auto c = ContainerFromString(settings.value(QStringLiteral("out_container")).toString());
        if (c.has_value())
            out.container = *c;
    }
    {
        const auto c = VideoCodecFromString(settings.value(QStringLiteral("out_video_codec")).toString());
        if (c.has_value())
            out.video_codec = *c;
    }
    {
        const auto c = AudioCodecFromString(settings.value(QStringLiteral("out_audio_codec")).toString());
        if (c.has_value())
            out.audio_codec = *c;
    }
    {
        const auto m = OutputResolutionModeFromString(settings.value(QStringLiteral("out_resolution_mode")).toString());
        if (m.has_value())
            out.resolution.mode = *m;
    }
    {
        bool ok_w = false;
        bool ok_h = false;
        const int width = settings.value(QStringLiteral("out_custom_width"), 0).toInt(&ok_w);
        const int height = settings.value(QStringLiteral("out_custom_height"), 0).toInt(&ok_h);
        if (ok_w && ok_h && width >= 0 && height >= 0) {
            out.resolution.custom_width = static_cast<uint32_t>(width);
            out.resolution.custom_height = static_cast<uint32_t>(height);
        }
    }
    {
        const auto fit = OutputFitModeFromString(settings.value(QStringLiteral("out_fit_mode")).toString());
        if (fit.has_value())
            out.resolution.fit = *fit;
    }
    {
        const auto sm = SplitRecordingModeFromString(settings.value(QStringLiteral("out_split_mode")).toString());
        if (sm.has_value())
            out.split.mode = *sm;
        bool ok = false;
        const int minutes = settings.value(QStringLiteral("out_split_custom_minutes"), 30).toInt(&ok);
        if (ok && minutes > 0)
            out.split.custom_minutes = static_cast<uint32_t>(minutes);
    }

    // --- Video ---
    auto& vid = preset.config.video;
    {
        const auto q = NvencQualityPresetFromString(settings.value(QStringLiteral("vid_quality")).toString());
        if (q.has_value())
            vid.quality = *q;
    }
    {
        const auto rc = RateControlModeFromString(settings.value(QStringLiteral("vid_rate_control")).toString());
        if (rc.has_value())
            vid.rate_control = *rc;
        // If absent (older preset schema), defaults to ConstantQuality — no behavior change.
    }
    {
        bool ok = false;
        const int bk = settings.value(QStringLiteral("vid_bitrate_kbps"), 20000).toInt(&ok);
        if (ok && bk > 0)
            vid.bitrate_kbps = static_cast<uint32_t>(bk);
        // SanitizePreset() will clamp to [1000, 200000].
    }
    if (settings.contains(QStringLiteral("vid_cfr"))) {
        vid.cfr = settings.value(QStringLiteral("vid_cfr"), true).toBool();
    }
    if (settings.contains(QStringLiteral("vid_capture_cursor"))) {
        vid.capture_cursor = settings.value(QStringLiteral("vid_capture_cursor"), true).toBool();
    }
    {
        bool ok_num = false;
        bool ok_den = false;
        const int num = settings.value(QStringLiteral("vid_frame_rate_num"), 60).toInt(&ok_num);
        const int den = settings.value(QStringLiteral("vid_frame_rate_den"), 1).toInt(&ok_den);
        if (ok_num && ok_den && num > 0 && den > 0) {
            vid.frame_rate_num = static_cast<uint32_t>(num);
            vid.frame_rate_den = static_cast<uint32_t>(den);
        }
        // If not present or invalid, SanitizePreset() will reset to 60/1.
    }

    // --- Audio ---
    auto& aud = preset.config.audio;
    {
        const auto k = CaptureTargetKindFromString(settings.value(QStringLiteral("aud_target_kind")).toString());
        if (k.has_value())
            aud.target_kind = *k;
    }
    {
        const auto m = MicChannelModeFromString(settings.value(QStringLiteral("aud_mic_channel_mode")).toString());
        if (m.has_value())
            aud.mic_channel_mode = *m;
    }
    {
        const QString mic_dev = settings.value(QStringLiteral("aud_selected_mic_device_id")).toString().trimmed();
        if (mic_dev.isEmpty()) {
            aud.selected_mic_device_id = std::nullopt;
        } else {
            aud.selected_mic_device_id = mic_dev.toStdString();
        }
    }
    {
        // Stored as double for lossless round-trip.
        bool ok = false;
        const double gain = settings.value(QStringLiteral("aud_mic_gain_linear"), 1.0).toDouble(&ok);
        aud.mic_gain_linear = ok ? static_cast<float>(gain) : 1.0f;
    }
    {
        const bool has_pid = settings.value(QStringLiteral("aud_has_window_pid"), false).toBool();
        if (has_pid) {
            bool ok = false;
            const uint pid = settings.value(QStringLiteral("aud_window_pid"), 0u).toUInt(&ok);
            if (ok && pid != 0) {
                aud.selected_window_pid = static_cast<uint32_t>(pid);
            } else {
                aud.selected_window_pid = std::nullopt;
            }
        } else {
            aud.selected_window_pid = std::nullopt;
        }
    }
    {
        const int row_count = settings.value(QStringLiteral("aud_row_count"), 0).toInt();
        aud.source_rows.clear();
        for (int i = 0; i < row_count; ++i) {
            const auto kind =
                AudioSourceKindFromString(settings.value(QStringLiteral("aud_row_%1_kind").arg(i)).toString());
            if (!kind.has_value())
                continue;
            recorder_core::AudioSourceRow row;
            row.kind = *kind;
            row.enabled = settings.value(QStringLiteral("aud_row_%1_enabled").arg(i), true).toBool();
            row.merge_with_above = settings.value(QStringLiteral("aud_row_%1_merge").arg(i), false).toBool();
            aud.source_rows.push_back(row);
        }
    }
    // Audio encoding params (ADR 0019).
    {
        bool ok = false;
        const int bk = settings.value(QStringLiteral("aud_audio_bitrate_kbps"), 160).toInt(&ok);
        aud.audio_bitrate_kbps = (ok && bk >= 0) ? static_cast<uint32_t>(bk) : 160u;
        // SanitizePresetConfig() clamps to valid codec-specific ranges.
    }
    {
        const auto fd =
            OpusFrameDurationFromString(settings.value(QStringLiteral("aud_opus_frame_duration")).toString());
        aud.opus_frame_duration = fd.value_or(recorder_core::OpusFrameDuration::Ms20);
    }
    {
        bool ok = false;
        const int cplx = settings.value(QStringLiteral("aud_opus_complexity"), 10).toInt(&ok);
        aud.opus_complexity = (ok && cplx >= 0 && cplx <= 10) ? cplx : 10;
    }

    // --- Webcam ---
    auto& wc = preset.config.webcam;
    wc.enabled = settings.value(QStringLiteral("wc_enabled"), false).toBool();
    wc.device_id = settings.value(QStringLiteral("wc_device_id")).toString().toStdString();
    wc.width = settings.value(QStringLiteral("wc_width"), 1280).toInt();
    wc.height = settings.value(QStringLiteral("wc_height"), 720).toInt();
    wc.fps = settings.value(QStringLiteral("wc_fps"), 30).toInt();
    wc.overlay.x_norm = static_cast<float>(settings.value(QStringLiteral("wc_overlay_x"), 0.0).toDouble());
    wc.overlay.y_norm = static_cast<float>(settings.value(QStringLiteral("wc_overlay_y"), 0.0).toDouble());
    wc.overlay.w_norm = static_cast<float>(settings.value(QStringLiteral("wc_overlay_w"), 0.25).toDouble());
    wc.overlay.h_norm = static_cast<float>(settings.value(QStringLiteral("wc_overlay_h"), 0.25).toDouble());
    wc.overlay_user_placed = settings.value(QStringLiteral("wc_overlay_user_placed"), false).toBool();
    wc.aspect_ratio_locked = settings.value(QStringLiteral("wc_aspect_ratio_locked"), true).toBool();
    wc.mirror = settings.value(QStringLiteral("wc_mirror"), false).toBool();
    wc.chroma_key.enabled = settings.value(QStringLiteral("wc_chroma_enabled"), false).toBool();
    {
        const auto m =
            WebcamChromaKeyColorModeFromString(settings.value(QStringLiteral("wc_chroma_color_mode")).toString());
        if (m.has_value())
            wc.chroma_key.color_mode = *m;
    }
    wc.chroma_key.custom_r = static_cast<uint8_t>(settings.value(QStringLiteral("wc_chroma_custom_r"), 0).toInt());
    wc.chroma_key.custom_g = static_cast<uint8_t>(settings.value(QStringLiteral("wc_chroma_custom_g"), 255).toInt());
    wc.chroma_key.custom_b = static_cast<uint8_t>(settings.value(QStringLiteral("wc_chroma_custom_b"), 0).toInt());
    wc.chroma_key.tolerance =
        static_cast<float>(settings.value(QStringLiteral("wc_chroma_tolerance"), 0.40).toDouble());
    wc.chroma_key.softness = static_cast<float>(settings.value(QStringLiteral("wc_chroma_softness"), 0.15).toDouble());
    wc.chroma_key.spill_reduction =
        static_cast<float>(settings.value(QStringLiteral("wc_chroma_spill"), 0.30).toDouble());

    // --- Countdown ---
    preset.config.countdown_seconds = settings.value(QStringLiteral("countdown_seconds"), 0).toInt();

    return preset;
}

// ---------------------------------------------------------------------------
// Seed helper: produce the standard reset state.
// ---------------------------------------------------------------------------
PersistedPresetState MakeResetState() {
    PersistedPresetState state;
    state.presets.push_back(MakeDefaultPreset());
    state.selected_id = std::string(kDefaultPresetId);
    state.default_id = std::string(kDefaultPresetId);
    state.was_reset = true;
    return state;
}

} // namespace

// ---------------------------------------------------------------------------
// RecordingPresetStore — constructors
// ---------------------------------------------------------------------------

RecordingPresetStore::RecordingPresetStore() {
    const QString config_dir = settings::ResolveAppConfigDir();
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        file_path_ = QDir(config_dir).filePath(QStringLiteral("presets.ini"));
    } else {
        file_path_ = QStringLiteral("presets.ini");
    }
}

RecordingPresetStore::RecordingPresetStore(QString file_path) : file_path_(std::move(file_path)) {
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

PersistedPresetState RecordingPresetStore::Load() const {
    if (file_path_.isEmpty()) {
        return MakeResetState();
    }

    // If file doesn't exist, return reset state.
    if (!QFileInfo::exists(file_path_)) {
        return MakeResetState();
    }

    QSettings settings(file_path_, QSettings::IniFormat);

    // Version check.
    bool version_ok = false;
    const int schema_version = settings.value(QStringLiteral("schemaVersion"), -1).toInt(&version_ok);
    if (!version_ok || schema_version != kPresetSchemaVersion) {
        return MakeResetState();
    }

    const int count = settings.beginReadArray(QStringLiteral("items"));

    std::vector<RecordingPreset> accepted;
    std::set<std::string> seen_ids;

    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const auto maybe = LoadPresetItem(settings);
        if (!maybe.has_value()) {
            continue; // Malformed item — skip.
        }
        const RecordingPreset& raw = *maybe;
        if (raw.id.empty()) {
            continue;
        }
        if (seen_ids.count(raw.id) > 0) {
            continue; // Duplicate id — drop later occurrence.
        }
        seen_ids.insert(raw.id);
        accepted.push_back(SanitizePreset(raw));
    }

    settings.endArray();

    // No valid items → reset.
    if (accepted.empty()) {
        return MakeResetState();
    }

    const auto id_in_list = [&](const std::string& id) {
        for (const auto& p : accepted)
            if (p.id == id)
                return true;
        return false;
    };

    // Repair selected_id.
    std::string selected_id = settings.value(QStringLiteral("selectedId")).toString().toStdString();
    std::string default_id = settings.value(QStringLiteral("defaultId")).toString().toStdString();

    if (!id_in_list(selected_id)) {
        if (id_in_list(default_id)) {
            selected_id = default_id;
        } else {
            selected_id = accepted.front().id;
        }
    }

    // Repair default_id.
    if (!id_in_list(default_id)) {
        // Prefer kDefaultPresetId if present.
        const std::string canonical(kDefaultPresetId);
        if (id_in_list(canonical)) {
            default_id = canonical;
        } else {
            default_id = accepted.front().id;
        }
    }

    PersistedPresetState state;
    state.presets = std::move(accepted);
    state.selected_id = std::move(selected_id);
    state.default_id = std::move(default_id);
    state.was_reset = false;
    return state;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

void RecordingPresetStore::Save(const std::vector<RecordingPreset>& presets, const std::string& selected_id,
                                const std::string& default_id) const {
    if (file_path_.isEmpty()) {
        return;
    }

    const QFileInfo info(file_path_);
    QDir().mkpath(info.absolutePath());

    QSettings settings(file_path_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
    settings.setValue(QStringLiteral("selectedId"), QString::fromStdString(selected_id));
    settings.setValue(QStringLiteral("defaultId"), QString::fromStdString(default_id));

    // beginWriteArray clears any prior array entries — deleted presets leave no
    // stale keys.
    settings.beginWriteArray(QStringLiteral("items"), static_cast<int>(presets.size()));
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        settings.setArrayIndex(i);
        SavePresetItem(settings, presets[static_cast<std::size_t>(i)]);
    }
    settings.endArray();

    settings.sync(); // QSettings atomic write (temp + rename).
}

// ---------------------------------------------------------------------------
// FilePath
// ---------------------------------------------------------------------------

const QString& RecordingPresetStore::FilePath() const {
    return file_path_;
}

// ---------------------------------------------------------------------------
// ExportPresetToFile
// ---------------------------------------------------------------------------

bool RecordingPresetStore::ExportPresetToFile(const RecordingPreset& preset, const QString& path, QString* err) {
    if (path.isEmpty()) {
        if (err)
            *err = QStringLiteral("Export path is empty.");
        return false;
    }

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (err)
            *err = QStringLiteral("Could not create parent directory: %1").arg(info.absolutePath());
        return false;
    }

    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
    // Tag as a single-preset export so ImportPresetsFromFile can detect which
    // layout was used without ambiguity.
    settings.setValue(QStringLiteral("exportKind"), QStringLiteral("single"));

    settings.beginWriteArray(QStringLiteral("items"), 1);
    settings.setArrayIndex(0);
    SavePresetItem(settings, preset);
    settings.endArray();
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (err)
            *err = QStringLiteral("Failed to write preset file: %1").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ExportAllUserPresetsToFile
// ---------------------------------------------------------------------------

bool RecordingPresetStore::ExportAllUserPresetsToFile(const QVector<RecordingPreset>& presets, const QString& path,
                                                      QString* err) {
    if (path.isEmpty()) {
        if (err)
            *err = QStringLiteral("Export path is empty.");
        return false;
    }

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (err)
            *err = QStringLiteral("Could not create parent directory: %1").arg(info.absolutePath());
        return false;
    }

    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("schemaVersion"), kPresetSchemaVersion);
    settings.setValue(QStringLiteral("exportKind"), QStringLiteral("all"));

    settings.beginWriteArray(QStringLiteral("items"), static_cast<int>(presets.size()));
    for (int i = 0; i < presets.size(); ++i) {
        settings.setArrayIndex(i);
        SavePresetItem(settings, presets[i]);
    }
    settings.endArray();
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (err)
            *err = QStringLiteral("Failed to write preset file: %1").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ImportPresetsFromFile
// ---------------------------------------------------------------------------

QVector<RecordingPreset> RecordingPresetStore::ImportPresetsFromFile(const QString& path,
                                                                     const std::vector<std::string>& existing_ids,
                                                                     QString* err) {
    if (path.isEmpty()) {
        if (err)
            *err = QStringLiteral("Import path is empty.");
        return {};
    }

    if (!QFileInfo::exists(path)) {
        if (err)
            *err = QStringLiteral("File not found: %1").arg(path);
        return {};
    }

    QSettings settings(path, QSettings::IniFormat);

    if (settings.status() != QSettings::NoError) {
        if (err)
            *err = QStringLiteral("Could not open file for reading: %1").arg(path);
        return {};
    }

    // Schema version check: best-effort — log a warning but still attempt to
    // parse.  Pre-1.0: no migration; newer files are parsed as-is + sanitized.
    bool version_ok = false;
    const int file_version = settings.value(QStringLiteral("schemaVersion"), -1).toInt(&version_ok);
    const bool version_mismatch = !version_ok || file_version != kPresetSchemaVersion;

    const int count = settings.beginReadArray(QStringLiteral("items"));

    if (count <= 0) {
        settings.endArray();
        if (err) {
            if (version_mismatch) {
                *err = QStringLiteral("Preset file has an unsupported schema version (%1, expected %2). No items could "
                                      "be imported.")
                           .arg(file_version)
                           .arg(kPresetSchemaVersion);
            } else {
                *err = QStringLiteral("Preset file contains no items.");
            }
        }
        return {};
    }

    // Build a fast-lookup set of existing ids (plus ids we have already
    // assigned to earlier items in this import batch).
    std::set<std::string> used_ids(existing_ids.begin(), existing_ids.end());

    QVector<RecordingPreset> result;
    result.reserve(count);

    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const auto maybe = LoadPresetItem(settings);
        if (!maybe.has_value())
            continue; // Malformed — skip.

        RecordingPreset preset = SanitizePreset(*maybe);

        // Collision handling: if the id is already used, generate a fresh one
        // and suffix the name so the user can tell it apart.
        if (used_ids.count(preset.id) > 0) {
            preset.id = GeneratePresetId();
            if (!preset.name.empty() && preset.name.rfind(" (imported)") == std::string::npos) {
                preset.name += " (imported)";
            }
        }
        used_ids.insert(preset.id);
        result.push_back(std::move(preset));
    }

    settings.endArray();

    if (result.isEmpty()) {
        if (err) {
            if (version_mismatch) {
                *err = QStringLiteral("Preset file has an unsupported schema version (%1, expected %2). No valid items "
                                      "could be imported.")
                           .arg(file_version)
                           .arg(kPresetSchemaVersion);
            } else {
                *err = QStringLiteral("No valid presets found in file: %1").arg(path);
            }
        }
        return {};
    }

    return result;
}

} // namespace exosnap
