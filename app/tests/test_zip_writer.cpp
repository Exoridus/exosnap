// ZipWriter wraps the vendored miniz to build a support-bundle .zip. These tests
// round-trip through libs/update's ExtractZip (the same miniz, the reader side) so
// a green test proves a real, extractable archive, and pin the entry-name guard.

#include <gtest/gtest.h>

#include "diagnostics/ZipWriter.h"

#include <update/zip_extract.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace exosnap::diagnostics {
namespace {

std::string ReadFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = f.readAll();
    return std::string(data.constData(), static_cast<size_t>(data.size()));
}

TEST(ZipWriter, RoundTripsThroughExtractZip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString zip_path = tmp.path() + QStringLiteral("/bundle.zip");
    const QString dest = tmp.path() + QStringLiteral("/out");

    {
        ZipWriter zip;
        ASSERT_TRUE(zip.AddFileFromMemory("exosnap.log", std::string_view("line one\nline two\n")));
        ASSERT_TRUE(zip.AddFileFromMemory("reports/session-1.json", std::string_view("{\"schema_version\":1}")));
        ASSERT_TRUE(zip.WriteToFile(zip_path.toStdWString()));
    }

    const auto err = update::ExtractZip(zip_path.toStdWString(), dest.toStdWString());
    ASSERT_FALSE(err.has_value()) << *err;

    EXPECT_EQ(ReadFile(dest + QStringLiteral("/exosnap.log")), "line one\nline two\n");
    EXPECT_EQ(ReadFile(dest + QStringLiteral("/reports/session-1.json")), "{\"schema_version\":1}");
}

TEST(ZipWriter, RejectsUnsafeEntryNames) {
    ZipWriter zip;
    EXPECT_FALSE(zip.AddFileFromMemory("../escape.txt", std::string_view("x")));
    EXPECT_FALSE(zip.AddFileFromMemory("C:/abs.txt", std::string_view("x")));
    EXPECT_FALSE(zip.AddFileFromMemory("/abs.txt", std::string_view("x")));
    // A safe name still works after the rejections.
    EXPECT_TRUE(zip.AddFileFromMemory("ok.txt", std::string_view("x")));
}

TEST(ZipWriter, EmptyArchiveFinalizes) {
    ZipWriter zip;
    const auto bytes = zip.Finalize();
    EXPECT_FALSE(bytes.empty()); // a valid empty zip still has an end-of-central-directory record
}

// ---------------------------------------------------------------------------
// QCR-204: success is a statement about the file, not about the buffer.
// ---------------------------------------------------------------------------

TEST(ZipWriter, WriteToAnUnopenablePathFails) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // A directory occupying the archive path: the open cannot succeed, and the
    // caller must never be told a support bundle was produced.
    const QString zip_path = tmp.path() + QStringLiteral("/bundle.zip");
    ASSERT_TRUE(QDir().mkpath(zip_path));

    ZipWriter zip;
    ASSERT_TRUE(zip.AddFileFromMemory("exosnap.log", std::string_view("line one\n")));
    EXPECT_FALSE(zip.WriteToFile(zip_path.toStdWString()));
}

TEST(ZipWriter, AFinalizedWriterCannotBeReusedAndReportsSoRatherThanWritingAStub) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString first = tmp.path() + QStringLiteral("/first.zip");
    const QString second = tmp.path() + QStringLiteral("/second.zip");

    ZipWriter zip;
    ASSERT_TRUE(zip.AddFileFromMemory("exosnap.log", std::string_view("line one\n")));
    ASSERT_TRUE(zip.WriteToFile(first.toStdWString()));

    // The second call has nothing to finalize. It must fail, and — the part that
    // matters for a support bundle — it must not leave a file at the name the
    // caller asked for, because that file would not be a readable archive.
    EXPECT_FALSE(zip.AddFileFromMemory("late.txt", std::string_view("x")));
    EXPECT_FALSE(zip.WriteToFile(second.toStdWString()));
    EXPECT_FALSE(QFile::exists(second)) << "no file may be left under a name whose write was reported as failed";
}

TEST(ZipWriter, ASuccessfulWriteLeavesAnArchiveThatOpens) {
    // The positive half of the same contract: after WriteToFile returns true the
    // bytes are on the volume and complete, not sitting in a stream buffer whose
    // destructor may still fail. Re-opening and extracting is the only honest
    // check of that, since the failure being fixed is invisible from good().
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString zip_path = tmp.path() + QStringLiteral("/bundle.zip");
    const QString dest = tmp.path() + QStringLiteral("/out");

    ZipWriter zip;
    ASSERT_TRUE(zip.AddFileFromMemory("exosnap.log", std::string_view("line one\n")));
    ASSERT_TRUE(zip.WriteToFile(zip_path.toStdWString()));

    // Deliberately inside the same scope as the writer: before the flush/close
    // fix this was the difference between "the ZipWriter object is gone" and
    // "the bytes are on disk".
    const auto err = update::ExtractZip(zip_path.toStdWString(), dest.toStdWString());
    ASSERT_FALSE(err.has_value()) << *err;
    EXPECT_EQ(ReadFile(dest + QStringLiteral("/exosnap.log")), "line one\n");
}

} // namespace
} // namespace exosnap::diagnostics
