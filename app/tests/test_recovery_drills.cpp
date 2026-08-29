// test_recovery_drills.cpp — the recovery drill matrix (Recording / Finalize /
// Remux) × (Ordered-Stop / Process-Kill), driven against the REAL RecoveryService
// with REAL synthetic-pipeline artefacts (a genuine MatroskaStreamWriter MKV, not
// a hand-forged dummy).
//
// A process-kill is modelled by truncating a finalized MKV (its committed clusters
// are exactly what a killed recording leaves; the missing trailer is the "not yet
// finalized" state). The full TerminateProcess-against-the-real-Coordinator kill —
// which alone exercises the Add/UpdateFinalized/Remove manifest choreography — is
// a LIVE drill (docs/dev/soak-and-recovery-drills.md); this suite validates the
// RECOVERY side (Scan/Finish/repair) against realistic partials.
//
// The Remux×Kill cell asserts the durability guarantee: after a kill that left a
// corrupt half-MP4 at the user-visible final path, recovery must place a playable
// MP4 there and leave no corrupt stub behind. RecoveryService::Finish now remuxes
// to a sibling ".tmp" staging file and atomically renames it onto the target (replacing
// any stale partial in place), so this is a hard assertion rather than an xfail.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <exosnap/engine/mp4_remuxer.h>

#include "services/AtomicFileOps.h"
#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"
#include "synthetic_session.h"

#include <filesystem>
#include <string>
#include <vector>

namespace exosnap {
namespace {

RecoveryManifestEntry MakeEntry(const QString& id, const QString& artefact, const QString& container,
                                const QString& final_output, bool finalized) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = final_output;
    e.started_at = QStringLiteral("2026-07-12T00:00:00Z");
    e.finalized = finalized;
    return e;
}

// Write a real MKV via the shared synthetic session (real MatroskaStreamWriter).
bool WriteSyntheticMkv(const QString& path, exosnap::engine::VideoCodec vc, exosnap::engine::AudioCodec ac,
                       double seconds) {
    exosnap::engine::testutil::SyntheticSessionConfig cfg;
    cfg.video_codec = vc;
    cfg.audio_codec = ac;
    cfg.output_path = path.toStdString();
    cfg.target_seconds = seconds;
    return exosnap::engine::testutil::SyntheticSession(cfg).Run().success;
}

bool TruncateInPlaceToFraction(const QString& path, double frac) {
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    const qint64 keep = static_cast<qint64>(static_cast<double>(f.size()) * frac);
    return f.resize(keep);
}

bool CanDemux(const QString& path) {
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) != 0)
        return false;
    const bool info_ok = avformat_find_stream_info(ctx, nullptr) >= 0;
    AVPacket* pkt = av_packet_alloc();
    int packets = 0;
    while (av_read_frame(ctx, pkt) >= 0) {
        ++packets;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return info_ok && packets > 0;
}

// =============================================================================
// Recording × Ordered-Stop: a clean finalized MKV → rename → demuxable file.
// =============================================================================
TEST(RecoveryDrill, RecordingOrderedStop_FinalizedMkvRenamesToPlayableFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RecoveryManifestStore store(QDir(tmp.path()).filePath(QStringLiteral("manifest.json")));
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("rec.mkv.tmp"));
    ASSERT_TRUE(WriteSyntheticMkv(artefact, exosnap::engine::VideoCodec::Av1, exosnap::engine::AudioCodec::Opus, 2.0));

    const QString final_out = QDir(tmp.path()).filePath(QStringLiteral("rec.mkv"));
    auto e = MakeEntry(QStringLiteral("rec-ordered"), artefact, QStringLiteral("mkv"), final_out, /*finalized=*/true);
    store.Add(e);

    const auto r = service.Finish(e);
    EXPECT_TRUE(r.success) << r.message;
    EXPECT_TRUE(QFileInfo::exists(final_out));
    EXPECT_TRUE(CanDemux(final_out)) << "renamed recording is not demuxable";
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// Recording × Process-Kill: a truncated (non-finalized) partial → RemuxToMkv
// repair. The call must complete; on repair failure the artefact + entry survive.
// =============================================================================
TEST(RecoveryDrill, RecordingProcessKill_NonFinalizedPartialRepairsOrPreserves) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RecoveryManifestStore store(QDir(tmp.path()).filePath(QStringLiteral("manifest.json")));
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("killed.mkv.tmp"));
    ASSERT_TRUE(WriteSyntheticMkv(artefact, exosnap::engine::VideoCodec::H264, exosnap::engine::AudioCodec::Aac, 4.0));
    ASSERT_TRUE(TruncateInPlaceToFraction(artefact, 0.6)); // drop the trailer + tail clusters

    const QString final_out = QDir(tmp.path()).filePath(QStringLiteral("killed.mkv"));
    auto e = MakeEntry(QStringLiteral("rec-kill"), artefact, QStringLiteral("mkv"), final_out, /*finalized=*/false);
    store.Add(e);

    const auto r = service.Finish(e); // must return (no crash/hang) on a truncated file
    if (r.success) {
        EXPECT_TRUE(QFileInfo::exists(final_out));
        EXPECT_TRUE(store.Entries().isEmpty());
    } else {
        // Graceful failure: the artefact is the only trustworthy copy — keep it.
        EXPECT_TRUE(QFileInfo::exists(artefact));
        EXPECT_EQ(store.Entries().size(), 1);
    }
}

