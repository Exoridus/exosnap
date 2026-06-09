#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// EXOSNAP_INSTALL_TREE is set by CMake to the canonical install prefix
// (e.g. during CI).  When unset the test searches a default location
// relative to the build tree and skips if neither is present.
fs::path install_root() {
#ifdef EXOSNAP_INSTALL_TREE
    fs::path p{EXOSNAP_INSTALL_TREE};
    if (fs::exists(p) && fs::is_directory(p))
        return p;
#endif
    return {};
}

bool dir_exists(const fs::path& root, const std::string& rel) {
    auto p = root / rel;
    return fs::exists(p) && fs::is_directory(p);
}

bool file_exists(const fs::path& root, const std::string& rel) {
    auto p = root / rel;
    return fs::exists(p) && fs::is_regular_file(p);
}

// Helper to collect all relative file paths under root.
std::vector<std::string> relative_paths(const fs::path& root) {
    std::vector<std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        result.push_back(fs::relative(entry.path(), root).string());
    }
    return result;
}

// Join paths with newlines for substring searches.
std::string join(const std::vector<std::string>& paths) {
    std::string result;
    for (const auto& p : paths) {
        result += p + "\n";
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Skip if no install tree is available
// ---------------------------------------------------------------------------
class InstallTreeTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        _root = install_root();
    }
    static fs::path _root;
};
fs::path InstallTreeTest::_root;

#define SKIP_IF_NO_TREE()                                                                                              \
    do {                                                                                                               \
        if (_root.empty())                                                                                             \
            GTEST_SKIP() << "No install tree available";                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// Presence — required runtime files
// ---------------------------------------------------------------------------
TEST_F(InstallTreeTest, ExosnapExeExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "exosnap.exe")) << "Missing exosnap.exe in install tree";
}

TEST_F(InstallTreeTest, RootLicenseExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "LICENSE")) << "Missing root LICENSE";
}

TEST_F(InstallTreeTest, ThirdPartyNoticesExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "THIRD_PARTY_NOTICES.md")) << "Missing THIRD_PARTY_NOTICES.md";
}

TEST_F(InstallTreeTest, LicensesDirExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(dir_exists(_root, "licenses")) << "Missing licenses/ directory";
}

TEST_F(InstallTreeTest, AllExpectedLicensesPresent) {
    SKIP_IF_NO_TREE();
    const std::vector<std::string> expected = {
        "licenses/fdk-aac.txt", "licenses/libebml.txt", "licenses/libmatroska.txt", "licenses/nlohmann_json.txt",
        "licenses/opus.txt",    "licenses/qt.txt",      "licenses/spdlog.txt",      "licenses/tomlplusplus.txt",
    };
    for (const auto& lic : expected) {
        EXPECT_TRUE(file_exists(_root, lic)) << "Missing license: " << lic;
    }
}

TEST_F(InstallTreeTest, QtPluginsDirExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(dir_exists(_root, "plugins/platforms")) << "Missing Qt platforms plugin directory";
}

TEST_F(InstallTreeTest, QtRuntimeDllsPresent) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "bin/Qt6Core.dll")) << "Missing Qt6Core.dll";
    EXPECT_TRUE(file_exists(_root, "bin/Qt6Gui.dll")) << "Missing Qt6Gui.dll";
}

// ---------------------------------------------------------------------------
// Absence — development files must NOT leak
// ---------------------------------------------------------------------------
TEST_F(InstallTreeTest, NoIncludeDir) {
    SKIP_IF_NO_TREE();
    EXPECT_FALSE(dir_exists(_root, "include")) << "include/ must not appear in the runtime install tree";
}

TEST_F(InstallTreeTest, NoLibDir) {
    SKIP_IF_NO_TREE();
    EXPECT_FALSE(dir_exists(_root, "lib")) << "lib/ must not appear in the runtime install tree";
}

TEST_F(InstallTreeTest, NoCmakeConfigs) {
    SKIP_IF_NO_TREE();
    auto cmake_dir = _root / "lib" / "cmake";
    EXPECT_FALSE(fs::exists(cmake_dir)) << "lib/cmake/ must not appear in the runtime install tree";
}

TEST_F(InstallTreeTest, NoPkgConfigFiles) {
    SKIP_IF_NO_TREE();
    auto pc_dir = _root / "lib" / "pkgconfig";
    EXPECT_FALSE(fs::exists(pc_dir)) << "lib/pkgconfig/ must not appear in the runtime install tree";
}

TEST_F(InstallTreeTest, NoStaticLibs) {
    SKIP_IF_NO_TREE();
    for (const auto& p : relative_paths(_root)) {
        auto ext = fs::path(p).extension().string();
        EXPECT_NE(ext, ".lib") << "No .lib files (static/import libraries) may appear in the install tree, found: "
                               << p;
    }
}

TEST_F(InstallTreeTest, NoHeaderFiles) {
    SKIP_IF_NO_TREE();
    for (const auto& p : relative_paths(_root)) {
        auto ext = fs::path(p).extension().string();
        EXPECT_NE(ext, ".h") << "No .h header files may appear in the install tree, found: " << p;
        EXPECT_NE(ext, ".hpp") << "No .hpp header files may appear in the install tree, found: " << p;
    }
}

TEST_F(InstallTreeTest, NoWorkspaceLeak) {
    SKIP_IF_NO_TREE();
    auto paths = join(relative_paths(_root));
    EXPECT_EQ(paths.find(".workspace"), std::string::npos) << ".workspace must not appear in install tree paths";
}

TEST_F(InstallTreeTest, NoClaudeLeak) {
    SKIP_IF_NO_TREE();
    auto paths = join(relative_paths(_root));
    EXPECT_EQ(paths.find(".claude"), std::string::npos) << ".claude must not appear in install tree paths";
}

TEST_F(InstallTreeTest, NoAbsoluteBuildPaths) {
    SKIP_IF_NO_TREE();
    auto paths = join(relative_paths(_root));
    EXPECT_EQ(paths.find("C:"), std::string::npos) << "No absolute Windows paths may appear in install tree paths";
}

TEST_F(InstallTreeTest, NoSourceFiles) {
    SKIP_IF_NO_TREE();
    for (const auto& p : relative_paths(_root)) {
        auto ext = fs::path(p).extension().string();
        EXPECT_NE(ext, ".cpp") << "No .cpp source files may appear in the install tree, found: " << p;
    }
}
