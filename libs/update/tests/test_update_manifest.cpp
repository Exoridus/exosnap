// test_update_manifest.cpp -- manifest JSON parse tests.

#include <gtest/gtest.h>
#include <update/manifest_io.h>
#include <update/update_types.h>

using namespace exosnap::update;

static const std::string kValidManifest = R"({
    "version": "1.2.3",
    "minimum_accepted_version": "1.0.0",
    "packages": [
        {
            "kind": "installer",
            "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/exosnap-1.2.3-setup.exe",
            "sha256": "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"
        },
        {
            "kind": "portable",
            "url": "https://github.com/Exoridus/exosnap/releases/download/v1.2.3/exosnap-1.2.3-portable.zip",
            "sha256": "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
        }
    ],
    "signature": ")" + std::string(128, '0') +
                                          R"("
})";

TEST(ManifestParse, ValidManifest) {
    auto result = ParseManifest(kValidManifest);
    ASSERT_TRUE(std::holds_alternative<UpdateManifest>(result));
    const auto& m = std::get<UpdateManifest>(result);
    EXPECT_EQ(m.version, (SemVer{1, 2, 3}));
    EXPECT_EQ(m.minimum_accepted_version, (SemVer{1, 0, 0}));
    ASSERT_EQ(m.packages.size(), 2u);
    EXPECT_EQ(m.packages[0].kind, PackageKind::Installer);
    EXPECT_EQ(m.packages[1].kind, PackageKind::Portable);
}

TEST(ManifestParse, MissingVersionField) {
    const std::string json = R"({
        "minimum_accepted_version": "1.0.0",
        "packages": [],
        "signature": ")" + std::string(128, '0') +
                             R"("
    })";
    auto result = ParseManifest(json);
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}

TEST(ManifestParse, InvalidVersionString) {
    const std::string json = R"({
        "version": "not-a-version",
        "minimum_accepted_version": "1.0.0",
        "packages": [],
        "signature": ")" + std::string(128, '0') +
                             R"("
    })";
    auto result = ParseManifest(json);
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}

TEST(ManifestParse, UnknownPackageKindFails) {
    const std::string json = R"({
        "version": "1.0.0",
        "minimum_accepted_version": "1.0.0",
        "packages": [{"kind":"dvd","url":"x","sha256":")" +
                             std::string(64, 'a') + R"("}],
        "signature": ")" + std::string(128, '0') +
                             R"("
    })";
    auto result = ParseManifest(json);
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}

TEST(ManifestParse, ShortSha256Fails) {
    const std::string json = R"({
        "version": "1.0.0",
        "minimum_accepted_version": "1.0.0",
        "packages": [{"kind":"installer","url":"x","sha256":"abc"}],
        "signature": ")" + std::string(128, '0') +
                             R"("
    })";
    auto result = ParseManifest(json);
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}

TEST(ManifestParse, InvalidSignatureHexFails) {
    const std::string json = R"({
        "version": "1.0.0",
        "minimum_accepted_version": "1.0.0",
        "packages": [],
        "signature": "ZZZZ"
    })";
    auto result = ParseManifest(json);
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}

TEST(ManifestParse, EmptyJsonFails) {
    auto result = ParseManifest("{}");
    EXPECT_TRUE(std::holds_alternative<std::string>(result));
}
