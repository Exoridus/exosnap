#include "AppSettingsStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringView>

#include <optional>
#include <utility>

namespace exosnap {
namespace {

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

QString AudioCodecToString(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("aac");
    case capability::AudioCodec::Opus:
    case capability::AudioCodec::Pcm:
        return QStringLiteral("opus");
    }
    return QStringLiteral("opus");
}

std::optional<capability::AudioCodec> AudioCodecFromString(QStringView value) {
    const QString normalized = value.trimmed().toString().toLower();
    if (normalized == QStringLiteral("opus")) {
        return capability::AudioCodec::Opus;
    }
    if (normalized == QStringLiteral("aac")) {
        return capability::AudioCodec::AacMf;
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
    persisted.audio_ui_state = capability::AudioUiState{};

    if (settings_path_.isEmpty()) {
        return persisted;
    }

    QSettings settings(settings_path_, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("output"));
    const QString folder = settings.value(QStringLiteral("folder")).toString().trimmed();
    if (!folder.isEmpty()) {
        persisted.output.output_folder = std::filesystem::path(folder.toStdWString());
    }

    const QString naming_pattern = settings.value(QStringLiteral("naming_pattern")).toString();
    if (!naming_pattern.isEmpty()) {
        persisted.output.naming_pattern = naming_pattern.toStdWString();
    }

    if (const auto container = ContainerFromString(settings.value(QStringLiteral("container")).toString());
        container.has_value()) {
        persisted.output.container = *container;
    }

    if (const auto audio_codec = AudioCodecFromString(settings.value(QStringLiteral("audio_codec")).toString());
        audio_codec.has_value()) {
        persisted.output.audio_codec = *audio_codec;
    }
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    if (settings.contains(QStringLiteral("record_application_audio"))) {
        persisted.audio_ui_state.record_application_audio =
            settings.value(QStringLiteral("record_application_audio")).toBool();
    }
    if (settings.contains(QStringLiteral("record_system_audio"))) {
        persisted.audio_ui_state.record_system_audio = settings.value(QStringLiteral("record_system_audio")).toBool();
    }
    if (settings.contains(QStringLiteral("separate_output_tracks"))) {
        persisted.audio_ui_state.separate_output_tracks =
            settings.value(QStringLiteral("separate_output_tracks")).toBool();
    }
    if (settings.contains(QStringLiteral("record_microphone"))) {
        persisted.audio_ui_state.record_microphone = settings.value(QStringLiteral("record_microphone")).toBool();
    }
    if (const auto mode = MicChannelModeFromString(settings.value(QStringLiteral("mic_channel_mode")).toString());
        mode.has_value()) {
        persisted.audio_ui_state.mic_channel_mode = *mode;
    }

    const QString selected_mic_device_id =
        settings.value(QStringLiteral("selected_mic_device_id")).toString().trimmed();
    if (selected_mic_device_id.isEmpty()) {
        persisted.audio_ui_state.selected_mic_device_id = std::nullopt;
    } else {
        persisted.audio_ui_state.selected_mic_device_id = selected_mic_device_id.toStdString();
    }
    settings.endGroup();

    return persisted;
}

void AppSettingsStore::Save(const PersistedAppSettings& settings_snapshot) const {
    if (settings_path_.isEmpty()) {
        return;
    }

    const QFileInfo info(settings_path_);
    QDir().mkpath(info.absolutePath());

    QSettings settings(settings_path_, QSettings::IniFormat);

    settings.beginGroup(QStringLiteral("output"));
    settings.setValue(QStringLiteral("folder"),
                      QString::fromStdWString(settings_snapshot.output.output_folder.wstring()));
    settings.setValue(QStringLiteral("naming_pattern"),
                      QString::fromStdWString(settings_snapshot.output.naming_pattern));
    settings.setValue(QStringLiteral("container"), ContainerToString(settings_snapshot.output.container));
    settings.setValue(QStringLiteral("audio_codec"), AudioCodecToString(settings_snapshot.output.audio_codec));
    settings.endGroup();

    settings.beginGroup(QStringLiteral("audio"));
    settings.setValue(QStringLiteral("record_application_audio"),
                      settings_snapshot.audio_ui_state.record_application_audio);
    settings.setValue(QStringLiteral("record_system_audio"), settings_snapshot.audio_ui_state.record_system_audio);
    settings.setValue(QStringLiteral("separate_output_tracks"),
                      settings_snapshot.audio_ui_state.separate_output_tracks);
    settings.setValue(QStringLiteral("record_microphone"), settings_snapshot.audio_ui_state.record_microphone);
    settings.setValue(QStringLiteral("mic_channel_mode"),
                      MicChannelModeToString(settings_snapshot.audio_ui_state.mic_channel_mode));
    settings.setValue(QStringLiteral("selected_mic_device_id"),
                      settings_snapshot.audio_ui_state.selected_mic_device_id.has_value()
                          ? QString::fromStdString(*settings_snapshot.audio_ui_state.selected_mic_device_id)
                          : QString());
    settings.endGroup();

    settings.sync();
}

QString AppSettingsStore::SettingsFilePath() const {
    return settings_path_;
}

} // namespace exosnap
