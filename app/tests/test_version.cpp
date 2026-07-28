#include <gtest/gtest.h>

#include "ExoSnapBuildInfo.h"

#include <cctype>
#include <string>

// These tests pin the runtime-facing release identity that the About surface,
// the update checker, and logs present. The canonical sources are the root
// project(... VERSION ...) (base) and EXOSNAP_RELEASE_VERSION (full release
// version); both are injected as compile definitions so the whole generation
// chain is guarded, not just the literal strings.

namespace {
std::string version() {
    return std::string(exosnap::build::kVersion);
}

std::string base_version() {
    return std::string(exosnap::build::kBaseVersion);
}

bool is_numeric_dotted_semver(const std::string& v) {
    int dots = 0;
    for (char c : v) {
        if (c == '.')
            ++dots;
        else if (!(c >= '0' && c <= '9'))
            return false;
    }
    return dots == 2;
}
} // namespace

// Verifies the build-info generation chain: the runtime strings must equal the
// canonical configure-time values. Self-updates on a version bump — no
// per-release edit needed here.
TEST(Version, MatchesCanonicalConfiguredVersions) {
    EXPECT_EQ(version(), EXOSNAP_EXPECTED_VERSION);
    EXPECT_EQ(base_version(), EXOSNAP_EXPECTED_BASE_VERSION);
}

TEST(Version, BaseIsThreeComponentNumericSemVer) {
    EXPECT_TRUE(is_numeric_dotted_semver(base_version()))
        << "base version must be numeric X.Y.Z, got: " << base_version();
}

// The full version is the base version plus an optional SemVer prerelease
// suffix — never a diverging core version.
TEST(Version, FullVersionExtendsBaseVersion) {
    const std::string v = version();
    const std::string base = base_version();
    ASSERT_EQ(v.rfind(base, 0), 0u) << "full version must start with base: " << v;
    const std::string suffix = v.substr(base.size());
    if (!suffix.empty()) {
        EXPECT_EQ(suffix[0], '-') << "suffix must be a -prerelease label: " << v;
        EXPECT_GT(suffix.size(), 1u) << "dangling '-' in version: " << v;
    }
}

// Unofficial builds must be honest: they always carry the reserved -dev label
// and can never impersonate a release. Official builds must never carry it.
TEST(Version, DevLabelMatchesOfficialFlag) {
    const std::string v = version();
    if (exosnap::build::kOfficialBuild) {
        EXPECT_EQ(v.find("-dev"), std::string::npos) << "official build must not report a dev version: " << v;
    } else {
        EXPECT_EQ(v, base_version() + "-dev") << "unofficial build must report <base>-dev, got: " << v;
    }
}

TEST(Version, IsNotOnePointZero) {
    const std::string v = base_version();
    EXPECT_NE(v, "1.0");
    EXPECT_NE(v, "1.0.0");
    EXPECT_NE(v.rfind("1.", 0), 0u) << "version must not be a 1.x release: " << v;
}

TEST(Version, BuildTimestampIsIso8601Utc) {
    const std::string ts = exosnap::build::kBuildTimestampUtc;
    ASSERT_EQ(ts.size(), 20u) << ts;
    for (size_t i = 0; i < ts.size(); ++i) {
        switch (i) {
        case 4:
        case 7:
            EXPECT_EQ(ts[i], '-') << ts;
            break;
        case 10:
            EXPECT_EQ(ts[i], 'T') << ts;
            break;
        case 13:
        case 16:
            EXPECT_EQ(ts[i], ':') << ts;
            break;
        case 19:
            EXPECT_EQ(ts[i], 'Z') << ts;
            break;
        default:
            EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(ts[i]))) << ts;
            break;
        }
    }
}

TEST(Version, CommitIdentityIsConsistent) {
    const std::string short_sha = exosnap::build::kGitCommit;
    const std::string full_sha = exosnap::build::kGitCommitFull;
    if (full_sha == "Unavailable") {
        EXPECT_EQ(short_sha, "Unavailable");
        return;
    }
    EXPECT_EQ(full_sha.size(), 40u) << full_sha;
    for (char c : full_sha) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c))) << full_sha;
    }
    ASSERT_NE(short_sha, "Unavailable");
    EXPECT_EQ(full_sha.rfind(short_sha, 0), 0u)
        << "short SHA must prefix the full SHA: " << short_sha << " / " << full_sha;
}
