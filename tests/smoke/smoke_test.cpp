#include <gtest/gtest.h>
#include <recorder_core/version.h>

TEST(SmokeTest, VersionIsNonEmpty) {
    const auto v = recorder_core::version();
    EXPECT_FALSE(v.empty());
    EXPECT_TRUE(v.starts_with("0."));
}
