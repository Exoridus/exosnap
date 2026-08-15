#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"

namespace exosnap {
namespace {

// =============================================================================
// Helpers
// =============================================================================

QString UniqueTempPath(const QString& suffix = QStringLiteral(".json")) {
    // Unique temp dir per test process (gtest_discover_tests = one process per
    // test); a shared fixed name races under ctest -j.
    static QTemporaryDir s_dir;
    static int s_counter = 0;
    return s_dir.filePath(QStringLiteral("exosnap_svc_test_%1%2").arg(++s_counter).arg(suffix));
}

// Write minimal valid bytes to path so QFileInfo::exists returns true.
bool CreateDummyFile(const QString& path, const QByteArray& content = QByteArray("dummy")) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(content);
    return true;
}

RecoveryManifestEntry MakeEntry(const QString& id, const QString& artefact,
                                const QString& container = QStringLiteral("mkv"), bool finalized = false) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T00:00:00Z");
    e.finalized = finalized;
    return e;
}

// Build a small but genuinely valid MKV at `path` via libavformat: one
// PCM_S16LE audio track with `seconds` of silence. No encoder is needed —
// PCM packets are raw bytes — so this stays lightweight while producing a
// file the repair-remux path can open and stream-copy from.
bool BuildPcmMkvFixture(const QString& path, double seconds) {
    const QByteArray path_utf8 = path.toUtf8();
    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "matroska", path_utf8.constData()) < 0 || ctx == nullptr)
        return false;

    bool ok = false;
    do {
        AVStream* st = avformat_new_stream(ctx, nullptr);
        if (st == nullptr)
            break;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        st->codecpar->sample_rate = 48000;
        av_channel_layout_default(&st->codecpar->ch_layout, 2);
        st->codecpar->format = AV_SAMPLE_FMT_S16;
        st->codecpar->bits_per_coded_sample = 16;
        st->time_base = AVRational{1, 48000};

        if (avio_open(&ctx->pb, path_utf8.constData(), AVIO_FLAG_WRITE) < 0)
            break;
        if (avformat_write_header(ctx, nullptr) < 0)
            break;

        AVPacket* pkt = av_packet_alloc();
        if (pkt == nullptr)
            break;
        const int samples_per_pkt = 1024;
        const int bytes_per_pkt = samples_per_pkt * 2 /*ch*/ * 2 /*bytes per sample*/;
        bool write_failed = false;
        for (int64_t pts = 0; pts < static_cast<int64_t>(seconds * 48000.0); pts += samples_per_pkt) {
            if (av_new_packet(pkt, bytes_per_pkt) < 0) {
                write_failed = true;
                break;
            }
            std::memset(pkt->data, 0, static_cast<size_t>(bytes_per_pkt));
            pkt->pts = pts;
            pkt->dts = pts;
            pkt->duration = samples_per_pkt;
            pkt->stream_index = 0;
            av_packet_rescale_ts(pkt, AVRational{1, 48000}, st->time_base);
            const int ret = av_interleaved_write_frame(ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                write_failed = true;
                break;
            }
        }
        av_packet_free(&pkt);
        if (write_failed)
            break;
        if (av_write_trailer(ctx) < 0)
            break;
        ok = true;
    } while (false);

    if (ctx->pb != nullptr)
        avio_closep(&ctx->pb);
    avformat_free_context(ctx);
    return ok;
}

// Holds a mandatory Windows byte-range lock on [offset, EOF) of `path` for
// the object's lifetime, on a handle separate from whatever the repair-remux
// opens. Windows byte-range locks are enforced against ALL other handles to
// the file, so any ReadFile the remuxer issues that overlaps the locked range
// fails with a genuine OS-level I/O error — a real mid-stream read fault, not
// a mock. (Same fault-injection pattern as the engine's remuxer tests.)
class ByteRangeLock {
  public:
    ByteRangeLock(const QString& path, uint64_t offset) {
        const QByteArray path_local = path.toLocal8Bit();
        handle_ = CreateFileA(path_local.constData(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return;
        const uint32_t offset_low = static_cast<uint32_t>(offset & 0xFFFFFFFFu);
        const uint32_t offset_high = static_cast<uint32_t>(offset >> 32);
        // Lock to "infinity" so every read at or beyond `offset` hits the lock,
        // regardless of the remuxer's exact buffering chunk sizes.
        locked_ = LockFile(handle_, offset_low, offset_high, 0xFFFFFFFFu, 0x7FFFFFFFu) != FALSE;
    }

    ~ByteRangeLock() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_); // also releases the lock
    }

