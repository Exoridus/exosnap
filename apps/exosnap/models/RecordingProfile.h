#pragma once

#include "OutputSettingsModel.h"
#include "VideoSettingsModel.h"

#include <capability/audio_ui_state.h>
#include <capability/config_types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap {

enum class RecordingProfileSource {
    BuiltIn,
    User,
};

enum class RecordingProfileAvailability {
    Available,
    Unavailable,
};

struct RecordingProfile {
    std::string id;
    std::string name;
    RecordingProfileSource source = RecordingProfileSource::BuiltIn;
    RecordingProfileAvailability availability = RecordingProfileAvailability::Available;
    std::string availability_reason;
    std::optional<std::string> base_builtin_id;

    OutputSettingsModel output;
    VideoSettingsModel video;
    capability::AudioUiState audio_ui_state;
};

struct ActiveRecordingProfileState {
    std::string active_profile_id;
    bool active_profile_modified = false;
};

constexpr std::string_view kBuiltInProfileMkvH264AacId = "builtin.mkv_h264_aac";
constexpr std::string_view kBuiltInProfileWebmAv1OpusId = "builtin.webm_av1_opus";
constexpr std::string_view kBuiltInProfileMp4H264AacId = "builtin.mp4_h264_aac";

[[nodiscard]] std::vector<RecordingProfile> MakeBuiltInRecordingProfiles();
[[nodiscard]] bool IsBuiltInProfileId(std::string_view profile_id) noexcept;
[[nodiscard]] std::optional<RecordingProfile> FindBuiltInRecordingProfile(std::string_view profile_id);
[[nodiscard]] RecordingProfile MakeSafeDefaultUserProfile();
[[nodiscard]] std::wstring CodecToken(capability::VideoCodec codec);
[[nodiscard]] std::wstring CodecToken(capability::AudioCodec codec);
[[nodiscard]] std::wstring ContainerToken(capability::Container container);

} // namespace exosnap
