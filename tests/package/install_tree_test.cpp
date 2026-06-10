#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The install tree is located in priority order:
//   1. EXOSNAP_INSTALL_TREE environment variable (lets `ctest` validate a freshly
//      staged tree without reconfiguring the build).
//   2. EXOSNAP_INSTALL_TREE compile definition (set by CI at configure time).
// When neither resolves to a directory the tests skip.
fs::path install_root() {
    if (const char* env = std::getenv("EXOSNAP_INSTALL_TREE")) {
        fs::path p{env};
        if (!std::string(env).empty() && fs::exists(p) && fs::is_directory(p))
            return p;
    }
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

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
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
// Presence — required runtime files (flat portable layout: exe + Qt DLLs +
// qt.conf live at the package root so the executable is directly launchable)
// ---------------------------------------------------------------------------
TEST_F(InstallTreeTest, ExosnapExeExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "exosnap.exe")) << "Missing exosnap.exe in install tree";
}

TEST_F(InstallTreeTest, QtConfAtRoot) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "qt.conf")) << "Missing qt.conf next to the executable (flat deploy layout)";
}

TEST_F(InstallTreeTest, QtRuntimeDllsPresentAtRoot) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "Qt6Core.dll")) << "Missing Qt6Core.dll next to the executable";
    EXPECT_TRUE(file_exists(_root, "Qt6Gui.dll")) << "Missing Qt6Gui.dll next to the executable";
    EXPECT_TRUE(file_exists(_root, "Qt6Widgets.dll")) << "Missing Qt6Widgets.dll next to the executable";
}

TEST_F(InstallTreeTest, QtPluginsDirExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(dir_exists(_root, "plugins/platforms")) << "Missing Qt platforms plugin directory";
}

TEST_F(InstallTreeTest, RootLicenseExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "LICENSE")) << "Missing root LICENSE";
}

TEST_F(InstallTreeTest, ThirdPartyNoticesExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "THIRD_PARTY_NOTICES.md")) << "Missing THIRD_PARTY_NOTICES.md";
}

TEST_F(InstallTreeTest, KnownLimitationsExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "KNOWN_LIMITATIONS.md")) << "Missing KNOWN_LIMITATIONS.md portable doc";
}

TEST_F(InstallTreeTest, PortableReadmeExists) {
    SKIP_IF_NO_TREE();
    EXPECT_TRUE(file_exists(_root, "README-PORTABLE.md")) << "Missing README-PORTABLE.md portable doc";
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

// Forbidden file extensions — static/import libs, symbols, intermediates, source.
TEST_F(InstallTreeTest, NoForbiddenFileExtensions) {
    SKIP_IF_NO_TREE();
    const std::vector<std::string> forbidden = {".lib", ".pdb", ".ilk", ".exp",   ".obj", ".pch", ".h",
                                                ".hpp", ".cpp", ".c",   ".cmake", ".pc",  ".log", ".tmp"};
    for (const auto& p : relative_paths(_root)) {
        auto ext = lower(fs::path(p).extension().string());
        EXPECT_EQ(std::find(forbidden.begin(), forbidden.end(), ext), forbidden.end())
            << "Forbidden development file leaked into install tree: " << p;
    }
}

// User data must never ship in the artifact.
TEST_F(InstallTreeTest, NoUserDataFiles) {
    SKIP_IF_NO_TREE();
    const std::vector<std::string> forbidden_names = {"settings.ini", "presets.ini", "recording-history.json"};
    for (const auto& p : relative_paths(_root)) {
        auto name = lower(fs::path(p).filename().string());
        EXPECT_EQ(std::find(forbidden_names.begin(), forbidden_names.end(), name), forbidden_names.end())
            << "User-data file leaked into install tree: " << p;
    }
}

// No Debug Qt DLLs (e.g. Qt6Cored.dll) may ship in a Release artifact.
TEST_F(InstallTreeTest, NoDebugQtDlls) {
    SKIP_IF_NO_TREE();
    for (const auto& p : relative_paths(_root)) {
        auto name = lower(fs::path(p).filename().string());
        bool is_debug_qt = name.rfind("qt6", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == "d.dll";
        EXPECT_FALSE(is_debug_qt) << "Debug Qt DLL leaked into Release install tree: " << p;
    }
}

// The stale plain-text limitations file must not reappear.
TEST_F(InstallTreeTest, NoStaleKnownLimitationsTxt) {
    SKIP_IF_NO_TREE();
    EXPECT_FALSE(file_exists(_root, "KNOWN_LIMITATIONS.txt")) << "Stale KNOWN_LIMITATIONS.txt must not ship";
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
