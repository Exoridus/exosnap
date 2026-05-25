#include "RecordingProfile.h"

#include <algorithm>

namespace exosnap {
namespace {

capability::AudioUiState DefaultAudioUiState() {
    capability::AudioUiState state;
    state.target_kind = capability::CaptureTargetKind::Display;
    state.source_rows = {
        {recorder_core::AudioSourceKind::SystemOutput, true, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
    };
    return state;
}

RecordingProfile MakeBuiltInProfile(std::string id, std::string name, capability::Container container,
                                    capability::VideoCodec video_codec, capability::AudioCodec audio_codec) {
    RecordingProfile profile;
    profile.id = std::move(id);
    profile.name = std::move(name);
    profile.source = RecordingProfileSource::BuiltIn;
    profile.output = OutputSettingsModel::Defaults();
    profile.output.container = container;
    profile.output.video_codec = video_codec;
    profile.output.audio_codec = audio_codec;
    profile.video = VideoSettingsModel::Defaults();
    profile.audio_ui_state = DefaultAudioUiState();
    return profile;
}

} // namespace

std::vector<RecordingProfile> MakeBuiltInRecordingProfiles() {
    return {
        MakeBuiltInProfile(std::string(kBuiltInProfileMkvH264AacId), "MKV · H.264 · AAC",
                           capability::Container::Matroska, capability::VideoCodec::H264Nvenc,
                           capability::AudioCodec::AacMf),
        MakeBuiltInProfile(std::string(kBuiltInProfileWebmAv1OpusId), "WebM · AV1 · Opus", capability::Container::WebM,
                           capability::VideoCodec::Av1Nvenc, capability::AudioCodec::Opus),
        MakeBuiltInProfile(std::string(kBuiltInProfileMp4H264AacId), "MP4 · H.264 · AAC", capability::Container::Mp4,
                           capability::VideoCodec::H264Nvenc, capability::AudioCodec::AacMf),
    };
}

bool IsBuiltInProfileId(std::string_view profile_id) noexcept {
    return profile_id == kBuiltInProfileMkvH264AacId || profile_id == kBuiltInProfileWebmAv1OpusId ||
           profile_id == kBuiltInProfileMp4H264AacId;
}

std::optional<RecordingProfile> FindBuiltInRecordingProfile(std::string_view profile_id) {
    const std::vector<RecordingProfile> builtins = MakeBuiltInRecordingProfiles();
    const auto it = std::find_if(builtins.begin(), builtins.end(),
                                 [profile_id](const RecordingProfile& profile) { return profile.id == profile_id; });
    if (it == builtins.end()) {
        return std::nullopt;
    }
    return *it;
}

RecordingProfile MakeSafeDefaultUserProfile() {
    RecordingProfile profile = MakeBuiltInRecordingProfiles().front();
    profile.source = RecordingProfileSource::User;
    profile.id = "user.default";
    profile.name = "My Profile";
    profile.base_builtin_id = std::string(kBuiltInProfileMkvH264AacId);
    return profile;
}

std::wstring CodecToken(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return L"h264";
    case capability::VideoCodec::HevcNvenc:
        return L"hevc";
    case capability::VideoCodec::Av1Nvenc:
        return L"av1";
    }
    return L"h264";
}

std::wstring CodecToken(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return L"aac";
    case capability::AudioCodec::Opus:
        return L"opus";
    case capability::AudioCodec::Pcm:
        return L"pcm";
    }
    return L"aac";
}

std::wstring ContainerToken(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return L"mkv";
    case capability::Container::Mp4:
        return L"mp4";
    case capability::Container::WebM:
        return L"webm";
    }
    return L"mkv";
}

} // namespace exosnap
