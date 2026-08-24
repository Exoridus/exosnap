#include <exosnap/engine/version.h>
#include <gtest/gtest.h>

TEST(SmokeTest, VersionIsNonEmpty) {
    const auto v = exosnap::engine::version();
    EXPECT_FALSE(v.empty());
    EXPECT_TRUE(v.starts_with("0."));
}
