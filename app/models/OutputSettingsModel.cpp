#include "OutputSettingsModel.h"

#include <shlobj.h>
#include <windows.h>

namespace exosnap {

namespace {

// Canonical custom resolution bounds (CUSTOM-OUTPUT-RESOLUTION-R1).
constexpr uint32_t kMinCustomWidth = 320;
constexpr uint32_t kMinCustomHeight = 180;
constexpr uint32_t kMaxCustomDimension = 7680;

[[nodiscard]] bool IsCustomSizeUsable(uint32_t width, uint32_t height) noexcept {
    return width >= kMinCustomWidth && height >= kMinCustomHeight && width <= kMaxCustomDimension &&
           height <= kMaxCustomDimension;
}

} // namespace

OutputSettingsModel OutputSettingsModel::Defaults() {
    OutputSettingsModel defaults;

    PWSTR videos_path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &videos_path);
    if (SUCCEEDED(hr) && videos_path != nullptr && videos_path[0] != L'\0') {
        defaults.output_folder = std::filesystem::path(videos_path) / L"ExoSnap";
        CoTaskMemFree(videos_path);
    } else {
        if (videos_path != nullptr) {
            CoTaskMemFree(videos_path);
        }

        wchar_t profile[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            defaults.output_folder = std::filesystem::path(profile) / L"Videos" / L"ExoSnap";
        } else {
            defaults.output_folder = std::filesystem::path(L"C:\\Users\\Public\\Videos\\ExoSnap");
        }
    }

    return defaults;
}

std::optional<recorder_core::FrameSize> PresetOutputSize(OutputResolutionMode mode) noexcept {
    switch (mode) {
    case OutputResolutionMode::UHD2160:
        return recorder_core::FrameSize{3840, 2160};
    case OutputResolutionMode::QHD1440:
        return recorder_core::FrameSize{2560, 1440};
    case OutputResolutionMode::FHD1080:
        return recorder_core::FrameSize{1920, 1080};
    case OutputResolutionMode::HD720:
        return recorder_core::FrameSize{1280, 720};
    case OutputResolutionMode::Native:
    case OutputResolutionMode::Custom:
        return std::nullopt;
    }
    return std::nullopt;
}

const wchar_t* OutputResolutionModeName(OutputResolutionMode mode) noexcept {
    switch (mode) {
    case OutputResolutionMode::Native:
        return L"Native";
    case OutputResolutionMode::UHD2160:
        return L"4K";
    case OutputResolutionMode::QHD1440:
        return L"1440p";
    case OutputResolutionMode::FHD1080:
        return L"1080p";
    case OutputResolutionMode::HD720:
        return L"720p";
    case OutputResolutionMode::Custom:
        return L"Custom";
    }
    return L"Native";
}

const wchar_t* OutputFitModeName(recorder_core::OutputFitMode mode) noexcept {
    switch (mode) {
    case recorder_core::OutputFitMode::Contain:
        return L"Fit";
    }
    return L"Fit";
}

std::optional<recorder_core::FrameSize> ResolveRequestedOutputSize(const OutputResolutionSettings& settings,
                                                                   recorder_core::FrameSize source) noexcept {
    if (settings.mode == OutputResolutionMode::Native) {
        const recorder_core::FrameSize aligned = recorder_core::AlignOutputSizeEven(source);
        if (!recorder_core::IsEncoderAlignedSize(aligned)) {
            return std::nullopt;
        }
        return aligned;
    }

    if (const std::optional<recorder_core::FrameSize> preset = PresetOutputSize(settings.mode)) {
        return *preset;
    }

    if (settings.mode == OutputResolutionMode::Custom) {
        if (!IsCustomSizeUsable(settings.custom_width, settings.custom_height)) {
            return std::nullopt;
        }
        const recorder_core::FrameSize aligned =
            recorder_core::AlignOutputSizeEven({settings.custom_width, settings.custom_height});
        if (!recorder_core::IsEncoderAlignedSize(aligned)) {
            return std::nullopt;
        }
        return aligned;
    }

    return std::nullopt;
}

