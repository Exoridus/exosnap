#include "AppSettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringView>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace exosnap {
namespace {

constexpr int kSettingsVersionCurrent = 4;
constexpr int kMicGainDbDefault = 0;
constexpr int kMicGainDbMin = 0;
constexpr int kMicGainDbMax = 24;

QString ContainerToString(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("mkv");
    case capability::Container::Mp4:
        return QStringLiteral("mp4");
    case capability::Container::WebM:
        return QStringLiteral("webm");
    }
    return QStringLiteral("mkv");
}

std::optional<capability::Container> ContainerFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("mkv")) {
        return capability::Container::Matroska;
    }
    if (normalized == QStringLiteral("mp4")) {
        return capability::Container::Mp4;
    }
    if (normalized == QStringLiteral("webm")) {
        return capability::Container::WebM;
    }
    return std::nullopt;
}

QString VideoCodecToString(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("h264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("hevc");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("av1");
    }
    return QStringLiteral("h264");
}

std::optional<capability::VideoCodec> VideoCodecFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("h264")) {
        return capability::VideoCodec::H264Nvenc;
    }
    if (normalized == QStringLiteral("hevc")) {
        return capability::VideoCodec::HevcNvenc;
    }
    if (normalized == QStringLiteral("av1")) {
        return capability::VideoCodec::Av1Nvenc;
    }
    return std::nullopt;
}

QString AudioCodecToString(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("aac");
    case capability::AudioCodec::Opus:
        return QStringLiteral("opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("pcm");
    }
    return QStringLiteral("aac");
}

std::optional<capability::AudioCodec> AudioCodecFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("opus")) {
        return capability::AudioCodec::Opus;
    }
    if (normalized == QStringLiteral("aac")) {
        return capability::AudioCodec::AacMf;
    }
    if (normalized == QStringLiteral("pcm")) {
        return capability::AudioCodec::Pcm;
    }
    return std::nullopt;
}

QString MicChannelModeToString(recorder_core::MicChannelMode mode) {
    switch (mode) {
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

std::optional<recorder_core::MicChannelMode> MicChannelModeFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("auto")) {
        return recorder_core::MicChannelMode::Auto;
    }
    if (normalized == QStringLiteral("preserve")) {
        return recorder_core::MicChannelMode::PreserveStereo;
    }
    if (normalized == QStringLiteral("mono_mix")) {
        return recorder_core::MicChannelMode::MonoMix;
    }
    if (normalized == QStringLiteral("left")) {
        return recorder_core::MicChannelMode::LeftToStereo;
    }
    if (normalized == QStringLiteral("right")) {
        return recorder_core::MicChannelMode::RightToStereo;
    }
    return std::nullopt;
}

QString CaptureTargetKindToString(capability::CaptureTargetKind kind) {
    switch (kind) {
    case capability::CaptureTargetKind::Window:
        return QStringLiteral("window");
    case capability::CaptureTargetKind::Display:
        return QStringLiteral("display");
    }
    return QStringLiteral("display");
}

std::optional<capability::CaptureTargetKind> CaptureTargetKindFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("window")) {
        return capability::CaptureTargetKind::Window;
    }
    if (normalized == QStringLiteral("display")) {
        return capability::CaptureTargetKind::Display;
    }
    return std::nullopt;
}

QString NvencQualityPresetToString(recorder_core::NvencQualityPreset preset) {
    switch (preset) {
    case recorder_core::NvencQualityPreset::High:
        return QStringLiteral("high");
    case recorder_core::NvencQualityPreset::Balanced:
        return QStringLiteral("balanced");
    case recorder_core::NvencQualityPreset::Small:
        return QStringLiteral("small");
    }
    return QStringLiteral("balanced");
}

std::optional<recorder_core::NvencQualityPreset> NvencQualityPresetFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("high"))
        return recorder_core::NvencQualityPreset::High;
    if (normalized == QStringLiteral("balanced"))
        return recorder_core::NvencQualityPreset::Balanced;
    if (normalized == QStringLiteral("small"))
        return recorder_core::NvencQualityPreset::Small;
    return std::nullopt;
}

