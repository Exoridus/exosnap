#pragma once

#include <capability/config_types.h>
#include <recorder_core/codec_types.h>
#include <recorder_core/output_geometry.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace exosnap {

enum class OutputResolutionMode {
    Native,
    UHD2160,
    QHD1440,
    FHD1080,
    HD720,
    Custom,
};

struct OutputResolutionSettings {
    OutputResolutionMode mode = OutputResolutionMode::Native;
    uint32_t custom_width = 0;
    uint32_t custom_height = 0;
    recorder_core::OutputFitMode fit = recorder_core::OutputFitMode::Contain;
};

// Automatic recording split (SPLIT-RECORDING-R1). Off keeps single-file behavior;
// the preset durations map onto RecordingSplitSettings at session start. Manual
// splits work regardless of mode.
enum class SplitRecordingMode {
    Off,
    Every15Min,
    Every30Min,
    Every60Min,
    Custom,
};

// Automatic split-by-size mode (SPLIT-BY-SIZE-R1). Independent of the time mode;
// when both are active, whichever threshold is hit first triggers the split.
enum class SplitSizeMode {
    Off,    // no size-based splitting
    Custom, // split when segment bytes >= custom_size_mb * 1024 * 1024
};

struct SplitRecordingSettings {
    SplitRecordingMode mode = SplitRecordingMode::Off;
    // Custom interval in minutes; clamped to [kMinMinutes, kMaxMinutes].
    uint32_t custom_minutes = 30;

    static constexpr uint32_t kMinMinutes = 1;
    static constexpr uint32_t kMaxMinutes = 24u * 60u; // 24 hours

    // Size threshold (SPLIT-BY-SIZE-R1): independent of the time threshold.
    SplitSizeMode size_mode = SplitSizeMode::Off;
    // Custom segment size in MiB; clamped to [kMinSizeMb, kMaxSizeMb].
    uint32_t custom_size_mb = 2048; // 2 GiB default

    static constexpr uint32_t kMinSizeMb = 50;
    static constexpr uint32_t kMaxSizeMb = 1024u * 1024u; // 1 TiB

    bool operator==(const SplitRecordingSettings&) const = default;
};

// Resolved media-time interval in milliseconds for the active mode, or 0 (Off).
[[nodiscard]] uint64_t SplitDurationMs(const SplitRecordingSettings& s) noexcept;
// Resolved segment size threshold in bytes for the active size mode, or 0 (Off).
[[nodiscard]] uint64_t SplitSizeBytes(const SplitRecordingSettings& s) noexcept;
// Clamp custom_minutes and custom_size_mb into their valid ranges.
void SanitizeSplitSettings(SplitRecordingSettings& s) noexcept;
[[nodiscard]] const wchar_t* SplitRecordingModeName(SplitRecordingMode mode) noexcept;
[[nodiscard]] const wchar_t* SplitSizeModeName(SplitSizeMode mode) noexcept;

struct OutputSettingsModel {
    std::filesystem::path output_folder;
    std::wstring naming_pattern = L"{datetime}_{app}_{title}";
    capability::Container container = capability::Container::Matroska;
    capability::VideoCodec video_codec = capability::VideoCodec::H264;
    capability::AudioCodec audio_codec = capability::AudioCodec::Aac;
    // Video bit depth (0.7.0). 8-bit is the universal default; 10-bit is only
    // valid for HEVC / AV1 (gated by capability::QueryCombo and reconciled in
    // SanitizePresetConfig — forced back to Bit8 for H.264 / unsupported combos).
    capability::BitDepth bit_depth = capability::BitDepth::Bit8;
    // Chroma subsampling (expert). 4:2:0 is the universal default; 4:4:4 is an
    // 8-bit H.264/HEVC-only expert path (gated by capability::QueryCombo /
    // QueryChroma444 and reconciled in SanitizePresetConfig — forced back to
    // Cs420 for AV1, 10-bit, or GPUs without YUV444 support).
    capability::ChromaSubsampling chroma_subsampling = capability::ChromaSubsampling::Cs420;
    // Y'CbCr quantization range. Limited (16-235, broadcast) is the default as of
    // fix/color-range-signaling: common consumer players (verified: VLC) ignore
    // the range flag entirely and always apply limited->full expansion, so a
    // Full-range recording looks permanently crushed/dark there regardless of
    // correct tagging — the same reason OBS and the rest of the consumer-video
    // ecosystem encode limited by default. Full (0-255, native screen precision)
    // remains available as an opt-in for pipelines known to honour the range
    // flag. Always valid for every codec/container — never gated.
    capability::ColorRange color_range = capability::ColorRange::Limited;
    // NVENC encoder speed/quality preset (P1 fastest/lowest quality .. P7
    // slowest/best quality). Applies uniformly to all three NVENC codecs; never
    // capability-gated. Default P4 (balanced) — matches the prior AV1/HEVC
    // default; H.264 previously used P6 (visible default change, expert-
    // overridable — see ADR 0039). Takes effect from the next recording
    // (not applied live).
    recorder_core::NvencPreset nvenc_preset = recorder_core::NvencPreset::P4;
    // HDR handling mode. Model only for now — no UI control yet; the expert
    // HDR control will gate on capability::QueryHdr10Native(). Default
    // TonemapSdr — see recorder_core::HdrMode.
    recorder_core::HdrMode hdr_mode = recorder_core::HdrMode::TonemapSdr;
    OutputResolutionSettings resolution;
    SplitRecordingSettings split;

    static OutputSettingsModel Defaults();
};

// Merges the format-editor-owned fields of a ConfigPage::formatSettingsChanged payload
// into the live output settings. MainWindow's handler routes through this ONE function
// so a model field can never again be dropped silently on the way to output_settings_
// (color_range and bit_depth were lost exactly that way: the combo emitted them but the
// field-by-field copy in the handler ignored them, so the recording never saw the
// selection). `split` is merged too (SETTINGS-HONESTY-R1): it was the last field the
// handler still dropped, so a live edit of the Output Split card (mode / custom minutes /
// size mode / custom MB) only reached output_settings_ via preset-apply or startup —
// never from the live Settings edit itself. Consumption still happens once, at recording
// start (RecordingCoordinator::SetOutputSettings) — "a split change applies from the next
// recording" remains the correct, intentional semantics; only the live-mirror gap is fixed.
void MergeFormatSelection(OutputSettingsModel& live, const OutputSettingsModel& incoming);

[[nodiscard]] std::optional<recorder_core::FrameSize> PresetOutputSize(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputResolutionModeName(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputFitModeName(recorder_core::OutputFitMode mode) noexcept;
[[nodiscard]] std::optional<recorder_core::FrameSize>
ResolveRequestedOutputSize(const OutputResolutionSettings& settings, recorder_core::FrameSize source) noexcept;
void SanitizeOutputResolution(OutputResolutionSettings& settings) noexcept;

} // namespace exosnap
