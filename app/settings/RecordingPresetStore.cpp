#include "RecordingPresetStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringView>
#include <QTextStream>
#include <QVector>

#include "settings/ConfigPaths.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <sstream>

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

QString VideoBitDepthToString(capability::BitDepth v) {
    switch (v) {
    case capability::BitDepth::Bit8:
        return QStringLiteral("8");
    case capability::BitDepth::Bit10:
        return QStringLiteral("10");
    }
    return QStringLiteral("8");
}

std::optional<capability::BitDepth> VideoBitDepthFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("8"))
        return capability::BitDepth::Bit8;
    if (n == QStringLiteral("10"))
        return capability::BitDepth::Bit10;
    return std::nullopt;
}

QString ChromaSubsamplingToString(capability::ChromaSubsampling v) {
    switch (v) {
    case capability::ChromaSubsampling::Cs420:
        return QStringLiteral("420");
    case capability::ChromaSubsampling::Cs422:
        return QStringLiteral("422");
    case capability::ChromaSubsampling::Cs444:
        return QStringLiteral("444");
    }
    return QStringLiteral("420");
}

std::optional<capability::ChromaSubsampling> ChromaSubsamplingFromString(QStringView s) {
    const QString n = s.trimmed().toString();
    if (n == QStringLiteral("420"))
        return capability::ChromaSubsampling::Cs420;
    if (n == QStringLiteral("422"))
        return capability::ChromaSubsampling::Cs422;
    if (n == QStringLiteral("444"))
        return capability::ChromaSubsampling::Cs444;
    return std::nullopt;
}

QString ColorRangeToString(capability::ColorRange v) {
    switch (v) {
    case capability::ColorRange::Full:
        return QStringLiteral("full");
    case capability::ColorRange::Limited:
        return QStringLiteral("limited");
    }
    return QStringLiteral("full");
}

std::optional<capability::ColorRange> ColorRangeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("full"))
        return capability::ColorRange::Full;
    if (n == QStringLiteral("limited"))
        return capability::ColorRange::Limited;
    return std::nullopt;
}

QString AudioCodecToString(capability::AudioCodec v) {
    switch (v) {
    case capability::AudioCodec::Aac:
        return QStringLiteral("aac");
    case capability::AudioCodec::Opus:
        return QStringLiteral("opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("pcm");
    case capability::AudioCodec::Flac:
        return QStringLiteral("flac");
    }
    return QStringLiteral("aac");
}

std::optional<capability::AudioCodec> AudioCodecFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("opus"))
        return capability::AudioCodec::Opus;
    if (n == QStringLiteral("aac"))
        return capability::AudioCodec::Aac;
    if (n == QStringLiteral("pcm"))
        return capability::AudioCodec::Pcm;
    if (n == QStringLiteral("flac"))
        return capability::AudioCodec::Flac;
    return std::nullopt;
}

QString NvencPresetToString(recorder_core::NvencPreset v) {
    switch (v) {
    case recorder_core::NvencPreset::P1:
        return QStringLiteral("p1");
    case recorder_core::NvencPreset::P2:
        return QStringLiteral("p2");
    case recorder_core::NvencPreset::P3:
        return QStringLiteral("p3");
    case recorder_core::NvencPreset::P4:
        return QStringLiteral("p4");
    case recorder_core::NvencPreset::P5:
        return QStringLiteral("p5");
    case recorder_core::NvencPreset::P6:
        return QStringLiteral("p6");
    case recorder_core::NvencPreset::P7:
        return QStringLiteral("p7");
    }
    return QStringLiteral("p4");
}

std::optional<recorder_core::NvencPreset> NvencPresetFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("p1"))
        return recorder_core::NvencPreset::P1;
    if (n == QStringLiteral("p2"))
        return recorder_core::NvencPreset::P2;
    if (n == QStringLiteral("p3"))
        return recorder_core::NvencPreset::P3;
    if (n == QStringLiteral("p4"))
        return recorder_core::NvencPreset::P4;
    if (n == QStringLiteral("p5"))
        return recorder_core::NvencPreset::P5;
    if (n == QStringLiteral("p6"))
        return recorder_core::NvencPreset::P6;
    if (n == QStringLiteral("p7"))
        return recorder_core::NvencPreset::P7;
    return std::nullopt;
}

QString HdrModeToString(recorder_core::HdrMode v) {
    switch (v) {
    case recorder_core::HdrMode::Off:
        return QStringLiteral("off");
    case recorder_core::HdrMode::TonemapSdr:
        return QStringLiteral("tonemap_sdr");
    case recorder_core::HdrMode::Hdr10:
        return QStringLiteral("hdr10");
    }
    return QStringLiteral("tonemap_sdr");
}

std::optional<recorder_core::HdrMode> HdrModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("off"))
        return recorder_core::HdrMode::Off;
    if (n == QStringLiteral("tonemap_sdr"))
        return recorder_core::HdrMode::TonemapSdr;
    if (n == QStringLiteral("hdr10"))
        return recorder_core::HdrMode::Hdr10;
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

QString SplitSizeModeToString(SplitSizeMode v) {
    switch (v) {
    case SplitSizeMode::Off:
        return QStringLiteral("off");
    case SplitSizeMode::Custom:
        return QStringLiteral("custom");
    }
    return QStringLiteral("off");
}