QString AudioSourceKindToString(recorder_core::AudioSourceKind kind) {
    switch (kind) {
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

std::optional<recorder_core::AudioSourceKind> AudioSourceKindFromString(QStringView value) {
    const QString s = value.trimmed().toString().toLower();
    if (s == QStringLiteral("app"))
        return recorder_core::AudioSourceKind::App;
    if (s == QStringLiteral("mic"))
        return recorder_core::AudioSourceKind::Mic;
    if (s == QStringLiteral("sys"))
        return recorder_core::AudioSourceKind::Sys;
    if (s == QStringLiteral("system_output"))
        return recorder_core::AudioSourceKind::SystemOutput;
    return std::nullopt;
}

float MicGainLinearFromDb(int mic_gain_db) {
    const int clamped = std::clamp(mic_gain_db, kMicGainDbMin, kMicGainDbMax);
    return std::pow(10.0f, static_cast<float>(clamped) / 20.0f);
}

int MicGainDbFromLinear(float mic_gain_linear) {
    if (mic_gain_linear <= 0.0f) {
        return kMicGainDbMin;
    }
    const int db = static_cast<int>(std::round(20.0f * std::log10f(mic_gain_linear)));
    return std::clamp(db, kMicGainDbMin, kMicGainDbMax);
}

void EnforceContainerCodecCompatibility(OutputSettingsModel& output) {
    if (output.container == capability::Container::Mp4) {
        output.video_codec = capability::VideoCodec::H264Nvenc;
        output.audio_codec = capability::AudioCodec::AacMf;
    } else if (output.container == capability::Container::WebM) {
        output.video_codec = capability::VideoCodec::Av1Nvenc;
        output.audio_codec = capability::AudioCodec::Opus;
    } else if (output.container == capability::Container::Matroska) {
        if (output.video_codec == capability::VideoCodec::HevcNvenc) {
            output.video_codec = capability::VideoCodec::H264Nvenc;
        }
        if (output.video_codec == capability::VideoCodec::H264Nvenc &&
            output.audio_codec == capability::AudioCodec::Opus) {
            output.audio_codec = capability::AudioCodec::AacMf;
        }
        if (output.audio_codec == capability::AudioCodec::Pcm) {
            output.audio_codec = capability::AudioCodec::AacMf;
        }
    }
}

void LoadOutputSettingsFromCurrentGroup(QSettings& settings, OutputSettingsModel* out_output) {
    const QString folder = settings.value(QStringLiteral("folder")).toString().trimmed();
    if (!folder.isEmpty()) {
        out_output->output_folder = std::filesystem::path(folder.toStdWString());
    }

    const QString naming_pattern = settings.value(QStringLiteral("naming_pattern")).toString();
    if (!naming_pattern.isEmpty()) {
        out_output->naming_pattern = naming_pattern.toStdWString();
    }

    if (const auto container = ContainerFromString(settings.value(QStringLiteral("container")).toString());
        container.has_value()) {
        out_output->container = *container;
    }

    if (const auto video_codec = VideoCodecFromString(settings.value(QStringLiteral("video_codec")).toString());
        video_codec.has_value()) {
        out_output->video_codec = *video_codec;
    }

    if (const auto audio_codec = AudioCodecFromString(settings.value(QStringLiteral("audio_codec")).toString());
        audio_codec.has_value()) {
        out_output->audio_codec = *audio_codec;
    }
}

void SaveOutputSettingsToCurrentGroup(QSettings& settings, const OutputSettingsModel& output) {
    settings.setValue(QStringLiteral("folder"), QString::fromStdWString(output.output_folder.wstring()));
    settings.setValue(QStringLiteral("naming_pattern"), QString::fromStdWString(output.naming_pattern));
    settings.setValue(QStringLiteral("container"), ContainerToString(output.container));
    settings.setValue(QStringLiteral("video_codec"), VideoCodecToString(output.video_codec));
    settings.setValue(QStringLiteral("audio_codec"), AudioCodecToString(output.audio_codec));
}

void LoadVideoSettingsFromCurrentGroup(QSettings& settings, VideoSettingsModel* out_video) {
    if (const auto quality = NvencQualityPresetFromString(settings.value(QStringLiteral("quality")).toString());
        quality.has_value()) {
        out_video->quality = *quality;
    }
    if (settings.contains(QStringLiteral("cfr"))) {
        out_video->cfr = settings.value(QStringLiteral("cfr"), true).toBool();
    }
    if (settings.contains(QStringLiteral("capture_cursor"))) {
        out_video->capture_cursor = settings.value(QStringLiteral("capture_cursor"), true).toBool();
    }
}

void SaveVideoSettingsToCurrentGroup(QSettings& settings, const VideoSettingsModel& video) {
    settings.setValue(QStringLiteral("quality"), NvencQualityPresetToString(video.quality));
    settings.setValue(QStringLiteral("cfr"), video.cfr);
    settings.setValue(QStringLiteral("capture_cursor"), video.capture_cursor);
}

void LoadWebcamSettingsFromCurrentGroup(QSettings& settings, WebcamSettings* out) {
    out->enabled = settings.value(QStringLiteral("enabled"), false).toBool();
    out->device_id = settings.value(QStringLiteral("device_id")).toString().toStdString();
    out->width = settings.value(QStringLiteral("width"), 1280).toInt();
    out->height = settings.value(QStringLiteral("height"), 720).toInt();
    out->fps = settings.value(QStringLiteral("fps"), 30).toInt();
    out->overlay.x_norm = settings.value(QStringLiteral("overlay_x"), 0.0f).toFloat();
    out->overlay.y_norm = settings.value(QStringLiteral("overlay_y"), 0.0f).toFloat();
    out->overlay.w_norm = settings.value(QStringLiteral("overlay_w"), 0.25f).toFloat();
    out->overlay.h_norm = settings.value(QStringLiteral("overlay_h"), 0.25f).toFloat();
    out->overlay_user_placed = settings.value(QStringLiteral("overlay_user_placed"), false).toBool();
    out->aspect_ratio_locked = settings.value(QStringLiteral("aspect_ratio_locked"), true).toBool();
    out->chroma_key.enabled = settings.value(QStringLiteral("chroma_enabled"), false).toBool();
    out->chroma_key.r = static_cast<uint8_t>(settings.value(QStringLiteral("chroma_r"), 0).toInt());
    out->chroma_key.g = static_cast<uint8_t>(settings.value(QStringLiteral("chroma_g"), 177).toInt());
    out->chroma_key.b = static_cast<uint8_t>(settings.value(QStringLiteral("chroma_b"), 64).toInt());
    out->chroma_key.tolerance = settings.value(QStringLiteral("chroma_tolerance"), 0.30f).toFloat();
    out->chroma_key.softness = settings.value(QStringLiteral("chroma_softness"), 0.05f).toFloat();
    *out = SanitizeWebcamSettings(*out);
}

void SaveWebcamSettingsToCurrentGroup(QSettings& settings, const WebcamSettings& webcam) {
    const WebcamSettings sanitized = SanitizeWebcamSettings(webcam);
    settings.setValue(QStringLiteral("enabled"), sanitized.enabled);
    settings.setValue(QStringLiteral("device_id"), QString::fromStdString(sanitized.device_id));
    settings.setValue(QStringLiteral("width"), sanitized.width);
    settings.setValue(QStringLiteral("height"), sanitized.height);
    settings.setValue(QStringLiteral("fps"), sanitized.fps);
    settings.setValue(QStringLiteral("overlay_x"), sanitized.overlay.x_norm);
    settings.setValue(QStringLiteral("overlay_y"), sanitized.overlay.y_norm);
    settings.setValue(QStringLiteral("overlay_w"), sanitized.overlay.w_norm);
    settings.setValue(QStringLiteral("overlay_h"), sanitized.overlay.h_norm);
    settings.setValue(QStringLiteral("overlay_user_placed"), sanitized.overlay_user_placed);
    settings.setValue(QStringLiteral("aspect_ratio_locked"), sanitized.aspect_ratio_locked);
    settings.setValue(QStringLiteral("chroma_enabled"), sanitized.chroma_key.enabled);
    settings.setValue(QStringLiteral("chroma_r"), static_cast<int>(sanitized.chroma_key.r));
    settings.setValue(QStringLiteral("chroma_g"), static_cast<int>(sanitized.chroma_key.g));
    settings.setValue(QStringLiteral("chroma_b"), static_cast<int>(sanitized.chroma_key.b));
    settings.setValue(QStringLiteral("chroma_tolerance"), sanitized.chroma_key.tolerance);
    settings.setValue(QStringLiteral("chroma_softness"), sanitized.chroma_key.softness);
}

void LoadAudioStateFromCurrentGroup(QSettings& settings, capability::AudioUiState* out_audio) {
    out_audio->source_rows.clear();

    const int row_count = settings.value(QStringLiteral("source_row_count"), 0).toInt();
    for (int i = 0; i < row_count; ++i) {
        const QString prefix = QStringLiteral("source_row_%1_").arg(i);
        const auto kind = AudioSourceKindFromString(settings.value(prefix + QStringLiteral("kind")).toString());
        if (!kind.has_value())
            continue;
        recorder_core::AudioSourceRow row;
        row.kind = *kind;
        row.enabled = settings.value(prefix + QStringLiteral("enabled"), true).toBool();
        row.merge_with_above = settings.value(prefix + QStringLiteral("merge"), false).toBool();
        out_audio->source_rows.push_back(row);
    }

    if (const auto mode = MicChannelModeFromString(settings.value(QStringLiteral("mic_channel_mode")).toString());
        mode.has_value()) {
        out_audio->mic_channel_mode = *mode;
    }

    if (const auto target_kind = CaptureTargetKindFromString(settings.value(QStringLiteral("target_kind")).toString());
        target_kind.has_value()) {
        out_audio->target_kind = *target_kind;
    }

    const QString selected_mic_device_id =
        settings.value(QStringLiteral("selected_mic_device_id")).toString().trimmed();
    if (selected_mic_device_id.isEmpty()) {
        out_audio->selected_mic_device_id = std::nullopt;
    } else {
        out_audio->selected_mic_device_id = selected_mic_device_id.toStdString();
    }

    bool mic_gain_ok = false;
    const int mic_gain_db = settings.value(QStringLiteral("mic_gain_db"), kMicGainDbDefault).toInt(&mic_gain_ok);
    out_audio->mic_gain_linear = mic_gain_ok ? MicGainLinearFromDb(mic_gain_db) : 1.0f;
}

void SaveAudioStateToCurrentGroup(QSettings& settings, const capability::AudioUiState& audio) {
    const auto& rows = audio.source_rows;
    settings.setValue(QStringLiteral("source_row_count"), static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const QString prefix = QStringLiteral("source_row_%1_").arg(i);
        settings.setValue(prefix + QStringLiteral("kind"),
                          AudioSourceKindToString(rows[static_cast<std::size_t>(i)].kind));
        settings.setValue(prefix + QStringLiteral("enabled"), rows[static_cast<std::size_t>(i)].enabled);
        settings.setValue(prefix + QStringLiteral("merge"), rows[static_cast<std::size_t>(i)].merge_with_above);
    }

    settings.setValue(QStringLiteral("target_kind"), CaptureTargetKindToString(audio.target_kind));
    settings.setValue(QStringLiteral("mic_channel_mode"), MicChannelModeToString(audio.mic_channel_mode));
    settings.setValue(QStringLiteral("selected_mic_device_id"),
                      audio.selected_mic_device_id.has_value() ? QString::fromStdString(*audio.selected_mic_device_id)
                                                               : QString());
    settings.setValue(QStringLiteral("mic_gain_db"), MicGainDbFromLinear(audio.mic_gain_linear));
}

RecordingProfile LoadProfileFromCurrentGroup(QSettings& settings, RecordingProfileSource source) {
    RecordingProfile profile;
    profile.source = source;
    profile.output = OutputSettingsModel::Defaults();
    profile.video = VideoSettingsModel::Defaults();
    profile.audio_ui_state = capability::AudioUiState{};

    profile.id = settings.value(QStringLiteral("id")).toString().trimmed().toStdString();
    profile.name = settings.value(QStringLiteral("name")).toString().trimmed().toStdString();
    const QString base_builtin_id = settings.value(QStringLiteral("base_builtin_id")).toString().trimmed();
    if (!base_builtin_id.isEmpty()) {
        profile.base_builtin_id = base_builtin_id.toStdString();
    }

    settings.beginGroup(QStringLiteral("output"));
    LoadOutputSettingsFromCurrentGroup(settings, &profile.output);
    settings.endGroup();
    EnforceContainerCodecCompatibility(profile.output);

    settings.beginGroup(QStringLiteral("video"));
    LoadVideoSettingsFromCurrentGroup(settings, &profile.video);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    LoadAudioStateFromCurrentGroup(settings, &profile.audio_ui_state);
    settings.endGroup();

    return profile;
}

void SaveProfileToCurrentGroup(QSettings& settings, const RecordingProfile& profile) {
    settings.setValue(QStringLiteral("id"), QString::fromStdString(profile.id));
    settings.setValue(QStringLiteral("name"), QString::fromStdString(profile.name));
    settings.setValue(QStringLiteral("base_builtin_id"), profile.base_builtin_id.has_value()
                                                             ? QString::fromStdString(*profile.base_builtin_id)
                                                             : QString());

    settings.beginGroup(QStringLiteral("output"));
    SaveOutputSettingsToCurrentGroup(settings, profile.output);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("video"));
    SaveVideoSettingsToCurrentGroup(settings, profile.video);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    SaveAudioStateToCurrentGroup(settings, profile.audio_ui_state);
    settings.endGroup();
}

bool ProfileMatchesSettings(const RecordingProfile& profile, const PersistedAppSettings& settings) {
    const auto rows_equal = [](const std::vector<recorder_core::AudioSourceRow>& lhs,
                               const std::vector<recorder_core::AudioSourceRow>& rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            if (lhs[i].kind != rhs[i].kind || lhs[i].enabled != rhs[i].enabled ||
                lhs[i].merge_with_above != rhs[i].merge_with_above) {
                return false;
            }
        }
        return true;
    };

    return profile.output.output_folder == settings.output.output_folder &&
           profile.output.naming_pattern == settings.output.naming_pattern &&
           profile.output.container == settings.output.container &&
           profile.output.video_codec == settings.output.video_codec &&
           profile.output.audio_codec == settings.output.audio_codec &&
           profile.video.quality == settings.video.quality && profile.video.cfr == settings.video.cfr &&
           profile.video.capture_cursor == settings.video.capture_cursor &&
           rows_equal(profile.audio_ui_state.source_rows, settings.audio_ui_state.source_rows) &&
           profile.audio_ui_state.target_kind == settings.audio_ui_state.target_kind &&
           profile.audio_ui_state.mic_channel_mode == settings.audio_ui_state.mic_channel_mode &&
           profile.audio_ui_state.selected_mic_device_id == settings.audio_ui_state.selected_mic_device_id;
}