    ByteRangeLock(const ByteRangeLock&) = delete;
    ByteRangeLock& operator=(const ByteRangeLock&) = delete;

    [[nodiscard]] bool ok() const {
        return handle_ != INVALID_HANDLE_VALUE && locked_;
    }

  private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool locked_ = false;
};

// =============================================================================
// 1. Scan removes orphaned entries (artefact no longer exists)
// =============================================================================

TEST(RecoveryServiceTest, ScanRemovesOrphanedEntries) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Entry whose artefact exists.
    const QString real_artefact = QDir(tmp.path()).filePath(QStringLiteral("real.mkv"));
    ASSERT_TRUE(CreateDummyFile(real_artefact));
    store.Add(MakeEntry(QStringLiteral("id-real"), real_artefact));

    // Entry whose artefact is gone.
    store.Add(MakeEntry(QStringLiteral("id-orphan"), QStringLiteral("/nonexistent/path.mkv")));

    const auto candidates = service.Scan();
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].entry.id, QStringLiteral("id-real"));

    // Orphan must be removed from the manifest.
    EXPECT_EQ(store.Entries().size(), 1);
}

// An artefact that exists but holds nothing is orphaned too. It shipped as a
// recoverable candidate because the filter only asked whether the file was
// there: a recording killed before the muxer wrote its first byte then offered
// itself for recovery on every launch, indefinitely, with nothing to restore.
TEST(RecoveryServiceTest, ScanRemovesEmptyArtefacts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString real_artefact = QDir(tmp.path()).filePath(QStringLiteral("real.mkv"));
    ASSERT_TRUE(CreateDummyFile(real_artefact));
    store.Add(MakeEntry(QStringLiteral("id-real"), real_artefact));

    const QString empty_artefact = QDir(tmp.path()).filePath(QStringLiteral("empty.mkv"));
    ASSERT_TRUE(CreateDummyFile(empty_artefact, QByteArray()));
    ASSERT_TRUE(QFileInfo::exists(empty_artefact));
    ASSERT_EQ(QFileInfo(empty_artefact).size(), 0);
    store.Add(MakeEntry(QStringLiteral("id-empty"), empty_artefact));

    const auto candidates = service.Scan();
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].entry.id, QStringLiteral("id-real"));

    // And it is gone from the manifest, so the prompt does not return next launch.
    ASSERT_EQ(store.Entries().size(), 1);
    EXPECT_EQ(store.Entries()[0].id, QStringLiteral("id-real"));
}

// =============================================================================
// 2. KeepAsMkv with finalized=true → rename
// =============================================================================

TEST(RecoveryServiceTest, KeepAsMkvFinalizedRenames) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Artefact: a .mkv.tmp (finalized engine output)
    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    const auto e = MakeEntry(QStringLiteral("keep-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    store.Add(e);

    const auto result = service.KeepAsMkv(e);
    EXPECT_TRUE(result.success) << result.message;

    // Artefact should be renamed (gone from tmp path).
    EXPECT_FALSE(QFileInfo::exists(artefact));

    // Entry should be removed.
    EXPECT_TRUE(store.Entries().isEmpty());

    // The renamed file should exist (same dir, .mkv extension).
    const QString expected = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv"));
    EXPECT_TRUE(QFileInfo::exists(expected));
}

// =============================================================================
// 3. Discard deletes the artefact and removes the entry
// =============================================================================

TEST(RecoveryServiceTest, DiscardDeletesArtefactAndEntry) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("discard.mkv"));
    ASSERT_TRUE(CreateDummyFile(artefact));
    const auto e = MakeEntry(QStringLiteral("discard-id"), artefact);
    store.Add(e);

    const auto result = service.Discard(e);
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_FALSE(QFileInfo::exists(artefact));
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// 4. Collision handling in KeepAsMkv rename
// =============================================================================

