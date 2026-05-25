#include "ProfileExchange.h"

#include "../models/OutputPathPolicy.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringView>

#include <optional>

namespace exosnap {
namespace {

constexpr const char* kSchemaName = "exosnap.recording_profiles";
constexpr int kSchemaVersion = 1;

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
    if (normalized == QStringLiteral("mkv"))
        return capability::Container::Matroska;
    if (normalized == QStringLiteral("mp4"))
        return capability::Container::Mp4;
    if (normalized == QStringLiteral("webm"))
        return capability::Container::WebM;
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
    if (normalized == QStringLiteral("h264"))
        return capability::VideoCodec::H264Nvenc;
    if (normalized == QStringLiteral("hevc"))
        return capability::VideoCodec::HevcNvenc;
    if (normalized == QStringLiteral("av1"))
        return capability::VideoCodec::Av1Nvenc;
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
    if (normalized == QStringLiteral("aac"))
        return capability::AudioCodec::AacMf;
    if (normalized == QStringLiteral("opus"))
        return capability::AudioCodec::Opus;
    if (normalized == QStringLiteral("pcm"))
        return capability::AudioCodec::Pcm;
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
    if (normalized == QStringLiteral("window"))
        return capability::CaptureTargetKind::Window;
    if (normalized == QStringLiteral("display"))
        return capability::CaptureTargetKind::Display;
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
    if (normalized == QStringLiteral("auto"))
        return recorder_core::MicChannelMode::Auto;
    if (normalized == QStringLiteral("preserve"))
        return recorder_core::MicChannelMode::PreserveStereo;
    if (normalized == QStringLiteral("mono_mix"))
        return recorder_core::MicChannelMode::MonoMix;
    if (normalized == QStringLiteral("left"))
        return recorder_core::MicChannelMode::LeftToStereo;
    if (normalized == QStringLiteral("right"))
        return recorder_core::MicChannelMode::RightToStereo;
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
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("app"))
        return recorder_core::AudioSourceKind::App;
    if (normalized == QStringLiteral("mic"))
        return recorder_core::AudioSourceKind::Mic;
    if (normalized == QStringLiteral("sys"))
        return recorder_core::AudioSourceKind::Sys;
    if (normalized == QStringLiteral("system_output"))
        return recorder_core::AudioSourceKind::SystemOutput;
    return std::nullopt;
}

void EnforceContainerCodecCompatibility(OutputSettingsModel* output) {
    if (output->container == capability::Container::Mp4) {
        output->video_codec = capability::VideoCodec::H264Nvenc;
        output->audio_codec = capability::AudioCodec::AacMf;
    } else if (output->container == capability::Container::WebM) {
        output->video_codec = capability::VideoCodec::Av1Nvenc;
        output->audio_codec = capability::AudioCodec::Opus;
    } else if (output->container == capability::Container::Matroska) {
        if (output->video_codec == capability::VideoCodec::HevcNvenc) {
            output->video_codec = capability::VideoCodec::H264Nvenc;
        }
        if (output->video_codec == capability::VideoCodec::H264Nvenc &&
            output->audio_codec == capability::AudioCodec::Opus) {
            output->audio_codec = capability::AudioCodec::AacMf;
        }
        if (output->audio_codec == capability::AudioCodec::Pcm) {
            output->audio_codec = capability::AudioCodec::AacMf;
        }
    }
}

QJsonObject SerializeOutput(const OutputSettingsModel& output) {
    QJsonObject object;
    object.insert(QStringLiteral("folder"), QString::fromStdWString(output.output_folder.wstring()));
    object.insert(QStringLiteral("naming_pattern"), QString::fromStdWString(output.naming_pattern));
    object.insert(QStringLiteral("container"), ContainerToString(output.container));
    object.insert(QStringLiteral("video_codec"), VideoCodecToString(output.video_codec));
    object.insert(QStringLiteral("audio_codec"), AudioCodecToString(output.audio_codec));
    return object;
}