std::optional<RecordingProfile> FindProfileById(const std::vector<RecordingProfile>& profiles, const std::string& id) {
    const auto it = std::find_if(profiles.begin(), profiles.end(),
                                 [&id](const RecordingProfile& profile) { return profile.id == id; });
    if (it == profiles.end()) {
        return std::nullopt;
    }
    return *it;
}

RecordingProfile ResolveActiveProfile(const PersistedAppSettings& persisted) {
    const std::string active_id = persisted.active_profile.active_profile_id.empty()
                                      ? std::string(kBuiltInProfileMkvH264AacId)
                                      : persisted.active_profile.active_profile_id;

    if (const auto user = FindProfileById(persisted.user_profiles, active_id); user.has_value()) {
        return *user;
    }

    if (persisted.active_profile.active_profile_modified) {
        if (const auto modified = FindProfileById(persisted.modified_builtin_profiles, active_id);
            modified.has_value()) {
            return *modified;
        }
    }

    if (const auto builtin = FindBuiltInRecordingProfile(active_id); builtin.has_value()) {
        return *builtin;
    }

    const auto default_builtin = FindBuiltInRecordingProfile(kBuiltInProfileMkvH264AacId);
    return default_builtin.value_or(MakeSafeDefaultUserProfile());
}

void ReconcileActiveSettingsFromProfile(PersistedAppSettings* persisted) {
    const RecordingProfile active_profile = ResolveActiveProfile(*persisted);
    persisted->output = active_profile.output;
    persisted->video = active_profile.video;
    persisted->audio_ui_state = active_profile.audio_ui_state;
}

} // namespace