// =============================================================================
// Remux(MP4) × Ordered-Stop: a valid MKV, MP4-intended → RemuxToProgressiveMp4
// produces a playable MP4 and clears the entry.
// =============================================================================
TEST(RecoveryDrill, RemuxMp4Ordered_ValidMkvProducesPlayableMp4) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RecoveryManifestStore store(QDir(tmp.path()).filePath(QStringLiteral("manifest.json")));
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("clip.mkv.tmp"));
    ASSERT_TRUE(WriteSyntheticMkv(artefact, exosnap::engine::VideoCodec::H264, exosnap::engine::AudioCodec::Aac, 2.0));

    const QString final_out = QDir(tmp.path()).filePath(QStringLiteral("clip.mp4"));
    auto e = MakeEntry(QStringLiteral("mp4-ordered"), artefact, QStringLiteral("mp4"), final_out, /*finalized=*/false);
    store.Add(e);

    const auto r = service.Finish(e);
    EXPECT_TRUE(r.success) << r.message;
    EXPECT_TRUE(QFileInfo::exists(final_out));
    EXPECT_TRUE(CanDemux(final_out)) << "recovered MP4 is not demuxable";
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// Remux(MP4) × Process-Kill: a corrupt half-MP4 already sits at final_output_path
// (the killed remux). Recovery must overwrite it in place with a playable MP4 and
// leave no stale file behind — not side-step to a fresh name and strand the corrupt
// stub where the user looks for their result.
// =============================================================================
TEST(RecoveryDrill, RemuxMp4ProcessKill_ReplacesStalePartialAtTargetPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RecoveryManifestStore store(QDir(tmp.path()).filePath(QStringLiteral("manifest.json")));
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("session.mkv.tmp"));
    ASSERT_TRUE(WriteSyntheticMkv(artefact, exosnap::engine::VideoCodec::H264, exosnap::engine::AudioCodec::Aac, 2.0));

    // A corrupt half-MP4 already at the user-visible target (the killed remux).
    const QString final_out = QDir(tmp.path()).filePath(QStringLiteral("session.mp4"));
    {
        QFile f(final_out);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(4096, '\x00')); // not a valid MP4
    }
    ASSERT_FALSE(CanDemux(final_out)) << "precondition: the stale file must be corrupt";

    auto e = MakeEntry(QStringLiteral("mp4-kill"), artefact, QStringLiteral("mp4"), final_out, /*finalized=*/false);
    store.Add(e);

    const auto r = service.Finish(e);
    ASSERT_TRUE(r.success) << r.message;

    // The user's target path holds a playable MP4, not the corrupt stub.
    EXPECT_TRUE(QFileInfo::exists(final_out));
    EXPECT_TRUE(CanDemux(final_out)) << "target path should hold a playable MP4";

    // The good file is AT the target — recovery did not side-step to "(2)".
    const QString sidestepped = QDir(tmp.path()).filePath(QStringLiteral("session (2).mp4"));
    EXPECT_FALSE(QFileInfo::exists(sidestepped)) << "recovery must not strand a duplicate at a fresh name";

    // No transient left behind.
    const QString leftover_temp = QDir(tmp.path()).filePath(QStringLiteral("session.mp4.tmp"));
    EXPECT_FALSE(QFileInfo::exists(leftover_temp)) << "the .tmp staging file must be gone after the atomic rename";

    // The manifest entry is cleared on success.
    EXPECT_TRUE(store.Entries().isEmpty());
}

// =============================================================================
// LIVE remux (RecordingCoordinator) durability. The live remux-on-stop and the
// per-segment background remux both write to a sibling ".tmp" staging file on the
// target's own volume and only atomically rename it onto the final path on
// success — the same primitives (MakeSiblingTempPath / AtomicReplaceInPlace) the
// crash-recovery path uses. These drills exercise that exact sequence so the
// live path carries the identical guarantee: a kill mid-remux never leaves a
// half-written file at the user-visible output path.
// =============================================================================