OutputSettingsModel DeserializeOutput(const QJsonObject& object) {
    OutputSettingsModel output = OutputSettingsModel::Defaults();

    const QString folder = object.value(QStringLiteral("folder")).toString().trimmed();
    if (!folder.isEmpty()) {
        const auto normalized = NormalizeOutputFolderInput(folder.toStdWString());
        if (normalized.result == OutputFolderPolicyResult::Ok) {
            output.output_folder = normalized.resolved_path;
        }
    }

    const QString naming_pattern = object.value(QStringLiteral("naming_pattern")).toString();
    if (!naming_pattern.isEmpty()) {
        const auto normalized = NormalizeFilenamePatternInput(naming_pattern.toStdWString());
        if (normalized.result == FilenamePatternPolicyResult::Ok) {
            output.naming_pattern = normalized.normalized_pattern;
        }
    }

    if (const auto container = ContainerFromString(object.value(QStringLiteral("container")).toString());
        container.has_value()) {
        output.container = *container;
    }
    if (const auto video = VideoCodecFromString(object.value(QStringLiteral("video_codec")).toString());
        video.has_value()) {
        output.video_codec = *video;
    }
    if (const auto audio = AudioCodecFromString(object.value(QStringLiteral("audio_codec")).toString());
        audio.has_value()) {
        output.audio_codec = *audio;
    }

    EnforceContainerCodecCompatibility(&output);
    return output;
}

QJsonObject SerializeVideo(const VideoSettingsModel& video) {
    QJsonObject object;
    object.insert(QStringLiteral("quality"), NvencQualityPresetToString(video.quality));
    object.insert(QStringLiteral("cfr"), video.cfr);
    object.insert(QStringLiteral("capture_cursor"), video.capture_cursor);
    return object;
}

VideoSettingsModel DeserializeVideo(const QJsonObject& object) {
    VideoSettingsModel video = VideoSettingsModel::Defaults();
    if (const auto quality = NvencQualityPresetFromString(object.value(QStringLiteral("quality")).toString());
        quality.has_value()) {
        video.quality = *quality;
    }
    if (object.contains(QStringLiteral("cfr"))) {
        video.cfr = object.value(QStringLiteral("cfr")).toBool(video.cfr);
    }
    if (object.contains(QStringLiteral("capture_cursor"))) {
        video.capture_cursor = object.value(QStringLiteral("capture_cursor")).toBool(video.capture_cursor);
    }
    return video;
}

QJsonObject SerializeAudio(const capability::AudioUiState& audio) {
    QJsonObject object;
    object.insert(QStringLiteral("target_kind"), CaptureTargetKindToString(audio.target_kind));
    object.insert(QStringLiteral("mic_channel_mode"), MicChannelModeToString(audio.mic_channel_mode));
    object.insert(QStringLiteral("mic_gain_linear"), audio.mic_gain_linear);
    object.insert(QStringLiteral("selected_mic_device_id"), audio.selected_mic_device_id.has_value()
                                                                ? QString::fromStdString(*audio.selected_mic_device_id)
                                                                : QString());

    QJsonArray rows;
    for (const auto& row : audio.source_rows) {
        QJsonObject row_obj;
        row_obj.insert(QStringLiteral("kind"), AudioSourceKindToString(row.kind));
        row_obj.insert(QStringLiteral("enabled"), row.enabled);
        row_obj.insert(QStringLiteral("merge_with_above"), row.merge_with_above);
        rows.append(row_obj);
    }
    object.insert(QStringLiteral("source_rows"), rows);
    return object;
}

capability::AudioUiState DeserializeAudio(const QJsonObject& object) {
    capability::AudioUiState audio{};
    if (const auto target_kind = CaptureTargetKindFromString(object.value(QStringLiteral("target_kind")).toString());
        target_kind.has_value()) {
        audio.target_kind = *target_kind;
    }
    if (const auto mic_mode = MicChannelModeFromString(object.value(QStringLiteral("mic_channel_mode")).toString());
        mic_mode.has_value()) {
        audio.mic_channel_mode = *mic_mode;
    }
    if (object.contains(QStringLiteral("mic_gain_linear"))) {
        audio.mic_gain_linear = static_cast<float>(object.value(QStringLiteral("mic_gain_linear")).toDouble(1.0));
    }
    const QString selected_mic = object.value(QStringLiteral("selected_mic_device_id")).toString().trimmed();
    if (!selected_mic.isEmpty()) {
        audio.selected_mic_device_id = selected_mic.toStdString();
    }

    audio.source_rows.clear();
    const QJsonArray rows = object.value(QStringLiteral("source_rows")).toArray();
    for (const QJsonValue& row_value : rows) {
        const QJsonObject row_obj = row_value.toObject();
        const auto kind = AudioSourceKindFromString(row_obj.value(QStringLiteral("kind")).toString());
        if (!kind.has_value()) {
            continue;
        }
        recorder_core::AudioSourceRow row;
        row.kind = *kind;
        row.enabled = row_obj.value(QStringLiteral("enabled")).toBool(true);
        row.merge_with_above = row_obj.value(QStringLiteral("merge_with_above")).toBool(false);
        audio.source_rows.push_back(row);
    }
    return audio;
}

