#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path project_root() {
    return fs::path{EXOSNAP_PROJECT_SOURCE_DIR};
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string known_limitations() {
    return read_file(project_root() / "KNOWN_LIMITATIONS.md");
}
std::string portable_readme() {
    return read_file(project_root() / "README-PORTABLE.md");
}
std::string root_readme() {
    return read_file(project_root() / "README.md");
}

} // namespace

// ---------------------------------------------------------------------------
// Files exist (and the stale .txt is gone)
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsMarkdownExists) {
    EXPECT_TRUE(fs::is_regular_file(project_root() / "KNOWN_LIMITATIONS.md"));
}

TEST(PortableDocs, PortableReadmeExists) {
    EXPECT_TRUE(fs::is_regular_file(project_root() / "README-PORTABLE.md"));
}

TEST(PortableDocs, StalePlainTextLimitationsRemoved) {
    EXPECT_FALSE(fs::exists(project_root() / "KNOWN_LIMITATIONS.txt"))
        << "the stale alpha KNOWN_LIMITATIONS.txt must be replaced by the .md";
}

// ---------------------------------------------------------------------------
// Version consistency — every release-facing doc names the canonical project
// version and none claims 1.0. EXOSNAP_PROJECT_VERSION is injected from
// PROJECT_VERSION at build time, so this self-updates on a version bump: the
// only edit a release needs is the canonical project(... VERSION ...).
// ---------------------------------------------------------------------------
TEST(PortableDocs, AllDocsNameCanonicalVersion) {
    EXPECT_TRUE(contains(known_limitations(), EXOSNAP_PROJECT_VERSION))
        << "KNOWN_LIMITATIONS.md must name version " << EXOSNAP_PROJECT_VERSION;
    EXPECT_TRUE(contains(portable_readme(), EXOSNAP_PROJECT_VERSION))
        << "README-PORTABLE.md must name version " << EXOSNAP_PROJECT_VERSION;
    EXPECT_TRUE(contains(root_readme(), EXOSNAP_PROJECT_VERSION))
        << "README.md must name version " << EXOSNAP_PROJECT_VERSION;
}

TEST(PortableDocs, NoDocClaimsItIsOnePointZeroRelease) {
    // "1.0" may appear ("before 1.0.0"), but no doc may present the product as
    // ExoSnap 1.0 / version 1.0.
    for (const auto& doc : {known_limitations(), portable_readme(), root_readme()}) {
        EXPECT_FALSE(contains(doc, "ExoSnap 1.0"));
        EXPECT_FALSE(contains(doc, "version 1.0"));
        EXPECT_FALSE(contains(doc, "Version 1.0"));
    }
}

// ---------------------------------------------------------------------------
// Pre-v1 status
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsStatesPreV1Preview) {
    EXPECT_TRUE(contains(known_limitations(), "pre-v1"));
    EXPECT_TRUE(contains(known_limitations(), "preview"));
}

// ---------------------------------------------------------------------------
// NVIDIA / NVENC requirement
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsDocumentsNvidiaRequirement) {
    const auto doc = known_limitations();
    EXPECT_TRUE(contains(doc, "NVIDIA"));
    EXPECT_TRUE(contains(doc, "NVENC"));
}

TEST(PortableDocs, PortableReadmeDocumentsNvidiaRequirement) {
    EXPECT_TRUE(contains(portable_readme(), "NVIDIA"));
}

TEST(PortableDocs, RootReadmeDocumentsNvidiaRequirement) {
    EXPECT_TRUE(contains(root_readme(), "NVIDIA NVENC"));
}

// ---------------------------------------------------------------------------
// MP4 split support (0.2.0 — MP4-SPLIT-REMUX-R1)
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsDocumentsMp4SplitSupport) {
    // MP4 split is now supported via per-segment remux.
    EXPECT_TRUE(contains(known_limitations(), "split is supported for MKV, WebM, and MP4"));
}

TEST(PortableDocs, PortableReadmeDocumentsMp4SplitSupport) {
    EXPECT_TRUE(contains(portable_readme(), "Split is supported for"));
    EXPECT_TRUE(contains(portable_readme(), "MP4"));
}

TEST(PortableDocs, RootReadmeDocumentsMp4SplitSupport) {
    EXPECT_TRUE(contains(root_readme(), "per-segment background MP4 remux"));
}

// ---------------------------------------------------------------------------
// Crash-recovery limitation
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsDocumentsCrashRecoveryLimitation) {
    // 0.2.0: crash recovery is now implemented. The doc must describe the
    // recovery workflow rather than saying it does not exist.
    const auto doc = known_limitations();
    EXPECT_TRUE(contains(doc, "Crash recovery is available"));
    EXPECT_TRUE(contains(doc, "recovery manifest"));
}

// ---------------------------------------------------------------------------
// Unsupported hardware encoding is not misrepresented as available
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsListsUnsupportedEncoders) {
    const auto doc = known_limitations();
    // AMD / Intel / software fallback are discussed only as NOT supported.
    EXPECT_TRUE(contains(doc, "AMD AMF"));
    EXPECT_TRUE(contains(doc, "Intel"));
    EXPECT_TRUE(contains(doc, "not"));
    // Must not present them as supported.
    EXPECT_FALSE(contains(doc, "AMD AMF hardware encoding is supported"));
    EXPECT_FALSE(contains(doc, "Intel Quick Sync is supported"));
}

// ---------------------------------------------------------------------------
// Container matrix honesty — HEVC/PCM/FLAC are not claimed as current
// ---------------------------------------------------------------------------
TEST(PortableDocs, KnownLimitationsDoesNotClaimUnimplementedCodecs) {
    const auto doc = known_limitations();
    EXPECT_FALSE(contains(doc, "HEVC is supported"));
    EXPECT_FALSE(contains(doc, "FLAC is supported"));
    EXPECT_FALSE(contains(doc, "PCM is supported"));
}

// ---------------------------------------------------------------------------
// Portable README cross-references and checksum guidance
// ---------------------------------------------------------------------------
TEST(PortableDocs, PortableReadmeReferencesKnownLimitations) {
    EXPECT_TRUE(contains(portable_readme(), "KNOWN_LIMITATIONS.md"));
}

TEST(PortableDocs, PortableReadmeHasChecksumInstructions) {
    EXPECT_TRUE(contains(portable_readme(), "Get-FileHash"));
}

TEST(PortableDocs, PortableReadmeDocumentsDefaultHotkey) {
    EXPECT_TRUE(contains(portable_readme(), "Alt+F9"));
}