TEST(RecoveryServiceTest, KeepAsMkvHandlesCollision) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    // Pre-create the "natural" target to force collision resolution.
    const QString natural = QDir(tmp.path()).filePath(QStringLiteral("recording.mkv"));
    ASSERT_TRUE(CreateDummyFile(natural));

    const auto e = MakeEntry(QStringLiteral("col-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    store.Add(e);

    const auto result = service.KeepAsMkv(e);
    EXPECT_TRUE(result.success) << result.message;

    // Natural target is still there (untouched), artefact was renamed elsewhere.
    EXPECT_TRUE(QFileInfo::exists(natural));
    EXPECT_FALSE(QFileInfo::exists(artefact));

    // The renamed file must have "(2)" suffix.
    const QString collided = QDir(tmp.path()).filePath(QStringLiteral("recording (2).mkv"));
    EXPECT_TRUE(QFileInfo::exists(collided));
}

// =============================================================================
// 5. Scan returns size metadata
// =============================================================================

TEST(RecoveryServiceTest, ScanReturnsSizeMetadata) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("sized.mkv"));
    const QByteArray content(1024, 'X');
    ASSERT_TRUE(CreateDummyFile(artefact, content));
    store.Add(MakeEntry(QStringLiteral("size-id"), artefact));

    const auto candidates = service.Scan();
    ASSERT_EQ(candidates.size(), 1);
    EXPECT_EQ(candidates[0].artefact_size_bytes, 1024);
}

// =============================================================================
// ADR-0015: Finish tests
// =============================================================================

// 6. Finish with MKV-intended + finalized=true → rename
TEST(RecoveryServiceTest, FinishMkvFinalizedRenames) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("session.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    auto e = MakeEntry(QStringLiteral("finish-mkv-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    e.final_output_path = QDir(tmp.path()).filePath(QStringLiteral("session.mkv"));
    store.Add(e);

    const auto result = service.Finish(e);
    EXPECT_TRUE(result.success) << result.message;

    // Artefact should be gone (renamed).
    EXPECT_FALSE(QFileInfo::exists(artefact));
    // Manifest entry removed on success.
    EXPECT_TRUE(store.Entries().isEmpty());
    // Renamed file at final output path.
    EXPECT_TRUE(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral("session.mkv"))));
}

// 7. Finish with MKV-intended + finalized=false → does NOT crash on bad path
//    (RemuxToMkv will fail gracefully; entry preserved)
TEST(RecoveryServiceTest, FinishMkvNonFinalizedPreservesEntryOnRemuxFail) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Artefact exists but is not a valid MKV (dummy content) — remux will fail.
    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("corrupt.mkv"));
    ASSERT_TRUE(CreateDummyFile(artefact, QByteArray("not a real mkv")));

    auto e = MakeEntry(QStringLiteral("finish-corrupt-id"), artefact, QStringLiteral("mkv"), /*finalized=*/false);
    e.final_output_path = QDir(tmp.path()).filePath(QStringLiteral("corrupt.mkv"));
    store.Add(e);

    const auto result = service.Finish(e);
    // Remux fails on dummy data; Finish returns failure and preserves the entry.
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.message.empty());
    // Artefact is preserved.
    EXPECT_TRUE(QFileInfo::exists(artefact));
    // Manifest entry is preserved.
    EXPECT_EQ(store.Entries().size(), 1);
}

