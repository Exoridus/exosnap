#include "EditExportText.h"

#include <gtest/gtest.h>

namespace exosnap::quick {
namespace {

TEST(EditExportText, VideoFailureDoesNotRecommendChangingTheContainer) {
    const engine::EditExportIssue issue{
        engine::EditExportIssueCode::UnsupportedProfile, engine::EditExportIssueScope::Video, {}};
    const auto message = TranslateEditExportIssue(issue, false);
    EXPECT_TRUE(message.contains(QStringLiteral("video")));
    EXPECT_FALSE(message.contains(QStringLiteral("MP4")));
    EXPECT_FALSE(message.contains(QStringLiteral("MKV")));
}

TEST(EditExportText, AudioFailureIsNotDescribedAsAVideoOrContainerFailure) {
    const engine::EditExportIssue issue{
        engine::EditExportIssueCode::UnsupportedProfile, engine::EditExportIssueScope::Audio, {}};
    const auto message = TranslateEditExportIssue(issue, true);
    EXPECT_TRUE(message.contains(QStringLiteral("audio")));
    EXPECT_FALSE(message.contains(QStringLiteral("source cut")));
    EXPECT_FALSE(message.contains(QStringLiteral("MP4")));
}

TEST(EditExportText, OriginalCopyAlternativeRequiresQualifiedFacts) {
    const engine::EditExportIssue issue{
        engine::EditExportIssueCode::UnsupportedProfile, engine::EditExportIssueScope::Video, {}};
    EXPECT_TRUE(TranslateEditExportIssue(issue, true).contains(QStringLiteral("original streams")));
    EXPECT_FALSE(TranslateEditExportIssue(issue, false).contains(QStringLiteral("original streams")));
}

TEST(EditExportText, Mp4AdviceRequiresTheSpecificCapabilityIssue) {
    const engine::EditExportIssue issue{
        engine::EditExportIssueCode::Mp4AudioUnavailable, engine::EditExportIssueScope::Audio, {}};
    const auto message = TranslateEditExportIssue(issue, false);
    EXPECT_TRUE(message.contains(QStringLiteral("MP4 audio")));
    EXPECT_TRUE(message.contains(QStringLiteral("MKV")));
}

} // namespace
} // namespace exosnap::quick
