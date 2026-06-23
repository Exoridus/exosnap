#pragma once

#include <capability/config_types.h>
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
    capability::VideoCodec video_codec = capability::VideoCodec::H264Nvenc;
    capability::AudioCodec audio_codec = capability::AudioCodec::AacMf;
    // Video bit depth (0.7.0). 8-bit is the universal default; 10-bit is only
    // valid for HEVC / AV1 (gated by capability::QueryCombo and reconciled in
    // SanitizePresetConfig — forced back to Bit8 for H.264 / unsupported combos).
    capability::BitDepth bit_depth = capability::BitDepth::Bit8;
    OutputResolutionSettings resolution;
    SplitRecordingSettings split;

    static OutputSettingsModel Defaults();
};

[[nodiscard]] std::optional<recorder_core::FrameSize> PresetOutputSize(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputResolutionModeName(OutputResolutionMode mode) noexcept;
[[nodiscard]] const wchar_t* OutputFitModeName(recorder_core::OutputFitMode mode) noexcept;
[[nodiscard]] std::optional<recorder_core::FrameSize>
ResolveRequestedOutputSize(const OutputResolutionSettings& settings, recorder_core::FrameSize source) noexcept;
void SanitizeOutputResolution(OutputResolutionSettings& settings) noexcept;

} // namespace exosnap