void SanitizeOutputResolution(OutputResolutionSettings& settings) noexcept {
    if (settings.fit != recorder_core::OutputFitMode::Contain) {
        settings.fit = recorder_core::OutputFitMode::Contain;
    }

    if (settings.mode != OutputResolutionMode::Custom) {
        settings.custom_width = 0;
        settings.custom_height = 0;
        return;
    }

    if (!IsCustomSizeUsable(settings.custom_width, settings.custom_height)) {
        settings.mode = OutputResolutionMode::Native;
        settings.custom_width = 0;
        settings.custom_height = 0;
        return;
    }

    settings.custom_width = recorder_core::AlignOutputDimensionEven(settings.custom_width);
    settings.custom_height = recorder_core::AlignOutputDimensionEven(settings.custom_height);
}

uint64_t SplitDurationMs(const SplitRecordingSettings& s) noexcept {
    switch (s.mode) {
    case SplitRecordingMode::Off:
        return 0;
    case SplitRecordingMode::Every15Min:
        return 15ull * 60ull * 1000ull;
    case SplitRecordingMode::Every30Min:
        return 30ull * 60ull * 1000ull;
    case SplitRecordingMode::Every60Min:
        return 60ull * 60ull * 1000ull;
    case SplitRecordingMode::Custom: {
        uint32_t minutes = s.custom_minutes;
        if (minutes < SplitRecordingSettings::kMinMinutes)
            minutes = SplitRecordingSettings::kMinMinutes;
        if (minutes > SplitRecordingSettings::kMaxMinutes)
            minutes = SplitRecordingSettings::kMaxMinutes;
        return static_cast<uint64_t>(minutes) * 60ull * 1000ull;
    }
    }
    return 0;
}

uint64_t SplitSizeBytes(const SplitRecordingSettings& s) noexcept {
    if (s.size_mode == SplitSizeMode::Off)
        return 0;
    uint32_t mb = s.custom_size_mb;
    if (mb < SplitRecordingSettings::kMinSizeMb)
        mb = SplitRecordingSettings::kMinSizeMb;
    if (mb > SplitRecordingSettings::kMaxSizeMb)
        mb = SplitRecordingSettings::kMaxSizeMb;
    return static_cast<uint64_t>(mb) * 1024ULL * 1024ULL;
}

void SanitizeSplitSettings(SplitRecordingSettings& s) noexcept {
    if (s.custom_minutes < SplitRecordingSettings::kMinMinutes)
        s.custom_minutes = SplitRecordingSettings::kMinMinutes;
    if (s.custom_minutes > SplitRecordingSettings::kMaxMinutes)
        s.custom_minutes = SplitRecordingSettings::kMaxMinutes;
    if (s.custom_size_mb < SplitRecordingSettings::kMinSizeMb)
        s.custom_size_mb = SplitRecordingSettings::kMinSizeMb;
    if (s.custom_size_mb > SplitRecordingSettings::kMaxSizeMb)
        s.custom_size_mb = SplitRecordingSettings::kMaxSizeMb;
}

const wchar_t* SplitRecordingModeName(SplitRecordingMode mode) noexcept {
    switch (mode) {
    case SplitRecordingMode::Off:
        return L"Off";
    case SplitRecordingMode::Every15Min:
        return L"Every 15 min";
    case SplitRecordingMode::Every30Min:
        return L"Every 30 min";
    case SplitRecordingMode::Every60Min:
        return L"Every 60 min";
    case SplitRecordingMode::Custom:
        return L"Custom";
    }
    return L"Off";
}

const wchar_t* SplitSizeModeName(SplitSizeMode mode) noexcept {
    switch (mode) {
    case SplitSizeMode::Off:
        return L"Off";
    case SplitSizeMode::Custom:
        return L"Custom";
    }
    return L"Off";
}

} // namespace exosnap
