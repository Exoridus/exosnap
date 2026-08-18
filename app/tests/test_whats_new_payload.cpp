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

// QCR-203. LaunchUpdater treats this payload as optional — losing it costs the
// post-update overlay and nothing else, so a failure warns and the update
// continues. That decision is only defensible if the failure is reported at all,
// which is what this pins: QSaveFile::commit() is the write's success, not the
// open.
TEST(WhatsNewPayload, WriteToAnUnwritablePathReportsFailure) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    // A directory occupying the payload path: the parent exists, the file write
    // cannot succeed.
    ASSERT_TRUE(QDir().mkpath(path));

    EXPECT_FALSE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("1.2.0"))));
    EXPECT_FALSE(ReadWhatsNewPayload(path).has_value()) << "a failed write must leave nothing readable behind";
}

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

TEST(WhatsNewPayload, DeleteOnMissingFileIsNoop) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    ASSERT_FALSE(QFile::exists(path));
    DeleteWhatsNewPayload(path); // must not throw/crash on an absent file
    EXPECT_FALSE(QFile::exists(path));
}

// ---------------------------------------------------------------------------
// Corrupt-payload lifecycle at the level of the two primitives: a corrupt file
// parses to nullopt and is then removed, rather than being left to be re-read
// (and re-fail to parse) on every subsequent launch.
//
// The named caller this used to describe — MainWindow::checkAndShowWhatsNewOverlay()
// — went with the Qt Widgets shell. The rule did not: it now lives in
// ConsumeWhatsNewPayload(), covered end to end at the bottom of this file.
// ---------------------------------------------------------------------------

TEST(WhatsNewPayload, CorruptFileParsesToNulloptAndCallerDeletesIt) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("not json {");
    f.close();
    ASSERT_TRUE(QFile::exists(path));

    // Mirrors MainWindow::checkAndShowWhatsNewOverlay(): read, and on nullopt,
    // delete regardless of whether the file existed (best-effort delete is a
    // no-op when absent).
    const auto payload = ReadWhatsNewPayload(path);
    ASSERT_FALSE(payload.has_value());
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

// ---------------------------------------------------------------------------
// ConsumeWhatsNewPayload (read + decide + clear, once)
// ---------------------------------------------------------------------------
//
// The Qt Quick cutover dropped the surface these rules feed, and with it the only
// caller that applied them. They are here rather than in the composition root so
// that "shown once" and "cleared either way" are one function nobody can
// implement half of.

TEST(WhatsNewPayload, ConsumeShowsTheNotesAndClearsThePayload) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    ASSERT_TRUE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("1.2.0"))));

    const WhatsNewConsumption consumed = ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/false);
    EXPECT_TRUE(consumed.show);
    ASSERT_EQ(consumed.notes.size(), 2);
    // Newest first, as written.
    EXPECT_EQ(consumed.notes.at(0).version, QStringLiteral("1.2.0"));
    EXPECT_FALSE(QFile::exists(path)) << "a shown payload must not survive to be shown again";
}

TEST(WhatsNewPayload, ConsumeIsOneTimeAcrossLaunches) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    ASSERT_TRUE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("1.2.0"))));

    ASSERT_TRUE(ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/false).show);
    // The next launch of the same build finds nothing.
    const WhatsNewConsumption second = ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/false);
    EXPECT_FALSE(second.show) << "the post-update overlay showed a second time";
    EXPECT_TRUE(second.notes.isEmpty());
}

TEST(WhatsNewPayload, ConsumeClearsAPayloadForAnotherVersionWithoutShowing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    // First install / downgrade / manual-ZIP update: the payload names a version
    // this build is not.
    ASSERT_TRUE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("9.9.9"))));

    const WhatsNewConsumption consumed = ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/false);
    EXPECT_FALSE(consumed.show);
    EXPECT_TRUE(consumed.notes.isEmpty());
    EXPECT_FALSE(QFile::exists(path)) << "a payload for another version must not be left to be reconsidered";
}

TEST(WhatsNewPayload, ConsumeClearsASuppressedPayloadWithoutShowing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    ASSERT_TRUE(WriteWhatsNewPayload(path, MakePayload(QStringLiteral("1.2.0"))));

    EXPECT_FALSE(ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/true).show);
    // Suppression means "do not show notes after an update", not "stop asking on
    // the launch after that": the payload still goes.
    EXPECT_FALSE(QFile::exists(path));
}

TEST(WhatsNewPayload, ConsumeClearsACorruptPayloadSilently) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = PayloadPath(dir);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json");
    file.close();

    EXPECT_FALSE(ConsumeWhatsNewPayload(path, QStringLiteral("1.2.0"), /*suppressed=*/false).show);
    EXPECT_FALSE(QFile::exists(path)) << "a corrupt payload would be re-read and re-rejected on every launch";
}

TEST(WhatsNewPayload, ConsumeOnAnAbsentPayloadIsANoop) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // The ordinary case: no update has run.
    EXPECT_FALSE(ConsumeWhatsNewPayload(PayloadPath(dir), QStringLiteral("1.2.0"), /*suppressed=*/false).show);
}

} // namespace
} // namespace exosnap