std::optional<SplitSizeMode> SplitSizeModeFromString(QStringView s) {
    const QString n = s.trimmed().toString().toLower();
    if (n == QStringLiteral("off"))
        return SplitSizeMode::Off;
    if (n == QStringLiteral("custom"))
        return SplitSizeMode::Custom;
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
// TOML helpers — safe node readers with defaults
// ---------------------------------------------------------------------------

// Read a string from a toml node, returning default_val when missing or
// wrong type.
std::string TomlStr(const toml::node_view<const toml::node>& node, std::string_view default_val = "") {
    if (const auto* s = node.as_string())
        return **s;
    return std::string(default_val);
}

bool TomlBool(const toml::node_view<const toml::node>& node, bool default_val = false) {
    if (const auto* b = node.as_boolean())
        return **b;
    return default_val;
}

int64_t TomlInt(const toml::node_view<const toml::node>& node, int64_t default_val = 0) {
    if (const auto* i = node.as_integer())
        return **i;
    return default_val;
}

double TomlFloat(const toml::node_view<const toml::node>& node, double default_val = 0.0) {
    if (const auto* f = node.as_floating_point())
        return **f;
    // Also accept integer nodes (no decimal in TOML) — convert silently.
    if (const auto* i = node.as_integer())
        return static_cast<double>(**i);
    return default_val;
}

// ---------------------------------------------------------------------------
// Per-item TOML serialization helpers
// ---------------------------------------------------------------------------

// A StableDisplayId round-trips as its own sub-table. A missing sub-table (older
// pre-v25 file) simply yields an empty() identity — "no stored preference".
toml::table StableDisplayIdToToml(const StableDisplayId& id) {
    toml::table t;
    t.emplace("device_path", id.device_path);
    t.emplace("edid_vendor", id.edid_vendor);
    t.emplace("edid_product", static_cast<int64_t>(id.edid_product));
    t.emplace("serial", id.serial);
    t.emplace("friendly_name", id.friendly_name);
    t.emplace("gdi_name", id.gdi_name);
    t.emplace("seq_hint", static_cast<int64_t>(id.seq_hint));
    return t;
}

StableDisplayId StableDisplayIdFromToml(const toml::node_view<const toml::node>& node) {
    StableDisplayId id;
    id.device_path = TomlStr(node["device_path"]);
    id.edid_vendor = TomlStr(node["edid_vendor"]);
    id.edid_product = static_cast<uint32_t>(TomlInt(node["edid_product"], 0));
    id.serial = TomlStr(node["serial"]);
    id.friendly_name = TomlStr(node["friendly_name"]);
    id.gdi_name = TomlStr(node["gdi_name"]);
    id.seq_hint = static_cast<int>(TomlInt(node["seq_hint"], 0));
    return id;
}

// Serializes the config body shared by every stored preset AND the [live]
// table — capture/output/video/audio/webcam/countdown. No id/name here: those
// are preset-only identity, not part of the config.
toml::table ConfigToToml(const RecordingPresetConfig& config) {
    toml::table tbl;

    // --- Capture ---
    const auto& cap = config.capture;
    toml::table cap_tbl;
    cap_tbl.emplace("kind", PresetCaptureKindToString(cap.kind).toStdString());
    cap_tbl.emplace("display_id", StableDisplayIdToToml(cap.display_id));
    cap_tbl.emplace("window_key", cap.window_key);
    cap_tbl.emplace("has_region", cap.has_region);
    cap_tbl.emplace("region_display_id", StableDisplayIdToToml(cap.region_display_id));
    cap_tbl.emplace("region_x_norm", static_cast<double>(cap.region_x_norm));
    cap_tbl.emplace("region_y_norm", static_cast<double>(cap.region_y_norm));
    cap_tbl.emplace("region_w_norm", static_cast<double>(cap.region_w_norm));
    cap_tbl.emplace("region_h_norm", static_cast<double>(cap.region_h_norm));
    tbl.emplace("capture", std::move(cap_tbl));

    // --- Output ---
    const auto& out = config.output;
    toml::table out_tbl;
    out_tbl.emplace("folder", QString::fromStdWString(out.output_folder.wstring()).toStdString());
    out_tbl.emplace("naming_pattern", QString::fromStdWString(out.naming_pattern).toStdString());
    out_tbl.emplace("container", ContainerToString(out.container).toStdString());
    out_tbl.emplace("video_codec", VideoCodecToString(out.video_codec).toStdString());
    out_tbl.emplace("bit_depth", VideoBitDepthToString(out.bit_depth).toStdString());
    out_tbl.emplace("chroma_subsampling", ChromaSubsamplingToString(out.chroma_subsampling).toStdString());
    out_tbl.emplace("color_range", ColorRangeToString(out.color_range).toStdString());
    out_tbl.emplace("nvenc_preset", NvencPresetToString(out.nvenc_preset).toStdString());
    out_tbl.emplace("hdr_mode", HdrModeToString(out.hdr_mode).toStdString());
    out_tbl.emplace("audio_codec", AudioCodecToString(out.audio_codec).toStdString());
    out_tbl.emplace("resolution_mode", OutputResolutionModeToString(out.resolution.mode).toStdString());
    out_tbl.emplace("custom_width", static_cast<int64_t>(out.resolution.custom_width));
    out_tbl.emplace("custom_height", static_cast<int64_t>(out.resolution.custom_height));
    out_tbl.emplace("fit_mode", OutputFitModeToString(out.resolution.fit).toStdString());
    out_tbl.emplace("split_mode", SplitRecordingModeToString(out.split.mode).toStdString());
    out_tbl.emplace("split_custom_minutes", static_cast<int64_t>(out.split.custom_minutes));
    out_tbl.emplace("split_size_mode", SplitSizeModeToString(out.split.size_mode).toStdString());
    out_tbl.emplace("split_custom_size_mb", static_cast<int64_t>(out.split.custom_size_mb));
    tbl.emplace("output", std::move(out_tbl));

    // --- Video ---
    const auto& vid = config.video;
    toml::table vid_tbl;
    vid_tbl.emplace("cq", static_cast<int64_t>(vid.cq));
    vid_tbl.emplace("rate_control", RateControlModeToString(vid.rate_control).toStdString());
    vid_tbl.emplace("bitrate_kbps", static_cast<int64_t>(vid.bitrate_kbps));
    vid_tbl.emplace("cfr", vid.cfr);
    vid_tbl.emplace("frame_pacing", static_cast<int64_t>(static_cast<int>(vid.frame_pacing)));
    vid_tbl.emplace("capture_cursor", vid.capture_cursor);
    vid_tbl.emplace("frame_rate_num", static_cast<int64_t>(vid.frame_rate_num));
    vid_tbl.emplace("frame_rate_den", static_cast<int64_t>(vid.frame_rate_den));
    tbl.emplace("video", std::move(vid_tbl));

    // --- Audio ---
    const auto& aud = config.audio;
    toml::table aud_tbl;
    aud_tbl.emplace("target_kind", CaptureTargetKindToString(aud.target_kind).toStdString());
    aud_tbl.emplace("mic_channel_mode", MicChannelModeToString(aud.mic_channel_mode).toStdString());
    aud_tbl.emplace("selected_mic_device_id",
                    aud.selected_mic_device_id.has_value() ? *aud.selected_mic_device_id : std::string());
    aud_tbl.emplace("mic_gain_linear", static_cast<double>(aud.mic_gain_linear));
    aud_tbl.emplace("has_window_pid", aud.selected_window_pid.has_value());
    aud_tbl.emplace("window_pid",
                    static_cast<int64_t>(aud.selected_window_pid.has_value() ? aud.selected_window_pid.value() : 0u));
    aud_tbl.emplace("audio_bitrate_kbps", static_cast<int64_t>(aud.audio_bitrate_kbps));
    aud_tbl.emplace("opus_frame_duration", OpusFrameDurationToString(aud.opus_frame_duration).toStdString());
    aud_tbl.emplace("opus_complexity", static_cast<int64_t>(aud.opus_complexity));
    // Brickwall limiter (Audio v2 — 0.6.0).
    aud_tbl.emplace("limiter_enabled", aud.limiter_enabled);
    aud_tbl.emplace("limiter_ceiling_db", static_cast<double>(aud.limiter_ceiling_db));
    // A/V clock slaving (H-3).
    aud_tbl.emplace("clock_slaving_enabled", aud.clock_slaving_enabled);
    // Microphone high-pass filter (Audio v2 — 0.6.0).
    aud_tbl.emplace("mic_hpf_enabled", aud.mic_hpf_enabled);
    aud_tbl.emplace("mic_hpf_cutoff_hz", static_cast<double>(aud.mic_hpf_cutoff_hz));
    // Microphone noise gate (Audio v2 — 0.6.0).
    aud_tbl.emplace("mic_gate_enabled", aud.mic_gate_enabled);
    aud_tbl.emplace("mic_gate_threshold_db", static_cast<double>(aud.mic_gate_threshold_db));
    // Microphone automatic gain control (Audio v2 — 0.6.0).
    aud_tbl.emplace("mic_agc_enabled", aud.mic_agc_enabled);
    aud_tbl.emplace("mic_agc_target_db", static_cast<double>(aud.mic_agc_target_db));
    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0). Bool only.
    aud_tbl.emplace("mic_rnnoise_enabled", aud.mic_rnnoise_enabled);
    // Channel / sample-format model (ADR 0030 -- 0.6.0).
    aud_tbl.emplace("audio_sample_rate", static_cast<int64_t>(aud.audio_sample_rate));
    aud_tbl.emplace("audio_channels", static_cast<int64_t>(aud.audio_channels));
    aud_tbl.emplace("audio_bit_depth", static_cast<int64_t>(aud.audio_bit_depth));
    aud_tbl.emplace("pcm_float", aud.audio_pcm_float);
    aud_tbl.emplace("flac_compression_level", static_cast<int64_t>(aud.flac_compression_level));

    // audio sources as array-of-tables
    toml::array sources_arr;
    for (const auto& row : aud.source_rows) {
        toml::table row_tbl;
        row_tbl.emplace("kind", AudioSourceKindToString(row.kind).toStdString());
        row_tbl.emplace("enabled", row.enabled);
        row_tbl.emplace("merge", row.merge_with_above);
        // Audio v2 (0.6.0): per-row gain + mute.
        row_tbl.emplace("gain_db", static_cast<double>(row.gain_db));
        row_tbl.emplace("muted", row.muted);
        sources_arr.push_back(std::move(row_tbl));
    }
    aud_tbl.emplace("sources", std::move(sources_arr));
    tbl.emplace("audio", std::move(aud_tbl));

    // --- Webcam ---
    const auto& wc = config.webcam;
    toml::table wc_tbl;
    wc_tbl.emplace("enabled", wc.enabled);
    wc_tbl.emplace("device_id", wc.device_id);
    wc_tbl.emplace("width", static_cast<int64_t>(wc.width));
    wc_tbl.emplace("height", static_cast<int64_t>(wc.height));
    wc_tbl.emplace("fps", static_cast<int64_t>(wc.fps));
    wc_tbl.emplace("overlay_x", static_cast<double>(wc.overlay.x_norm));
    wc_tbl.emplace("overlay_y", static_cast<double>(wc.overlay.y_norm));
    wc_tbl.emplace("overlay_w", static_cast<double>(wc.overlay.w_norm));
    wc_tbl.emplace("overlay_h", static_cast<double>(wc.overlay.h_norm));
    wc_tbl.emplace("overlay_user_placed", wc.overlay_user_placed);
    wc_tbl.emplace("aspect_ratio_locked", wc.aspect_ratio_locked);
    wc_tbl.emplace("mirror", wc.mirror);
    wc_tbl.emplace("opacity", static_cast<double>(wc.opacity));

    toml::table ck_tbl;
    ck_tbl.emplace("enabled", wc.chroma_key.enabled);
    ck_tbl.emplace("color_mode", WebcamChromaKeyColorModeToString(wc.chroma_key.color_mode).toStdString());
    ck_tbl.emplace("custom_r", static_cast<int64_t>(wc.chroma_key.custom_r));
    ck_tbl.emplace("custom_g", static_cast<int64_t>(wc.chroma_key.custom_g));
    ck_tbl.emplace("custom_b", static_cast<int64_t>(wc.chroma_key.custom_b));
    ck_tbl.emplace("tolerance", static_cast<double>(wc.chroma_key.tolerance));
    ck_tbl.emplace("softness", static_cast<double>(wc.chroma_key.softness));
    ck_tbl.emplace("spill", static_cast<double>(wc.chroma_key.spill_reduction));
    wc_tbl.emplace("chroma_key", std::move(ck_tbl));

    tbl.emplace("webcam", std::move(wc_tbl));

    // --- Countdown ---
    tbl.emplace("countdown_seconds", static_cast<int64_t>(config.countdown_seconds));

    return tbl;
}