// 8. Finish uses fallback folder when stored folder no longer exists
TEST(RecoveryServiceTest, FinishFallsBackToConfiguredOutputFolder) {
    QTemporaryDir tmp_artefact;
    QTemporaryDir tmp_fallback;
    ASSERT_TRUE(tmp_artefact.isValid());
    ASSERT_TRUE(tmp_fallback.isValid());

    const QString store_path = QDir(tmp_artefact.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);
    service.SetFallbackOutputFolder(tmp_fallback.path());

    // Create artefact in artefact dir.
    const QString artefact = QDir(tmp_artefact.path()).filePath(QStringLiteral("rec.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    // Set final_output_path to a dir that does NOT exist.
    auto e = MakeEntry(QStringLiteral("fallback-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    e.final_output_path = QStringLiteral("C:/NonExistentDir12345/rec.mkv");
    store.Add(e);

    const auto result = service.Finish(e);
    EXPECT_TRUE(result.success) << result.message;
    // The renamed file should be in the fallback folder.
    EXPECT_FALSE(QFileInfo::exists(artefact));
    const QString expected_in_fallback = QDir(tmp_fallback.path()).filePath(QStringLiteral("rec.mkv"));
    EXPECT_TRUE(QFileInfo::exists(expected_in_fallback));
}

// 8b. Finish with MKV-intended + finalized=false, repair-remux fails MID-STREAM
//     (after the output file was created and partially written) → the partial
//     repair output is removed, the artefact and the manifest entry stay
//     (symmetric with the MP4 path's cleanup below).
//
// Fault injection: the artefact is a genuinely valid MKV; a byte-range lock on
// its second half lets avformat_open_input / find_stream_info (header reads
// near the front) succeed, so the repair creates the target file and copies
// packets until a read hits the lock and fails with a real I/O error. Without
// the failure-path cleanup this leaves a partial .mkv at the target path.
TEST(RecoveryServiceTest, FinishMkvNonFinalizedRemovesPartialOutputOnMidStreamFail) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    // Valid MKV artefact (5 s PCM ≈ 1 MB) named like a crash artefact.
    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("session.mkv.tmp"));
    ASSERT_TRUE(BuildPcmMkvFixture(artefact, /*seconds=*/5.0));
    const qint64 artefact_size = QFileInfo(artefact).size();
    ASSERT_GT(artefact_size, 4096) << "MKV fixture unexpectedly small";

    auto e = MakeEntry(QStringLiteral("finish-partial-id"), artefact, QStringLiteral("mkv"), /*finalized=*/false);
    e.final_output_path = QDir(tmp.path()).filePath(QStringLiteral("session.mkv"));
    store.Add(e);

    // Lock the second half of the artefact → mid-stream read failure.
    ByteRangeLock lock(artefact, static_cast<uint64_t>(artefact_size) / 2);
    ASSERT_TRUE(lock.ok()) << "Failed to establish the byte-range lock fixture";

    const auto result = service.Finish(e);
    EXPECT_FALSE(result.success) << "A mid-stream read failure must not report success";
    EXPECT_FALSE(result.message.empty());
    // Artefact and manifest entry are preserved — the artefact is the only
    // trustworthy recording.
    EXPECT_TRUE(QFileInfo::exists(artefact));
    EXPECT_EQ(store.Entries().size(), 1);
    // The partial repair output must NOT linger at the resolved target path.
    const QString target = QDir(tmp.path()).filePath(QStringLiteral("session.mkv"));
    EXPECT_FALSE(QFileInfo::exists(target)) << "Partial repair output was left behind";
}

// 9. Finish with MP4-intended → does NOT crash on dummy data (graceful failure)
TEST(RecoveryServiceTest, FinishMp4NonFinalizedPreservesEntryOnRemuxFail) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("rec.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact, QByteArray("not a real mkv")));

    auto e = MakeEntry(QStringLiteral("mp4-finish-id"), artefact, QStringLiteral("mp4"), /*finalized=*/false);
    e.final_output_path = QDir(tmp.path()).filePath(QStringLiteral("rec.mp4"));
    store.Add(e);

    const auto result = service.Finish(e);
    EXPECT_FALSE(result.success);             // remux fails on dummy data
    EXPECT_TRUE(QFileInfo::exists(artefact)); // artefact preserved
    EXPECT_EQ(store.Entries().size(), 1);     // entry preserved
}

// 10. SetFallbackOutputFolder with non-existent fallback still uses artefact parent
TEST(RecoveryServiceTest, FinishFallsBackToArtefactParentWhenNothingExists) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString store_path = QDir(tmp.path()).filePath(QStringLiteral("manifest.json"));
    RecoveryManifestStore store(store_path);
    RecoveryService service(store);
    // Set a non-existent fallback.
    service.SetFallbackOutputFolder(QStringLiteral("C:/DoesNotExist99999"));

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("lastresort.mkv.tmp"));
    ASSERT_TRUE(CreateDummyFile(artefact));

    // final_output_path points to a non-existent dir.
    auto e = MakeEntry(QStringLiteral("lastresort-id"), artefact, QStringLiteral("mkv"), /*finalized=*/true);
    e.final_output_path = QStringLiteral("C:/DoesNotExist88888/lastresort.mkv");
    store.Add(e);

    const auto result = service.Finish(e);
    // Last resort is artefact parent (tmp.path()), which exists → rename succeeds.
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_FALSE(QFileInfo::exists(artefact));
    // Renamed file is in artefact parent (tmp.path()).
    EXPECT_TRUE(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral("lastresort.mkv"))));
}

} // namespace
} // namespace exosnap