QJsonObject SerializeProfile(const RecordingProfile& profile) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::fromStdString(profile.id));
    object.insert(QStringLiteral("name"), QString::fromStdString(profile.name));
    object.insert(QStringLiteral("base_builtin_id"),
                  profile.base_builtin_id.has_value() ? QString::fromStdString(*profile.base_builtin_id) : QString());
    object.insert(QStringLiteral("output"), SerializeOutput(profile.output));
    object.insert(QStringLiteral("video"), SerializeVideo(profile.video));
    object.insert(QStringLiteral("audio"), SerializeAudio(profile.audio_ui_state));
    return object;
}

std::optional<RecordingProfile> DeserializeProfile(const QJsonObject& object) {
    const QString name = object.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return std::nullopt;
    }

    RecordingProfile profile = MakeSafeDefaultUserProfile();
    profile.source = RecordingProfileSource::User;
    profile.name = name.toStdString();
    profile.id = object.value(QStringLiteral("id")).toString().trimmed().toStdString();

    const QString base_builtin_id = object.value(QStringLiteral("base_builtin_id")).toString().trimmed();
    if (!base_builtin_id.isEmpty()) {
        profile.base_builtin_id = base_builtin_id.toStdString();
    } else {
        profile.base_builtin_id = std::nullopt;
    }

    profile.output = DeserializeOutput(object.value(QStringLiteral("output")).toObject());
    profile.video = DeserializeVideo(object.value(QStringLiteral("video")).toObject());
    profile.audio_ui_state = DeserializeAudio(object.value(QStringLiteral("audio")).toObject());
    return profile;
}

} // namespace

bool ExportProfilesToJsonFile(const QString& file_path, const std::vector<RecordingProfile>& profiles,
                              QString* out_error_message) {
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (out_error_message) {
            *out_error_message = QStringLiteral("Could not open file for writing.");
        }
        return false;
    }

    QJsonArray profiles_json;
    for (const auto& profile : profiles) {
        profiles_json.append(SerializeProfile(profile));
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(kSchemaName));
    root.insert(QStringLiteral("version"), kSchemaVersion);
    root.insert(QStringLiteral("profiles"), profiles_json);

    const QJsonDocument doc(root);
    const qint64 written = file.write(doc.toJson(QJsonDocument::Indented));
    if (written <= 0) {
        if (out_error_message) {
            *out_error_message = QStringLiteral("Could not write profile export file.");
        }
        return false;
    }
    return true;
}

ProfileImportResult ImportProfilesFromJsonFile(const QString& file_path) {
    ProfileImportResult result;
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error_message = QStringLiteral("Could not open file.");
        return result;
    }

    const QByteArray bytes = file.readAll();
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error_message = QStringLiteral("Invalid profile file format.");
        return result;
    }

    const QJsonObject root = doc.object();
    const QString schema = root.value(QStringLiteral("schema")).toString();
    if (schema != QString::fromLatin1(kSchemaName)) {
        result.error_message = QStringLiteral("Unsupported profile file schema.");
        return result;
    }

    const QJsonArray profiles_json = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& value : profiles_json) {
        const auto profile = DeserializeProfile(value.toObject());
        if (profile.has_value()) {
            result.profiles.push_back(*profile);
        }
    }

    if (result.profiles.empty()) {
        result.error_message = QStringLiteral("No valid profiles found in file.");
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace exosnap