// A stored preset is just an id + name wrapped around a config body.
toml::table PresetToToml(const RecordingPreset& preset) {
    toml::table tbl = ConfigToToml(preset.config);
    tbl.emplace("id", preset.id);
    tbl.emplace("name", preset.name);
    return tbl;
}

// Parses the config body shared by every stored preset AND the [live] table.
// Field-wise: a missing or wrong-typed key leaves that field at its model
// default instead of failing the whole parse.
RecordingPresetConfig ConfigFromToml(const toml::table& tbl) {
    RecordingPresetConfig config;

    // --- Capture ---
    auto& cap = config.capture;
    {
        const auto kind = PresetCaptureKindFromString(QString::fromStdString(TomlStr(tbl["capture"]["kind"])));
        cap.kind = kind.value_or(PresetCaptureKind::Display);
    }
    cap.display_id = StableDisplayIdFromToml(tbl["capture"]["display_id"]);
    cap.window_key = TomlStr(tbl["capture"]["window_key"]);
    cap.has_region = TomlBool(tbl["capture"]["has_region"], false);
    cap.region_display_id = StableDisplayIdFromToml(tbl["capture"]["region_display_id"]);
    cap.region_x_norm = static_cast<float>(TomlFloat(tbl["capture"]["region_x_norm"], 0.0));
    cap.region_y_norm = static_cast<float>(TomlFloat(tbl["capture"]["region_y_norm"], 0.0));
    cap.region_w_norm = static_cast<float>(TomlFloat(tbl["capture"]["region_w_norm"], 0.0));
    cap.region_h_norm = static_cast<float>(TomlFloat(tbl["capture"]["region_h_norm"], 0.0));

    // --- Output ---
    auto& out = config.output;
    {
        const std::string folder = TomlStr(tbl["output"]["folder"]);
        if (!folder.empty()) {
            out.output_folder = std::filesystem::path(QString::fromStdString(folder).toStdWString());
        }
    }
    {
        const std::string pat = TomlStr(tbl["output"]["naming_pattern"]);
        if (!pat.empty()) {
            out.naming_pattern = QString::fromStdString(pat).toStdWString();
        }
    }
    {
        const auto c = ContainerFromString(QString::fromStdString(TomlStr(tbl["output"]["container"])));
        if (c.has_value())
            out.container = *c;
    }
    {
        const auto c = VideoCodecFromString(QString::fromStdString(TomlStr(tbl["output"]["video_codec"])));
        if (c.has_value())
            out.video_codec = *c;
    }
    {
        const auto bd = VideoBitDepthFromString(QString::fromStdString(TomlStr(tbl["output"]["bit_depth"])));
        if (bd.has_value())
            out.bit_depth = *bd;
    }
    {
        // Additive field — missing key (older preset files) leaves out.chroma_subsampling
        // at its struct default (Cs420). No schema bump needed.
        const auto cs =
            ChromaSubsamplingFromString(QString::fromStdString(TomlStr(tbl["output"]["chroma_subsampling"])));
        if (cs.has_value())
            out.chroma_subsampling = *cs;
    }
    {
        const auto cr = ColorRangeFromString(QString::fromStdString(TomlStr(tbl["output"]["color_range"])));
        if (cr.has_value())
            out.color_range = *cr;
    }
    {
        // NVENC-PRESET-R1: additive field — missing key (older preset files) leaves
        // out.nvenc_preset at its struct default (P4). No schema bump needed.
        const auto np = NvencPresetFromString(QString::fromStdString(TomlStr(tbl["output"]["nvenc_preset"])));
        if (np.has_value())
            out.nvenc_preset = *np;
    }
    {
        // A missing/invalid key (schema-20-and-older files, which reset before
        // reaching here anyway — see kPresetSchemaVersion — or a hand-edited/
        // corrupt value) leaves out.hdr_mode at its struct default (TonemapSdr).
        const auto hm = HdrModeFromString(QString::fromStdString(TomlStr(tbl["output"]["hdr_mode"])));
        if (hm.has_value())
            out.hdr_mode = *hm;
    }
    {
        const auto c = AudioCodecFromString(QString::fromStdString(TomlStr(tbl["output"]["audio_codec"])));
        if (c.has_value())
            out.audio_codec = *c;
    }
    {
        const auto m =
            OutputResolutionModeFromString(QString::fromStdString(TomlStr(tbl["output"]["resolution_mode"])));
        if (m.has_value())
            out.resolution.mode = *m;
    }
    {
        const int64_t w = TomlInt(tbl["output"]["custom_width"], 0);
        const int64_t h = TomlInt(tbl["output"]["custom_height"], 0);
        if (w >= 0 && h >= 0) {
            out.resolution.custom_width = static_cast<uint32_t>(w);
            out.resolution.custom_height = static_cast<uint32_t>(h);
        }
    }
    {
        const auto fit = OutputFitModeFromString(QString::fromStdString(TomlStr(tbl["output"]["fit_mode"])));
        if (fit.has_value())
            out.resolution.fit = *fit;
    }
    {
        const auto sm = SplitRecordingModeFromString(QString::fromStdString(TomlStr(tbl["output"]["split_mode"])));
        if (sm.has_value())
            out.split.mode = *sm;
        const int64_t minutes = TomlInt(tbl["output"]["split_custom_minutes"], 30);
        if (minutes > 0)
            out.split.custom_minutes = static_cast<uint32_t>(minutes);
        const auto ssm = SplitSizeModeFromString(QString::fromStdString(TomlStr(tbl["output"]["split_size_mode"])));
        if (ssm.has_value())
            out.split.size_mode = *ssm;
        const int64_t size_mb = TomlInt(tbl["output"]["split_custom_size_mb"], 2048);
        if (size_mb > 0)
            out.split.custom_size_mb = static_cast<uint32_t>(size_mb);
    }

    // --- Video ---
    auto& vid = config.video;
    {
        const int64_t cq = TomlInt(tbl["video"]["cq"], static_cast<int64_t>(vid.cq));
        if (cq >= recorder_core::kNvencCqMin && cq <= recorder_core::kNvencCqMax)
            vid.cq = static_cast<uint32_t>(cq);
    }
    {
        const auto rc = RateControlModeFromString(QString::fromStdString(TomlStr(tbl["video"]["rate_control"])));
        if (rc.has_value())
            vid.rate_control = *rc;
    }
    {
        const int64_t bk = TomlInt(tbl["video"]["bitrate_kbps"], 20000);
        if (bk > 0)
            vid.bitrate_kbps = static_cast<uint32_t>(bk);
    }
    // cfr / capture_cursor: only override when the key is present.
    if (tbl["video"]["cfr"]) {
        vid.cfr = TomlBool(tbl["video"]["cfr"], true);
    }
    if (tbl["video"]["capture_cursor"]) {
        vid.capture_cursor = TomlBool(tbl["video"]["capture_cursor"], true);
    }
    {
        const int64_t num = TomlInt(tbl["video"]["frame_rate_num"], 60);
        const int64_t den = TomlInt(tbl["video"]["frame_rate_den"], 1);
        if (num > 0 && den > 0) {
            vid.frame_rate_num = static_cast<uint32_t>(num);
            vid.frame_rate_den = static_cast<uint32_t>(den);
        }
    }
    // CFR frame pacing (ADR 0035). Default 0 = Smooth; out-of-range clamped by SanitizePresetConfig.
    vid.frame_pacing = static_cast<recorder_core::FramePacingMode>(TomlInt(tbl["video"]["frame_pacing"], 0));

    // --- Audio ---
    auto& aud = config.audio;
    {
        const auto k = CaptureTargetKindFromString(QString::fromStdString(TomlStr(tbl["audio"]["target_kind"])));
        if (k.has_value())
            aud.target_kind = *k;
    }
    {
        const auto m = MicChannelModeFromString(QString::fromStdString(TomlStr(tbl["audio"]["mic_channel_mode"])));
        if (m.has_value())
            aud.mic_channel_mode = *m;
    }
    {
        const std::string mic_dev =
            QString::fromStdString(TomlStr(tbl["audio"]["selected_mic_device_id"])).trimmed().toStdString();
        if (mic_dev.empty()) {
            aud.selected_mic_device_id = std::nullopt;
        } else {
            aud.selected_mic_device_id = mic_dev;
        }
    }
    {
        const double gain = TomlFloat(tbl["audio"]["mic_gain_linear"], 1.0);
        aud.mic_gain_linear = static_cast<float>(gain);
    }
    {
        const bool has_pid = TomlBool(tbl["audio"]["has_window_pid"], false);
        if (has_pid) {
            const int64_t pid = TomlInt(tbl["audio"]["window_pid"], 0);
            if (pid > 0) {
                aud.selected_window_pid = static_cast<uint32_t>(pid);
            } else {
                aud.selected_window_pid = std::nullopt;
            }
        } else {
            aud.selected_window_pid = std::nullopt;
        }
    }
    // Audio encoding params (ADR 0019).
    {
        const int64_t bk = TomlInt(tbl["audio"]["audio_bitrate_kbps"], 160);
        aud.audio_bitrate_kbps = (bk >= 0) ? static_cast<uint32_t>(bk) : 160u;
    }
    {
        const auto fd =
            OpusFrameDurationFromString(QString::fromStdString(TomlStr(tbl["audio"]["opus_frame_duration"])));
        aud.opus_frame_duration = fd.value_or(recorder_core::OpusFrameDuration::Ms20);
    }
    {
        const int64_t cplx = TomlInt(tbl["audio"]["opus_complexity"], 10);
        aud.opus_complexity = (cplx >= 0 && cplx <= 10) ? static_cast<int>(cplx) : 10;
    }
    // Brickwall limiter (Audio v2 — 0.6.0). Older presets default to enabled /
    // 0.0 dBFS (no behavior change vs the previous hard clip at full scale).
    {
        aud.limiter_enabled = TomlBool(tbl["audio"]["limiter_enabled"], true);
        aud.limiter_ceiling_db = static_cast<float>(TomlFloat(tbl["audio"]["limiter_ceiling_db"], 0.0));
    }
    // A/V clock slaving (H-3). Missing key => default on (sync before bit-exactness);
    // pre-1.0, no migration.
    {
        aud.clock_slaving_enabled = TomlBool(tbl["audio"]["clock_slaving_enabled"], true);
    }
    // Microphone high-pass filter (Audio v2 — 0.6.0). Older presets default to
    // disabled / 80 Hz (no behavior change vs unfiltered mic capture).
    {
        aud.mic_hpf_enabled = TomlBool(tbl["audio"]["mic_hpf_enabled"], false);
        aud.mic_hpf_cutoff_hz = static_cast<float>(TomlFloat(tbl["audio"]["mic_hpf_cutoff_hz"], 80.0));
    }
    // Microphone noise gate (Audio v2 — 0.6.0). Older presets default to disabled
    // / -45 dB (no behavior change vs ungated mic capture).
    {
        aud.mic_gate_enabled = TomlBool(tbl["audio"]["mic_gate_enabled"], false);
        aud.mic_gate_threshold_db = static_cast<float>(TomlFloat(tbl["audio"]["mic_gate_threshold_db"], -45.0));
    }
    // Microphone automatic gain control (Audio v2 — 0.6.0). Older presets default
    // to disabled / -18 dB (no behavior change vs un-AGC'd mic capture).
    {
        aud.mic_agc_enabled = TomlBool(tbl["audio"]["mic_agc_enabled"], false);
        aud.mic_agc_target_db = static_cast<float>(TomlFloat(tbl["audio"]["mic_agc_target_db"], -18.0));
    }
    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0). Older
    // presets default to disabled (no behavior change vs unsuppressed capture).
    {
        aud.mic_rnnoise_enabled = TomlBool(tbl["audio"]["mic_rnnoise_enabled"], false);
    }
    // Channel / sample-format model (ADR 0030 -- 0.6.0). Older presets default to
    // 48000 Hz / stereo / 16-bit / level 5 (no behavior change vs previous fixed values).
    {
        const int64_t sr = TomlInt(tbl["audio"]["audio_sample_rate"], 48000);
        aud.audio_sample_rate = (sr == 44100 || sr == 48000 || sr == 96000) ? static_cast<uint32_t>(sr) : 48000u;

        const int64_t ch = TomlInt(tbl["audio"]["audio_channels"], 2);
        aud.audio_channels = (ch == 1 || ch == 2) ? static_cast<uint32_t>(ch) : 2u;

        const int64_t bd = TomlInt(tbl["audio"]["audio_bit_depth"], 16);
        aud.audio_bit_depth = (bd == 16 || bd == 24 || bd == 32) ? static_cast<uint32_t>(bd) : 16u;

        // Float-PCM: older presets (schema < 24) have no "pcm_float" key and
        // default to false (no behavior change). Cross-field consistency
        // (Pcm-only, requires bit_depth==32) is repaired by
        // RecordingPreset.cpp's SanitizePresetConfig, not here.
        aud.audio_pcm_float = TomlBool(tbl["audio"]["pcm_float"], false);

        const int64_t fl = TomlInt(tbl["audio"]["flac_compression_level"], 5);
        aud.flac_compression_level = (fl >= 0 && fl <= 8) ? static_cast<int>(fl) : 5;
    }
    // Audio source rows — array-of-tables [[audio.sources]]
    {
        aud.source_rows.clear();
        if (const auto* sources_node = tbl["audio"]["sources"].as_array()) {
            for (const auto& elem : *sources_node) {
                if (const auto* row_tbl = elem.as_table()) {
                    const auto kind = AudioSourceKindFromString(QString::fromStdString(TomlStr((*row_tbl)["kind"])));
                    if (!kind.has_value())
                        continue;
                    recorder_core::AudioSourceRow row;
                    row.kind = *kind;
                    row.enabled = TomlBool((*row_tbl)["enabled"], true);
                    row.merge_with_above = TomlBool((*row_tbl)["merge"], false);
                    // Audio v2 (0.6.0): per-row gain + mute. Older presets that
                    // lack these keys default to 0 dB / not muted (no behavior change).
                    row.gain_db = static_cast<float>(TomlFloat((*row_tbl)["gain_db"], 0.0));
                    row.muted = TomlBool((*row_tbl)["muted"], false);
                    aud.source_rows.push_back(row);
                }
            }
        }
    }

    // --- Webcam ---
    auto& wc = config.webcam;
    wc.enabled = TomlBool(tbl["webcam"]["enabled"], false);
    wc.device_id = TomlStr(tbl["webcam"]["device_id"]);
    wc.width = static_cast<int>(TomlInt(tbl["webcam"]["width"], 1280));
    wc.height = static_cast<int>(TomlInt(tbl["webcam"]["height"], 720));
    wc.fps = static_cast<int>(TomlInt(tbl["webcam"]["fps"], 30));
    wc.overlay.x_norm = static_cast<float>(TomlFloat(tbl["webcam"]["overlay_x"], 0.0));
    wc.overlay.y_norm = static_cast<float>(TomlFloat(tbl["webcam"]["overlay_y"], 0.0));
    wc.overlay.w_norm = static_cast<float>(TomlFloat(tbl["webcam"]["overlay_w"], 0.25));
    wc.overlay.h_norm = static_cast<float>(TomlFloat(tbl["webcam"]["overlay_h"], 0.25));
    wc.overlay_user_placed = TomlBool(tbl["webcam"]["overlay_user_placed"], false);
    wc.aspect_ratio_locked = TomlBool(tbl["webcam"]["aspect_ratio_locked"], true);
    wc.mirror = TomlBool(tbl["webcam"]["mirror"], false);
    wc.opacity = static_cast<float>(TomlFloat(tbl["webcam"]["opacity"], 1.0));
    wc.chroma_key.enabled = TomlBool(tbl["webcam"]["chroma_key"]["enabled"], false);
    {
        const auto m = WebcamChromaKeyColorModeFromString(
            QString::fromStdString(TomlStr(tbl["webcam"]["chroma_key"]["color_mode"])));
        if (m.has_value())
            wc.chroma_key.color_mode = *m;
    }
    wc.chroma_key.custom_r = static_cast<uint8_t>(TomlInt(tbl["webcam"]["chroma_key"]["custom_r"], 0));
    wc.chroma_key.custom_g = static_cast<uint8_t>(TomlInt(tbl["webcam"]["chroma_key"]["custom_g"], 255));
    wc.chroma_key.custom_b = static_cast<uint8_t>(TomlInt(tbl["webcam"]["chroma_key"]["custom_b"], 0));
    wc.chroma_key.tolerance = static_cast<float>(TomlFloat(tbl["webcam"]["chroma_key"]["tolerance"], 0.40));
    wc.chroma_key.softness = static_cast<float>(TomlFloat(tbl["webcam"]["chroma_key"]["softness"], 0.15));
    wc.chroma_key.spill_reduction = static_cast<float>(TomlFloat(tbl["webcam"]["chroma_key"]["spill"], 0.30));

    // --- Countdown ---
    config.countdown_seconds = static_cast<int>(TomlInt(tbl["countdown_seconds"], 0));

    return config;
}

