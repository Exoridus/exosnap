#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "services/WhatsNewPayload.h"

namespace exosnap {
namespace {

QString PayloadPath(const QTemporaryDir& dir) {
    return QDir(dir.path()).filePath(QStringLiteral("whats-new-pending.json"));
}

WhatsNewPendingPayload MakePayload(const QString& target) {
    WhatsNewPendingPayload p;
    p.target_version = target;
    p.notes.push_back(
        {QStringLiteral("1.2.0"), QStringLiteral("## 1.2.0\n- Feature C"), QStringLiteral("https://gh/r/v1.2.0")});
    p.notes.push_back(
        {QStringLiteral("1.1.0"), QStringLiteral("## 1.1.0\n- Feature B"), QStringLiteral("https://gh/r/v1.1.0")});
    return p;
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST(WhatsNewPayload, WriteThenReadRoundTrips) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);

    const WhatsNewPendingPayload written = MakePayload(QStringLiteral("1.2.0"));
    ASSERT_TRUE(WriteWhatsNewPayload(path, written));

    const auto read = ReadWhatsNewPayload(path);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->target_version, QStringLiteral("1.2.0"));
    ASSERT_EQ(read->notes.size(), 2);
    EXPECT_EQ(read->notes[0].version, QStringLiteral("1.2.0"));
    EXPECT_EQ(read->notes[0].body, QStringLiteral("## 1.2.0\n- Feature C"));
    EXPECT_EQ(read->notes[0].html_url, QStringLiteral("https://gh/r/v1.2.0"));
    EXPECT_EQ(read->notes[1].version, QStringLiteral("1.1.0"));
}

TEST(WhatsNewPayload, ReadMissingFileReturnsNullopt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(ReadWhatsNewPayload(PayloadPath(dir)).has_value());
}

TEST(WhatsNewPayload, ReadMalformedReturnsNullopt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("not json {");
    f.close();
    EXPECT_FALSE(ReadWhatsNewPayload(path).has_value());
}

TEST(WhatsNewPayload, DeleteRemovesFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    ASSERT_TRUE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("1.2.0"))));
    ASSERT_TRUE(QFile::exists(path));
    DeleteWhatsNewPayload(path);
    EXPECT_FALSE(QFile::exists(path));
}

// ---------------------------------------------------------------------------
// ShouldShowWhatsNew (pure decision)
// ---------------------------------------------------------------------------

TEST(WhatsNewPayload, ShowWhenTargetMatchesAndNotSuppressed) {
    const auto payload = MakePayload(QStringLiteral("1.2.0"));
    EXPECT_TRUE(ShouldShowWhatsNew(payload, QStringLiteral("1.2.0"), /*suppressed=*/false));
}

TEST(WhatsNewPayload, SuppressedHidesPostUpdateOverlay) {
    const auto payload = MakePayload(QStringLiteral("1.2.0"));
    EXPECT_FALSE(ShouldShowWhatsNew(payload, QStringLiteral("1.2.0"), /*suppressed=*/true));
}

TEST(WhatsNewPayload, TargetMismatchDoesNotShow) {
    // Downgrade / manual-ZIP / mismatched build: never show.
    const auto payload = MakePayload(QStringLiteral("9.9.9"));
    EXPECT_FALSE(ShouldShowWhatsNew(payload, QStringLiteral("1.0.0"), /*suppressed=*/false));
}

TEST(WhatsNewPayload, AbsentPayloadDoesNotShow) {
    EXPECT_FALSE(ShouldShowWhatsNew(std::nullopt, QStringLiteral("1.0.0"), /*suppressed=*/false));
}

TEST(WhatsNewPayload, EmptyNotesDoesNotShow) {
    WhatsNewPendingPayload payload;
    payload.target_version = QStringLiteral("1.2.0");
    EXPECT_FALSE(ShouldShowWhatsNew(payload, QStringLiteral("1.2.0"), /*suppressed=*/false));
}

} // namespace
} // namespace exosnap
