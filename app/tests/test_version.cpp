#include <gtest/gtest.h>

#include "ExoSnapBuildInfo.h"

#include <string>

// These tests pin the runtime-facing version string that the About surface and
// logs present. The canonical source is the root project(... VERSION ...); this
// guards against accidental drift back to a dev/pre string or a premature 1.0.

namespace {
std::string version() {
    return std::string(exosnap::build::kVersion);
}
} // namespace

// Verifies the build-info generation chain: the runtime kVersion must equal the
// canonical PROJECT_VERSION (injected as EXOSNAP_EXPECTED_VERSION at build time).
// Self-updates on a version bump — no per-release edit needed here.
TEST(Version, MatchesCanonicalProjectVersion) {
    EXPECT_EQ(version(), EXOSNAP_EXPECTED_VERSION);
}

TEST(Version, IsThreeComponentSemVer) {
    const std::string v = version();
    int dots = 0;
    for (char c : v) {
        if (c == '.')
            ++dots;
        else
            EXPECT_TRUE(c >= '0' && c <= '9') << "version must be numeric dotted form, got: " << v;
    }
    EXPECT_EQ(dots, 2) << "expected X.Y.Z, got: " << v;
}

TEST(Version, IsNotADevOrPreReleaseString) {
    const std::string v = version();
    EXPECT_EQ(v.find("dev"), std::string::npos) << v;
    EXPECT_EQ(v.find('-'), std::string::npos) << v;
}

TEST(Version, IsNotOnePointZero) {
    const std::string v = version();
    EXPECT_NE(v, "1.0");
    EXPECT_NE(v, "1.0.0");
    EXPECT_NE(v.rfind("1.", 0), 0u) << "version must not be a 1.x release: " << v;
}