// Parse a single TOML preset table into a RecordingPreset.
// Returns nullopt if the item is malformed (empty id).
std::optional<RecordingPreset> PresetFromToml(const toml::table& tbl) {
    RecordingPreset preset;

    preset.id = QString::fromStdString(TomlStr(tbl["id"])).trimmed().toStdString();
    if (preset.id.empty()) {
        return std::nullopt; // Malformed — skip.
    }
    preset.name = TomlStr(tbl["name"]);
    preset.config = ConfigFromToml(tbl);

    return preset;
}

// ---------------------------------------------------------------------------
// Serialize a document to a UTF-8 string (toml++ << operator).
// ---------------------------------------------------------------------------

std::string TomlDocToString(const toml::table& doc) {
    std::ostringstream oss;
    oss << doc;
    return oss.str();
}

// ---------------------------------------------------------------------------
// Atomic write: serialise `doc` and write via QSaveFile.
// Returns true on success; on failure sets *err (if non-null) and returns false.
// ---------------------------------------------------------------------------

bool WriteTomlAtomic(const toml::table& doc, const QString& path, QString* err) {
    const std::string toml_str = TomlDocToString(doc);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err)
            *err = QStringLiteral("Could not open file for writing: %1").arg(path);
        return false;
    }
    const QByteArray bytes = QByteArray::fromStdString(toml_str);
    if (file.write(bytes) != bytes.size()) {
        if (err)
            *err = QStringLiteral("Failed to write preset data to: %1").arg(path);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (err)
            *err = QStringLiteral("Failed to commit preset file (atomic rename): %1").arg(path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parse a TOML file non-throwingly.
// Returns an empty optional on any parse error; sets *parse_failed.
//
// Note: toml++ uses exceptions by default on MSVC (TOML_EXCEPTIONS=1).
// When exceptions are enabled, toml::parse_result IS toml::table, and
// toml::parse_file throws toml::parse_error on failure instead of returning
// a discriminated union.  We always wrap in try/catch for robustness.
// ---------------------------------------------------------------------------

std::optional<toml::table> ParseTomlFile(const QString& path, bool* parse_failed) {
    *parse_failed = false;
    try {
        return toml::parse_file(path.toStdString());
    } catch (const toml::parse_error&) {
        *parse_failed = true;
        return std::nullopt;
    } catch (...) {
        *parse_failed = true;
        return std::nullopt;
    }
}

// A first run (no file yet) is not a repair — there is nothing to fix.
PersistedPresetState MakeFirstRunState() {
    PersistedPresetState state;
    state.selected_id = std::string(kDefaultPresetId);
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
        file_path_ = QDir(config_dir).filePath(QStringLiteral("presets.toml"));
    } else {
        file_path_ = QStringLiteral("presets.toml");
    }
}

RecordingPresetStore::RecordingPresetStore(QString file_path) : file_path_(std::move(file_path)) {
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

PersistedPresetState RecordingPresetStore::Load() const {
    if (file_path_.isEmpty()) {
        return MakeFirstRunState();
    }

    // No file yet (first run) — nothing to repair.
    if (!QFileInfo::exists(file_path_)) {
        return MakeFirstRunState();
    }

    bool parse_failed = false;
    const auto maybe_doc = ParseTomlFile(file_path_, &parse_failed);
    if (!maybe_doc.has_value()) {
        PersistedPresetState state;
        state.selected_id = std::string(kDefaultPresetId);
        state.repaired = true; // Unparseable file — nothing survived.
        return state;
    }
    const toml::table& doc = *maybe_doc;

    // A schema mismatch alone is a routine version re-stamp, not a repair —
    // every field still parses cleanly and nothing is discarded. `repaired`
    // instead tracks whether an item actually had to be dropped below.
    const int64_t schema_version = TomlInt(doc["schema_version"], -1);
    // Files at or below this schema carry the ADR-0032 targeted color-range
    // rewrite on top of the ordinary field-wise repair (see RecordingPreset.h).
    const bool migrate_color_range = (schema_version >= 0 && schema_version <= kPresetSchemaColorRangeMigratedThrough);

    bool item_dropped = false;
    std::vector<RecordingPreset> accepted;
    std::set<std::string> seen_ids;

    // preset.default used to be an ordinary, editable preset before built-in
    // presets existed; every pre-upgrade presets.toml still carries one. It is
    // captured here (first occurrence wins) so its configuration can become
    // the live config below when the file predates [live] entirely — instead
    // of being silently discarded like the other three built-in ids, which no
    // released version ever wrote.
    std::optional<RecordingPresetConfig> carried_over_default_config;

    if (const toml::array* presets_arr = doc["presets"].as_array()) {
        for (const auto& elem : *presets_arr) {
            const auto* item_tbl = elem.as_table();
            if (!item_tbl) {
                item_dropped = true; // Malformed item — not even a table.
                continue;
            }
            const auto maybe = PresetFromToml(*item_tbl);
            if (!maybe.has_value()) {
                item_dropped = true; // Malformed item — skip.
                continue;
            }
            const RecordingPreset& raw = *maybe;
            if (raw.id.empty()) {
                item_dropped = true;
                continue;
            }
            if (IsBuiltInPresetId(raw.id)) {
                if (raw.id == kDefaultPresetId && !carried_over_default_config.has_value()) {
                    carried_over_default_config = raw.config;
                }
                continue; // Built-ins are code-defined — never loaded from disk.
            }
            if (seen_ids.count(raw.id) > 0) {
                item_dropped = true; // Duplicate id — drop later occurrence.
                continue;
            }
            seen_ids.insert(raw.id);
            RecordingPreset sanitized = SanitizePreset(raw);
            // ADR 0032: under schema <=19 "full" was the materialized old code
            // default — never an informed user choice (the colour-range combo
            // had a hydration bug and always displayed "Full (PC)" regardless
            // of the stored value). Rewrite it to the new Limited default; an
            // explicit "limited" is kept.
            if (migrate_color_range && sanitized.config.output.color_range == capability::ColorRange::Full) {
                sanitized.config.output.color_range = capability::ColorRange::Limited;
            }
            accepted.push_back(std::move(sanitized));
        }
    }

    // Shared by both the [live] table and the preset.default carry-over below
    // — one path applies the ADR-0032 color-range migration and sanitization
    // to a parsed config, regardless of which table it came from.
    const auto migrate_and_sanitize_live = [&](RecordingPresetConfig cfg) {
        if (migrate_color_range && cfg.output.color_range == capability::ColorRange::Full) {
            cfg.output.color_range = capability::ColorRange::Limited;
        }
        return SanitizePresetConfig(std::move(cfg));
    };

    std::optional<RecordingPresetConfig> live;
    if (const toml::table* live_tbl = doc["live"].as_table()) {
        live = migrate_and_sanitize_live(ConfigFromToml(*live_tbl));
    } else if (carried_over_default_config.has_value()) {
        // No [live] table: this is a pre-rework file. Its preset.default entry
        // (captured above) becomes the live config, once — the entry itself
        // does not survive into the new file, and this is not a repair since
        // nothing the user would notice was lost.
        live = migrate_and_sanitize_live(*carried_over_default_config);
    }

    // Repair selected_id: keep it if it names a built-in (always present) or a
    // surviving user preset; otherwise fall back to the built-in Default. The
    // registry re-repairs this independently, but the store keeps its own
    // contract self-consistent.
    std::string selected_id = TomlStr(doc["selected_id"]);
    const bool selected_is_builtin = IsBuiltInPresetId(selected_id);
    const bool selected_in_user_list =
        std::any_of(accepted.begin(), accepted.end(), [&](const RecordingPreset& p) { return p.id == selected_id; });
    if (!selected_is_builtin && !selected_in_user_list) {
        selected_id = std::string(kDefaultPresetId);
    }

    PersistedPresetState state;
    state.user_presets = std::move(accepted);
    state.selected_id = std::move(selected_id);
    state.live = std::move(live);
    state.repaired = item_dropped;
    return state;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

bool RecordingPresetStore::Save(const std::vector<RecordingPreset>& presets, const std::string& selected_id,
                                const RecordingPresetConfig& live, QString* err) const {
    if (file_path_.isEmpty()) {
        return true; // Nothing to persist — not a failure.
    }

    const QFileInfo info(file_path_);
    if (!QDir().mkpath(info.absolutePath())) {
        if (err)
            *err = QStringLiteral("Could not create parent directory: %1").arg(info.absolutePath());
        return false;
    }

    toml::table doc;
    doc.emplace("schema_version", static_cast<int64_t>(kPresetSchemaVersion));
    doc.emplace("selected_id", selected_id);
    doc.emplace("live", ConfigToToml(live));

    toml::array presets_arr;
    for (const auto& preset : presets) {
        if (IsBuiltInPresetId(preset.id))
            continue; // Built-ins are code-defined — never written to disk.
        presets_arr.push_back(PresetToToml(preset));
    }
    doc.emplace("presets", std::move(presets_arr));

    return WriteTomlAtomic(doc, file_path_, err);
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

    toml::table doc;
    doc.emplace("schema_version", static_cast<int64_t>(kPresetSchemaVersion));
    doc.emplace("export_kind", std::string("single"));

    toml::array presets_arr;
    presets_arr.push_back(PresetToToml(preset));
    doc.emplace("presets", std::move(presets_arr));

    return WriteTomlAtomic(doc, path, err);
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

    bool parse_failed = false;
    const auto maybe_doc = ParseTomlFile(path, &parse_failed);
    if (!maybe_doc.has_value()) {
        if (err)
            *err = QStringLiteral("Could not parse preset file (invalid TOML): %1").arg(path);
        return {};
    }
    const toml::table& doc = *maybe_doc;

    // Schema version check: best-effort — attempt to parse regardless.
    const int64_t file_version = TomlInt(doc["schema_version"], -1);
    const bool version_mismatch = (file_version != kPresetSchemaVersion);

    const toml::array* presets_arr = doc["presets"].as_array();
    if (!presets_arr || presets_arr->empty()) {
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
    result.reserve(static_cast<qsizetype>(presets_arr->size()));

    for (const auto& elem : *presets_arr) {
        const auto* item_tbl = elem.as_table();
        if (!item_tbl)
            continue;
        const auto maybe = PresetFromToml(*item_tbl);
        if (!maybe.has_value())
            continue; // Malformed — skip.

        RecordingPreset preset = SanitizePreset(*maybe);

        // Collision handling: if the id is already used, generate a fresh one.
        if (used_ids.count(preset.id) > 0) {
            preset.id = GeneratePresetId();
            // Name collisions are resolved by the registry's numeric dedupe
            // ("name (2)") at insert time — no marker suffix here.
        }
        used_ids.insert(preset.id);
        result.push_back(std::move(preset));
    }

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