AppSettingsStore::AppSettingsStore() {
    const QString config_dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!config_dir.isEmpty()) {
        QDir().mkpath(config_dir);
        settings_path_ = QDir(config_dir).filePath(QStringLiteral("settings.ini"));
    } else {
        settings_path_ = QStringLiteral("settings.ini");
    }
}

AppSettingsStore::AppSettingsStore(QString settings_file_path) : settings_path_(std::move(settings_file_path)) {
}

PersistedAppSettings AppSettingsStore::Load() const {
    PersistedAppSettings persisted;
    persisted.output = OutputSettingsModel::Defaults();
    persisted.video = VideoSettingsModel::Defaults();
    persisted.audio_ui_state = capability::AudioUiState{};
    persisted.active_profile.active_profile_id = std::string(kBuiltInProfileMkvH264AacId);

    if (settings_path_.isEmpty()) {
        return persisted;
    }

    QSettings settings(settings_path_, QSettings::IniFormat);
    const int settings_version = settings.value(QStringLiteral("settings_version")).toInt();

    settings.beginGroup(QStringLiteral("output"));
    LoadOutputSettingsFromCurrentGroup(settings, &persisted.output);
    settings.endGroup();
    EnforceContainerCodecCompatibility(persisted.output);

    settings.beginGroup(QStringLiteral("audio"));
    LoadAudioStateFromCurrentGroup(settings, &persisted.audio_ui_state);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("video"));
    LoadVideoSettingsFromCurrentGroup(settings, &persisted.video);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("webcam"));
    LoadWebcamSettingsFromCurrentGroup(settings, &persisted.webcam);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(persisted.hotkey_bindings.size()); ++i) {
        const QString key = QStringLiteral("binding_%1").arg(i);
        if (settings.contains(key)) {
            persisted.hotkey_bindings[static_cast<std::size_t>(i)] = settings.value(key).toString().trimmed();
        }
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("profiles"));
    {
        const QString active_id = settings.value(QStringLiteral("active_id")).toString().trimmed();
        if (!active_id.isEmpty()) {
            persisted.active_profile.active_profile_id = active_id.toStdString();
        }
        persisted.active_profile.active_profile_modified =
            settings.value(QStringLiteral("active_modified"), false).toBool();

        const int user_count = settings.value(QStringLiteral("user_count"), 0).toInt();
        for (int i = 0; i < user_count; ++i) {
            settings.beginGroup(QStringLiteral("user_%1").arg(i));
            RecordingProfile profile = LoadProfileFromCurrentGroup(settings, RecordingProfileSource::User);
            settings.endGroup();
            if (!profile.id.empty()) {
                persisted.user_profiles.push_back(std::move(profile));
            }
        }

        const int modified_count = settings.value(QStringLiteral("modified_builtin_count"), 0).toInt();
        for (int i = 0; i < modified_count; ++i) {
            settings.beginGroup(QStringLiteral("modified_builtin_%1").arg(i));
            RecordingProfile profile = LoadProfileFromCurrentGroup(settings, RecordingProfileSource::BuiltIn);
            settings.endGroup();
            if (!profile.id.empty()) {
                persisted.modified_builtin_profiles.push_back(std::move(profile));
            }
        }
    }
    settings.endGroup();

    if (settings_version < 2) {
        persisted.audio_ui_state.mic_gain_linear = 1.0f;
    }

    const bool active_is_user = std::any_of(
        persisted.user_profiles.begin(), persisted.user_profiles.end(),
        [&persisted](const RecordingProfile& p) { return p.id == persisted.active_profile.active_profile_id; });
    const bool active_has_modified_builtin = std::any_of(
        persisted.modified_builtin_profiles.begin(), persisted.modified_builtin_profiles.end(),
        [&persisted](const RecordingProfile& p) { return p.id == persisted.active_profile.active_profile_id; });

    if (!active_is_user && IsBuiltInProfileId(persisted.active_profile.active_profile_id) &&
        !active_has_modified_builtin) {
        const auto builtin = FindBuiltInRecordingProfile(persisted.active_profile.active_profile_id);
        if (builtin.has_value() && !ProfileMatchesSettings(*builtin, persisted)) {
            RecordingProfile modified = *builtin;
            modified.output = persisted.output;
            modified.video = persisted.video;
            modified.audio_ui_state = persisted.audio_ui_state;
            modified.base_builtin_id = persisted.active_profile.active_profile_id;
            persisted.modified_builtin_profiles.push_back(std::move(modified));
            persisted.active_profile.active_profile_modified = true;
        } else if (settings_version < 4) {
            persisted.active_profile.active_profile_modified = false;
        }
    }

    ReconcileActiveSettingsFromProfile(&persisted);
    return persisted;
}

