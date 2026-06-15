// test_update_semver.cpp -- SemVer parsing and comparison tests.

#include <gtest/gtest.h>
#include <update/update_types.h>

using namespace exosnap::update;

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
TEST(SemVer, ParseSimple) {
    auto v = ParseSemVer("1.2.3");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1u);
    EXPECT_EQ(v->minor, 2u);
    EXPECT_EQ(v->patch, 3u);
}

TEST(SemVer, ParseWithVPrefix) {
    // Leading 'v' is NOT part of semver; callers strip it before ParseSemVer
    auto v = ParseSemVer("0.3.0");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 0u);
    EXPECT_EQ(v->minor, 3u);
    EXPECT_EQ(v->patch, 0u);
}

TEST(SemVer, ParseWithPrereleaseLabel) {
    // Prerelease suffix is tolerated but ignored in comparison
    auto v = ParseSemVer("2.0.0-rc1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 2u);
    EXPECT_EQ(v->minor, 0u);
    EXPECT_EQ(v->patch, 0u);
}

TEST(SemVer, ParseInvalidEmpty) {
    EXPECT_FALSE(ParseSemVer("").has_value());
}

TEST(SemVer, ParseInvalidMissingComponents) {
    EXPECT_FALSE(ParseSemVer("1.2").has_value());
    EXPECT_FALSE(ParseSemVer("1").has_value());
}

TEST(SemVer, ParseInvalidNonNumeric) {
    EXPECT_FALSE(ParseSemVer("a.b.c").has_value());
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------
TEST(SemVer, Equality) {
    SemVer a{1, 2, 3}, b{1, 2, 3};
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);
}

TEST(SemVer, LessThanMajor) {
    EXPECT_LT((SemVer{0, 9, 9}), (SemVer{1, 0, 0}));
}

TEST(SemVer, LessThanMinor) {
    EXPECT_LT((SemVer{1, 2, 9}), (SemVer{1, 3, 0}));
}

TEST(SemVer, LessThanPatch) {
    EXPECT_LT((SemVer{1, 2, 3}), (SemVer{1, 2, 4}));
}

TEST(SemVer, GreaterThan) {
    EXPECT_GT((SemVer{2, 0, 0}), (SemVer{1, 9, 9}));
}

TEST(SemVer, ToString) {
    SemVer v{0, 3, 0};
    EXPECT_EQ(v.ToString(), "0.3.0");
}

TEST(SemVer, ToStringLargeNumbers) {
    SemVer v{10, 20, 300};
    EXPECT_EQ(v.ToString(), "10.20.300");
}
