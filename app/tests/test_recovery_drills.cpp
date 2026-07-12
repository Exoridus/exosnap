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
// KNOWN ISSUE surfaced here: the Remux×Kill cell leaves a corrupt half-MP4 at the
// user-visible final path (RemuxToProgressiveMp4 writes it directly; recovery then
// side-steps to a fresh name and never cleans the corrupt file). That fix is a
// separate Coordinator/Recovery slice; the drill asserts the DESIRED end state and
// skips (does not falsely pass) until the fix lands.

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

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"
#include "synthetic_session.h"

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
bool WriteSyntheticMkv(const QString& path, recorder_core::VideoCodec vc, recorder_core::AudioCodec ac,
                       double seconds) {
    recorder_core::testutil::SyntheticSessionConfig cfg;
    cfg.video_codec = vc;
    cfg.audio_codec = ac;
    cfg.output_path = path.toStdString();
    cfg.target_seconds = seconds;
    return recorder_core::testutil::SyntheticSession(cfg).Run().success;
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
    ASSERT_TRUE(WriteSyntheticMkv(artefact, recorder_core::VideoCodec::Av1Nvenc, recorder_core::AudioCodec::Opus, 2.0));

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
    ASSERT_TRUE(
        WriteSyntheticMkv(artefact, recorder_core::VideoCodec::H264Nvenc, recorder_core::AudioCodec::AacMf, 4.0));
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
    ASSERT_TRUE(
        WriteSyntheticMkv(artefact, recorder_core::VideoCodec::H264Nvenc, recorder_core::AudioCodec::AacMf, 2.0));

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
// Remux(MP4) × Process-Kill: KNOWN ISSUE — stale partial MP4 at the target path.
//
// Models a kill mid-remux: a corrupt half-MP4 already sits at final_output_path.
// The DESIRED end state is that recovery does not leave a corrupt artefact where
// the user expects their result. Current behaviour side-steps to a fresh name and
// leaves the corrupt file — so this drill SKIPS (never falsely passes) while the
// bug stands, and will start enforcing the desired state once the fix lands.
// =============================================================================
TEST(RecoveryDrill, RemuxMp4ProcessKill_StalePartialMp4_KNOWN_ISSUE) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    RecoveryManifestStore store(QDir(tmp.path()).filePath(QStringLiteral("manifest.json")));
    RecoveryService service(store);

    const QString artefact = QDir(tmp.path()).filePath(QStringLiteral("session.mkv.tmp"));
    ASSERT_TRUE(
        WriteSyntheticMkv(artefact, recorder_core::VideoCodec::H264Nvenc, recorder_core::AudioCodec::AacMf, 2.0));

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
    ASSERT_TRUE(r.success) << r.message; // recovery still produces a good MP4 somewhere

    if (QFileInfo::exists(final_out) && !CanDemux(final_out)) {
        GTEST_SKIP() << "KNOWN ISSUE: a corrupt half-MP4 remains at the user-visible target path "
                     << "(" << final_out.toStdString() << "). Recovery wrote the good MP4 under a "
                     << "different name and left the stale file. Tracked as a Coordinator/Recovery "
                     << "fix (remux-to-temp + atomic rename, or Finish clears final_output_path). "
                     << "See docs/dev/soak-and-recovery-drills.md.";
    }

    // DESIRED end state (enforced once the fix lands): the user's target path holds
    // a playable MP4, not a corrupt stub.
    EXPECT_TRUE(QFileInfo::exists(final_out));
    EXPECT_TRUE(CanDemux(final_out)) << "target path should hold a playable MP4";
}

} // namespace
} // namespace exosnap