void AppSettingsStore::Save(const PersistedAppSettings& settings_snapshot) const {
    if (settings_path_.isEmpty()) {
        return;
    }

    const QFileInfo info(settings_path_);
    QDir().mkpath(info.absolutePath());

    QSettings settings(settings_path_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("settings_version"), kSettingsVersionCurrent);

    settings.beginGroup(QStringLiteral("output"));
    SaveOutputSettingsToCurrentGroup(settings, settings_snapshot.output);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("video"));
    SaveVideoSettingsToCurrentGroup(settings, settings_snapshot.video);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    SaveAudioStateToCurrentGroup(settings, settings_snapshot.audio_ui_state);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("webcam"));
    SaveWebcamSettingsToCurrentGroup(settings, settings_snapshot.webcam);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("hotkeys"));
    for (int i = 0; i < static_cast<int>(settings_snapshot.hotkey_bindings.size()); ++i) {
        settings.setValue(QStringLiteral("binding_%1").arg(i),
                          settings_snapshot.hotkey_bindings[static_cast<std::size_t>(i)]);
    }
    settings.endGroup();

    settings.remove(QStringLiteral("profiles"));
    settings.beginGroup(QStringLiteral("profiles"));
    settings.setValue(QStringLiteral("active_id"),
                      QString::fromStdString(settings_snapshot.active_profile.active_profile_id));
    settings.setValue(QStringLiteral("active_modified"), settings_snapshot.active_profile.active_profile_modified);

    settings.setValue(QStringLiteral("user_count"), static_cast<int>(settings_snapshot.user_profiles.size()));
    for (int i = 0; i < static_cast<int>(settings_snapshot.user_profiles.size()); ++i) {
        settings.beginGroup(QStringLiteral("user_%1").arg(i));
        SaveProfileToCurrentGroup(settings, settings_snapshot.user_profiles[static_cast<std::size_t>(i)]);
        settings.endGroup();
    }

    settings.setValue(QStringLiteral("modified_builtin_count"),
                      static_cast<int>(settings_snapshot.modified_builtin_profiles.size()));
    for (int i = 0; i < static_cast<int>(settings_snapshot.modified_builtin_profiles.size()); ++i) {
        settings.beginGroup(QStringLiteral("modified_builtin_%1").arg(i));
        SaveProfileToCurrentGroup(settings, settings_snapshot.modified_builtin_profiles[static_cast<std::size_t>(i)]);
        settings.endGroup();
    }
    settings.endGroup();

    settings.sync();
}

const QString& AppSettingsStore::SettingsFilePath() const {
    return settings_path_;
}

} // namespace exosnap
