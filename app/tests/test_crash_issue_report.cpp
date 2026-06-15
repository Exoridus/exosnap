// test_crash_issue_report.cpp -- Unit tests for the Stage-0 GitHub crash-issue
// builder (services/CrashIssueReport). Pure QString/QUrl logic; no widgets.

#include <gtest/gtest.h>

#include <QUrl>
#include <QUrlQuery>

#include "services/CrashIssueReport.h"

namespace exosnap::services {
namespace {

CrashIssueData MakeData() {
    CrashIssueData d;
    d.app_version = QStringLiteral("0.4.0 · build a5d55f1");
    d.os = QStringLiteral("Windows 11 · 26100.1742");
    d.gpu = QStringLiteral("NVIDIA RTX 4070 · driver 552.44");
    d.encoder = QStringLiteral("NVENC AV1 → MKV");
    d.exception = QStringLiteral("0xC0000005 · ACCESS_VIOLATION");
    d.correlation_id = QStringLiteral("a1b2c3d4-e5f6-4789-abcd-1234567890ab");
    return d;
}

// ---------------------------------------------------------------------------
// Metadata block
// ---------------------------------------------------------------------------

TEST(CrashIssueMetadata, ContainsAllowlistedFields) {
    const QString block = BuildCrashMetadataBlock(MakeData());
    EXPECT_TRUE(block.contains(QStringLiteral("0.4.0")));
    EXPECT_TRUE(block.contains(QStringLiteral("Windows 11")));
    EXPECT_TRUE(block.contains(QStringLiteral("RTX 4070")));
    EXPECT_TRUE(block.contains(QStringLiteral("NVENC AV1")));
    EXPECT_TRUE(block.contains(QStringLiteral("ACCESS_VIOLATION")));
    EXPECT_TRUE(block.contains(QStringLiteral("a1b2c3d4")));
}

TEST(CrashIssueMetadata, EmptyExceptionRendersAsDash) {
    CrashIssueData d = MakeData();
    d.exception.clear();
    const QString block = BuildCrashMetadataBlock(d);
    EXPECT_TRUE(block.contains(QStringLiteral("Exception: —"))) << block.toStdString();
}

// ---------------------------------------------------------------------------
// Issue URL
// ---------------------------------------------------------------------------

TEST(CrashIssueUrl, StartsWithRepoIssuesPath) {
    const QString url = BuildCrashIssueUrl(MakeData());
    EXPECT_TRUE(url.startsWith(QStringLiteral("https://github.com/Exoridus/exosnap/issues/new"))) << url.toStdString();
}

TEST(CrashIssueUrl, IsWellFormedAndValid) {
    const QString url = BuildCrashIssueUrl(MakeData());
    QUrl parsed(url);
    ASSERT_TRUE(parsed.isValid()) << url.toStdString();
    EXPECT_EQ(parsed.host(), QStringLiteral("github.com"));
    EXPECT_EQ(parsed.path(), QStringLiteral("/Exoridus/exosnap/issues/new"));
}

TEST(CrashIssueUrl, CarriesTemplateLabelAndMetadataFieldId) {
    const QString url = BuildCrashIssueUrl(MakeData());
    QUrlQuery q(QUrl(url).query());
    EXPECT_EQ(q.queryItemValue(QStringLiteral("template")), QStringLiteral("crash.yml"));
    EXPECT_EQ(q.queryItemValue(QStringLiteral("labels")), QStringLiteral("crash"));
    // The auto-filled textarea is matched by the "metadata" field id in crash.yml.
    EXPECT_TRUE(q.hasQueryItem(QStringLiteral("metadata")));
}

TEST(CrashIssueUrl, MetadataParamDecodesToTheMetadataBlock) {
    const CrashIssueData d = MakeData();
    const QString url = BuildCrashIssueUrl(d);
    // Decode the percent-encoded metadata field and confirm it round-trips to
    // the same block BuildCrashMetadataBlock produces.
    QUrlQuery q(QUrl(url).query());
    const QString decoded = q.queryItemValue(QStringLiteral("metadata"), QUrl::FullyDecoded);
    EXPECT_EQ(decoded, BuildCrashMetadataBlock(d));
}

TEST(CrashIssueUrl, WithinLengthBudgetForTypicalData) {
    const QString url = BuildCrashIssueUrl(MakeData());
    EXPECT_LE(url.size(), 6000) << "Typical crash data must yield a URL within budget";
    // Must be the prefilled URL (not the bare fallback) for typical data.
    EXPECT_TRUE(url.contains(QStringLiteral("metadata")));
}

TEST(CrashIssueUrl, OversizedDataFallsBackToBareTemplateUrl) {
    CrashIssueData d = MakeData();
    // Force the encoded URL past the 6000-char budget.
    d.exception = QString(20000, QLatin1Char('X'));
    const QString url = BuildCrashIssueUrl(d);
    EXPECT_LE(url.size(), 6000) << "Fallback URL must stay within budget";
    // Bare template URL: template + labels, but no prefilled metadata field.
    QUrlQuery q(QUrl(url).query());
    EXPECT_EQ(q.queryItemValue(QStringLiteral("template")), QStringLiteral("crash.yml"));
    EXPECT_EQ(q.queryItemValue(QStringLiteral("labels")), QStringLiteral("crash"));
    EXPECT_FALSE(q.hasQueryItem(QStringLiteral("metadata"))) << "Oversized data must drop the prefilled metadata field";
}

TEST(CrashIssueUrl, NoRawSpacesOrBackslashesInEncodedUrl) {
    // Even if a caller passed an unscrubbed value, the URL must be percent-
    // encoded (well-formed) — no raw spaces or backslashes leak into the URL.
    CrashIssueData d = MakeData();
    d.gpu = QStringLiteral("C:\\Users\\Alice\\driver weird");
    const QString url = BuildCrashIssueUrl(d);
    EXPECT_FALSE(url.contains(QLatin1Char(' ')));
    EXPECT_FALSE(url.contains(QLatin1Char('\\')));
    EXPECT_TRUE(QUrl(url).isValid());
}

} // namespace
} // namespace exosnap::services