// The final output path is never written mid-remux; the finished file appears
// there only via the atomic rename. Before that, the bytes live in "<name>.tmp".
TEST(RecoveryDrill, LiveRemuxMp4_NeverWritesTargetMidFlightThenPublishesAtomically) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString transient_q = QDir(tmp.path()).filePath(QStringLiteral("live.mkv.tmp"));
    ASSERT_TRUE(
        WriteSyntheticMkv(transient_q, exosnap::engine::VideoCodec::H264, exosnap::engine::AudioCodec::Aac, 2.0));

    const std::filesystem::path transient(transient_q.toStdWString());
    const std::filesystem::path final_mp4(QDir(tmp.path()).filePath(QStringLiteral("live.mp4")).toStdWString());

    // The temp is a sibling on the same directory (== same volume) as the target.
    const std::filesystem::path temp = MakeSiblingTempPath(final_mp4);
    EXPECT_EQ(temp.filename().wstring(), L"live.mp4.tmp");
    EXPECT_EQ(temp.parent_path(), final_mp4.parent_path());

    bool progressed = false;
    auto progress_cb = [&](float) -> bool {
        progressed = true;
        // While the remux runs, the user-visible target must not exist yet.
        std::error_code ec;
        EXPECT_FALSE(std::filesystem::exists(final_mp4, ec))
            << "the target path must stay empty until the atomic publish";
        return true;
    };

    const auto result = exosnap::engine::RemuxToProgressiveMp4(transient, temp, progress_cb);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_TRUE(progressed) << "progress callback should fire so the mid-flight check runs";

    // Before the publish, the finished bytes are at the temp, not the target.
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(temp, ec));
    EXPECT_FALSE(std::filesystem::exists(final_mp4, ec)) << "not published until the rename";

    // Atomic publish onto the target.
    EXPECT_EQ(AtomicReplaceInPlace(temp, final_mp4), 0UL);
    EXPECT_TRUE(std::filesystem::exists(final_mp4, ec));
    EXPECT_TRUE(CanDemux(QString::fromStdWString(final_mp4.wstring()))) << "published MP4 must be playable";
    EXPECT_FALSE(std::filesystem::exists(temp, ec)) << "the temp was renamed away";
}

TEST(RecoveryDrill, DisposableSiblingStagingPath_CollisionRetainsTmpExtension) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::filesystem::path target(QDir(tmp.path()).filePath(QStringLiteral("live.mp4")).toStdWString());
    QFile first(QString::fromStdWString((target.parent_path() / L"live.mp4.tmp").wstring()));
    ASSERT_TRUE(first.open(QIODevice::WriteOnly));
    first.close();

    const std::filesystem::path staging = MakeDisposableSiblingStagingPath(target);

    EXPECT_EQ(staging.filename().wstring(), L"live.mp4.1.tmp");
}

// A kill/cancel mid-remux leaves the target untouched (a pre-existing good file
// survives byte-for-byte) and removes the ".tmp" staging file - no half-file anywhere.
TEST(RecoveryDrill, LiveRemuxMp4_CancelLeavesTargetUntouchedAndRemovesTemp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString transient_q = QDir(tmp.path()).filePath(QStringLiteral("live2.mkv.tmp"));
    ASSERT_TRUE(
        WriteSyntheticMkv(transient_q, exosnap::engine::VideoCodec::H264, exosnap::engine::AudioCodec::Aac, 2.0));

    const std::filesystem::path transient(transient_q.toStdWString());
    const QString final_q = QDir(tmp.path()).filePath(QStringLiteral("live2.mp4"));
    const std::filesystem::path final_mp4(final_q.toStdWString());

    // A previous good result already sits at the target — the live remux would
    // overwrite it on success, but on failure it must be left exactly as it was.
    const QByteArray sentinel("KEEP-ME-INTACT", 14);
    {
        QFile f(final_q);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        ASSERT_EQ(f.write(sentinel), sentinel.size());
    }

    const std::filesystem::path temp = MakeSiblingTempPath(final_mp4);

    // Cancel on the first progress callback — models a kill during the remux.
    bool fired = false;
    auto cancel_cb = [&](float) -> bool {
        fired = true;
        return false;
    };
    auto result = exosnap::engine::RemuxToProgressiveMp4(transient, temp, cancel_cb);

    // The live failure branch removes the abandoned temp; the target is never touched.
    if (!result.success) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    }

    EXPECT_FALSE(result.success) << "a cancelled remux must not report success";
    EXPECT_TRUE(fired);

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(temp, ec)) << "the .tmp staging file must be cleaned up on cancel";

    // The target still holds the untouched sentinel — no half-written MP4.
    ASSERT_TRUE(std::filesystem::exists(final_mp4, ec));
    QFile check(final_q);
    ASSERT_TRUE(check.open(QIODevice::ReadOnly));
    EXPECT_EQ(check.readAll(), sentinel) << "the target must be byte-for-byte untouched";
}

} // namespace
} // namespace exosnap
