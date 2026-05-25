#include <gtest/gtest.h>

#include "../models/RecordingProfile.h"
#include "../models/RecordingProfileRegistry.h"
#include "../settings/ProfileExchange.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <chrono>
#include <filesystem>

namespace exosnap {
namespace {

std::filesystem::path UniqueTempJsonPath(const char* suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return std::filesystem::temp_directory_path() / std::filesystem::path(std::string("exosnap_profile_exchange_") +
                                                                          suffix + "_" + std::to_string(ns) + ".json");
}

RecordingProfile MakeSampleUserProfile() {
    RecordingProfile profile = MakeSafeDefaultUserProfile();
    profile.source = RecordingProfileSource::User;
    profile.id = "user.sample";
    profile.name = "Sample";
    profile.output.container = capability::Container::WebM;
    profile.output.video_codec = capability::VideoCodec::Av1Nvenc;
    profile.output.audio_codec = capability::AudioCodec::Opus;
    profile.output.naming_pattern = L"{profile}/{datetime}";
    profile.video.cfr = true;
    profile.video.capture_cursor = true;
    profile.audio_ui_state.source_rows = {
        {recorder_core::AudioSourceKind::App, true, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
        {recorder_core::AudioSourceKind::Sys, true, false},
    };
    return profile;
}

} // namespace

TEST(ProfileExchangeTest, ExportImportRoundTrip_ProfileDataPreserved) {
    const std::filesystem::path temp_path = UniqueTempJsonPath("roundtrip");
    const RecordingProfile profile = MakeSampleUserProfile();

    QString export_error;
    EXPECT_TRUE(ExportProfilesToJsonFile(QString::fromStdWString(temp_path.wstring()), {profile}, &export_error))
        << export_error.toStdString();

    const ProfileImportResult imported = ImportProfilesFromJsonFile(QString::fromStdWString(temp_path.wstring()));
    EXPECT_TRUE(imported.ok) << imported.error_message.toStdString();
    ASSERT_EQ(imported.profiles.size(), 1u);
    EXPECT_EQ(imported.profiles[0].name, "Sample");
    EXPECT_EQ(imported.profiles[0].output.container, capability::Container::WebM);
    EXPECT_EQ(imported.profiles[0].output.video_codec, capability::VideoCodec::Av1Nvenc);
    EXPECT_EQ(imported.profiles[0].output.audio_codec, capability::AudioCodec::Opus);
    EXPECT_EQ(imported.profiles[0].output.naming_pattern, L"{profile}/{datetime}");

    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
}

TEST(ProfileExchangeTest, ImportRejectsUnsupportedSchema) {
    const std::filesystem::path temp_path = UniqueTempJsonPath("bad_schema");
    QFile file(QString::fromStdWString(temp_path.wstring()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));

    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("wrong.schema"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("profiles"), QJsonArray{});
    const QJsonDocument doc(root);
    ASSERT_GT(file.write(doc.toJson(QJsonDocument::Indented)), 0);
    file.close();

    const ProfileImportResult imported = ImportProfilesFromJsonFile(QString::fromStdWString(temp_path.wstring()));
    EXPECT_FALSE(imported.ok);
    EXPECT_FALSE(imported.error_message.isEmpty());

    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
}

TEST(ProfileExchangeTest, RegistryImportCreatesUserProfilesWithUniqueNames) {
    RecordingProfile imported_a = MakeSampleUserProfile();
    imported_a.name = "Alpha";
    imported_a.id = "incoming.one";
    imported_a.source = RecordingProfileSource::BuiltIn;

    RecordingProfile imported_b = MakeSampleUserProfile();
    imported_b.name = "Alpha";
    imported_b.id = "incoming.two";
    imported_b.source = RecordingProfileSource::BuiltIn;

    RecordingProfileRegistry registry;
    const int count = registry.ImportUserProfiles({imported_a, imported_b});
    EXPECT_EQ(count, 2);
    ASSERT_EQ(registry.UserProfiles().size(), 2u);
    EXPECT_EQ(registry.UserProfiles()[0].source, RecordingProfileSource::User);
    EXPECT_EQ(registry.UserProfiles()[1].source, RecordingProfileSource::User);
    EXPECT_EQ(registry.UserProfiles()[0].name, "Alpha");
    EXPECT_EQ(registry.UserProfiles()[1].name, "Alpha (2)");
    EXPECT_NE(registry.UserProfiles()[0].id, registry.UserProfiles()[1].id);
}

} // namespace exosnap
