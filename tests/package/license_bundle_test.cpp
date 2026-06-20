#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    if (!in)
        return {};
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

bool file_exists(const fs::path& p) {
    return fs::exists(p) && fs::is_regular_file(p);
}

std::string lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::vector<std::string> lines_in(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

// Root paths relative to the project source dir.
fs::path project_root() {
    // EXOSNAP_PROJECT_SOURCE_DIR is defined by the CMake build.
    return fs::path{EXOSNAP_PROJECT_SOURCE_DIR};
}

const std::vector<std::string> k_expected_licenses = {
    "spdlog.txt", "nlohmann_json.txt", "tomlplusplus.txt", "opus.txt", "fdk-aac.txt",
    "flac.txt",   "libebml.txt",       "libmatroska.txt",  "qt.txt",   "ffmpeg.txt",
};

const std::vector<std::string> k_expected_notices_sections = {
    "spdlog", "nlohmann/json", "toml++",     "Opus",       "FDK-AAC",        "FLAC", "libebml", "libmatroska",
    "Qt",     "FFmpeg",        "GoogleTest", "Build-only", "System/runtime",
};

} // namespace

// ---------------------------------------------------------------------------
// 1. Source-tree root license files exist
// ---------------------------------------------------------------------------
TEST(LicenseBundle, RootLicenseExists) {
    EXPECT_TRUE(file_exists(project_root() / "LICENSE"));
}

TEST(LicenseBundle, ThirdPartyNoticesExists) {
    EXPECT_TRUE(file_exists(project_root() / "THIRD_PARTY_NOTICES.md"));
}

TEST(LicenseBundle, QtLicenseSourceExists) {
    EXPECT_TRUE(file_exists(project_root() / "third_party" / "qt" / "LGPL-3.0.txt"));
}

// ---------------------------------------------------------------------------
// 2. THIRD_PARTY_NOTICES.md structure
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoticesHasBundledDepsSection) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    EXPECT_NE(content.find("## Bundled dependencies"), std::string::npos);
}

TEST(LicenseBundle, NoticesHasBuildOnlySection) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    EXPECT_NE(content.find("## Build-only dependencies"), std::string::npos);
}

TEST(LicenseBundle, NoticesHasSystemSection) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    EXPECT_NE(content.find("## System/runtime dependencies"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. Notice entries reference existing bundled files
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoticesHasLicensePathRefs) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    for (const auto& lic : k_expected_licenses) {
        EXPECT_NE(content.find(lic), std::string::npos) << "THIRD_PARTY_NOTICES.md should reference " << lic;
    }
}

// ---------------------------------------------------------------------------
// 4. No duplicate normalized license filenames
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoDuplicateLicenseFilenames) {
    std::set<std::string> lower_names;
    for (const auto& lic : k_expected_licenses) {
        auto l = lower(lic);
        EXPECT_FALSE(lower_names.count(l)) << "Duplicate license filename (case-insensitive): " << lic;
        lower_names.insert(l);
    }
}

// ---------------------------------------------------------------------------
// 5. No .workspace or local paths in notices
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoticesHasNoWorkspaceRefs) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    EXPECT_EQ(content.find(".workspace"), std::string::npos) << "THIRD_PARTY_NOTICES.md must not reference .workspace";
    EXPECT_EQ(content.find("C:\\Users"), std::string::npos) << "THIRD_PARTY_NOTICES.md must not contain local paths";
    EXPECT_EQ(content.find("C:/Users"), std::string::npos) << "THIRD_PARTY_NOTICES.md must not contain local paths";
}

// ---------------------------------------------------------------------------
// 6. Root LICENSE has SPDX identifier
// ---------------------------------------------------------------------------
TEST(LicenseBundle, RootLicenseHasSPDX) {
    auto content = read_file(project_root() / "LICENSE");
    EXPECT_NE(content.find("SPDX-License-Identifier"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 7. Sections cover all key dependency names
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoticesCoversAllDeps) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    for (const auto& section : k_expected_notices_sections) {
        EXPECT_NE(content.find(section), std::string::npos) << "THIRD_PARTY_NOTICES.md should mention " << section;
    }
}

// ---------------------------------------------------------------------------
// 8. Notices include license identifiers (not paraphrased)
// ---------------------------------------------------------------------------
TEST(LicenseBundle, NoticesHasLicenseIdentifiers) {
    auto content = read_file(project_root() / "THIRD_PARTY_NOTICES.md");
    EXPECT_NE(content.find("MIT"), std::string::npos);
    EXPECT_NE(content.find("LGPL"), std::string::npos);
    EXPECT_NE(content.find("BSD"), std::string::npos);
}
