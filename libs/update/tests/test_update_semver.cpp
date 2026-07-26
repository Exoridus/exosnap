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
    // "-rcN" is this project's only real prerelease tag shape (release-checklist.md
    // §3). The ordinal is captured so rc1/rc2/the eventual final release of the
    // same X.Y.Z compare distinctly instead of all three being equal.
    auto v = ParseSemVer("2.0.0-rc1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 2u);
    EXPECT_EQ(v->minor, 0u);
    EXPECT_EQ(v->patch, 0u);
    EXPECT_TRUE(v->is_prerelease);
    EXPECT_EQ(v->prerelease_number, 1u);
}

TEST(SemVer, ParseFinalReleaseIsNotPrerelease) {
    auto v = ParseSemVer("2.0.0");
    ASSERT_TRUE(v.has_value());
    EXPECT_FALSE(v->is_prerelease);
    EXPECT_EQ(v->prerelease_number, 0u);
}

TEST(SemVer, ParseWithMultiDigitPrereleaseOrdinal) {
    auto v = ParseSemVer("0.9.0-rc12");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->is_prerelease);
    EXPECT_EQ(v->prerelease_number, 12u);
}

TEST(SemVer, ParseWithUnrecognizedPrereleaseLabelStillParsesAsPrerelease) {
    // Not a shape this project's own tooling produces, but tolerated rather
    // than rejected -- sorts as prerelease ordinal 0 (before the final release
    // of the same X.Y.Z), just without fine-grained ordering.
    auto v = ParseSemVer("1.0.0-alpha");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->is_prerelease);
    EXPECT_EQ(v->prerelease_number, 0u);
}

TEST(SemVer, ParseWithBuildMetadata) {
    auto v = ParseSemVer("1.2.3+build.5");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->patch, 3u);
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

// ---------------------------------------------------------------------------
// Prerelease ordering (UPDATE-SEMVER-PRERELEASE-R1)
// ---------------------------------------------------------------------------
TEST(SemVer, Rc1LessThanRc2SameCoreVersion) {
    auto rc1 = ParseSemVer("0.9.0-rc1");
    auto rc2 = ParseSemVer("0.9.0-rc2");
    ASSERT_TRUE(rc1.has_value());
    ASSERT_TRUE(rc2.has_value());
    EXPECT_LT(*rc1, *rc2);
    EXPECT_FALSE(*rc2 < *rc1);
    EXPECT_NE(*rc1, *rc2);
}

TEST(SemVer, PrereleaseLessThanFinalOfSameCoreVersion) {
    auto rc1 = ParseSemVer("0.9.0-rc1");
    auto final_release = ParseSemVer("0.9.0");
    ASSERT_TRUE(rc1.has_value());
    ASSERT_TRUE(final_release.has_value());
    EXPECT_LT(*rc1, *final_release);
    EXPECT_GT(*final_release, *rc1);
}

TEST(SemVer, TwoFinalReleasesOfSameCoreVersionCompareEqual) {
    EXPECT_EQ(ParseSemVer("0.9.0"), ParseSemVer("0.9.0"));
}

TEST(SemVer, HigherCoreVersionOutranksPrereleaseRegardlessOfOrdinal) {
    // 0.10.0-rc1 must still beat 0.9.0-rc99 -- prerelease ordering only
    // matters once major.minor.patch are equal.
    auto next_rc1 = ParseSemVer("0.10.0-rc1");
    auto old_rc99 = ParseSemVer("0.9.0-rc99");
    ASSERT_TRUE(next_rc1.has_value());
    ASSERT_TRUE(old_rc99.has_value());
    EXPECT_GT(*next_rc1, *old_rc99);
}

TEST(SemVer, ToStringIncludesPrereleaseSuffix) {
    SemVer v = *ParseSemVer("0.9.0-rc3");
    EXPECT_EQ(v.ToString(), "0.9.0-rc3");
}

TEST(SemVer, ToStringOmitsSuffixForFinalRelease) {
    SemVer v = *ParseSemVer("0.9.0");
    EXPECT_EQ(v.ToString(), "0.9.0");
}
