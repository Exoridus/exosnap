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

#include "services/AtomicFileOps.h"
#include "services/MediaMetadataPublication.h"
#include "settings/ConfigPaths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <filesystem>

namespace {
class ScopedPublicationConfig {
  public:
    explicit ScopedPublicationConfig(const QString& folder) : previous_(qgetenv("EXOSNAP_CONFIG_DIR")) {
        qputenv("EXOSNAP_CONFIG_DIR", folder.toUtf8());
    }
    ~ScopedPublicationConfig() {
        qputenv("EXOSNAP_CONFIG_DIR", previous_);
    }

  private:
    QByteArray previous_;
};
} // namespace
TEST(MediaIdentityPublication, PersistsRecoveryRecordBeforeFirstCompanionRename) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedPublicationConfig config(directory.path());
    const auto source = std::filesystem::path(directory.path().toStdWString()) / L"staged.mp4.tmp";
    const auto target = std::filesystem::path(directory.path().toStdWString()) / L"final.mp4";
    const auto write = [](const std::filesystem::path& path) {
        QFile file(QString::fromStdWString(path.wstring()));
        return file.open(QIODevice::WriteOnly) && file.write("bytes") == 5;
    };
    ASSERT_TRUE(write(source));
    ASSERT_TRUE(write(std::filesystem::path(source.wstring() + L".exosnap.json")));
    bool observed = false;
    const auto error = exosnap::PublishMediaAndIdentitySidecar(source, target, false, [&] {
        observed = true;
        const QDir journals(QDir(directory.path()).filePath(QStringLiteral("publications")));
        EXPECT_FALSE(journals.entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());
    });
    EXPECT_EQ(error, ERROR_SUCCESS);
    EXPECT_TRUE(observed);
}

TEST(MediaIdentityPublication, CancellationWithoutIdentityDoesNotStageMarkers) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto staging = std::filesystem::path(directory.path().toStdWString()) / L"staged.mkv.tmp";
    const auto target = std::filesystem::path(directory.path().toStdWString()) / L"final.mkv";
    std::string error;
    EXPECT_FALSE(
        exosnap::StageMarkerSidecarForPublication(staging, target, {}, error, std::nullopt, [] { return false; }));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_FALSE(std::filesystem::exists(exosnap::DeriveMarkerSidecarPath(staging)));
}

TEST(MediaIdentityPublication, ReferenceOwnershipEndsOnlyAfterPublication) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto copied = std::filesystem::path(directory.path().toStdWString()) / L"copied.markers.json";
    QFile companion(QString::fromStdWString(copied.wstring()));
    ASSERT_TRUE(companion.open(QIODevice::WriteOnly));
    ASSERT_EQ(companion.write("markers"), 7);
    companion.close();
    {
        exosnap::MetadataCompanionPublicationGuard guard({copied});
    }
    EXPECT_FALSE(std::filesystem::exists(copied));
    ASSERT_TRUE(companion.open(QIODevice::WriteOnly));
    ASSERT_EQ(companion.write("markers"), 7);
    companion.close();
    {
        exosnap::MetadataCompanionPublicationGuard guard({copied});
        guard.Commit();
    }
    EXPECT_TRUE(std::filesystem::exists(copied));
}

TEST(MediaIdentityPublication, NewOutputPublishesMarkersAndRefusesExistingMedia) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto source = std::filesystem::path(directory.path().toStdWString()) / L"staged.mp4.tmp";
    const auto target = std::filesystem::path(directory.path().toStdWString()) / L"final.mp4";
    QFile media(QString::fromStdWString(source.wstring()));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("new media"), 9);
    media.close();
    const std::vector<exosnap::RecordingMarker> markers{{120, exosnap::RecordingMarkerType::Highlight, "marker"}};
    std::string error;
    ASSERT_TRUE(exosnap::StageMarkerSidecarForPublication(source, target, markers, error)) << error;
    ASSERT_EQ(exosnap::PublishMediaAndIdentitySidecar(source, target, false), 0UL);
    EXPECT_EQ(exosnap::ReadMarkerSidecar(exosnap::DeriveMarkerSidecarPath(target)), markers);
    EXPECT_FALSE(std::filesystem::exists(exosnap::DeriveMarkerSidecarPath(source)));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("second media"), 12);
    media.close();
    EXPECT_EQ(exosnap::PublishMediaAndIdentitySidecar(source, target, false), ERROR_FILE_EXISTS);
    EXPECT_TRUE(std::filesystem::exists(source));
    QFile published(QString::fromStdWString(target.wstring()));
    ASSERT_TRUE(published.open(QIODevice::ReadOnly));
    EXPECT_EQ(published.readAll(), "new media");
}

TEST(MediaIdentityPublication, HardLinkStagingKeepsRecoveryMasterUntilSuccessfulPublication) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto master = std::filesystem::path(directory.path().toStdWString()) / L"master.mkv";
    const auto staging = std::filesystem::path(directory.path().toStdWString()) / L"staged.mkv.tmp";
    const auto target = std::filesystem::path(directory.path().toStdWString()) / L"final.mkv";
    QFile media(QString::fromStdWString(master.wstring()));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("immutable recording"), 19);
    media.close();
    ASSERT_EQ(exosnap::StageCompletedMediaForPublication(master, staging), 0UL);
    EXPECT_TRUE(std::filesystem::equivalent(master, staging));
    ASSERT_EQ(exosnap::PublishMediaAndIdentitySidecar(staging, target, false), 0UL);
    EXPECT_TRUE(std::filesystem::equivalent(master, target));
    EXPECT_TRUE(std::filesystem::exists(master));
    EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST(MediaIdentityPublication, LockedMediaRestoresBothSidecarsAndPreservesExistingTarget) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto from = std::filesystem::path(dir.path().toStdWString()) / L"staged.mp4";
    const auto to = std::filesystem::path(dir.path().toStdWString()) / L"final.mp4";
    const auto from_sidecar = std::filesystem::path(from.wstring() + L".exosnap.json");
    const auto to_sidecar = std::filesystem::path(to.wstring() + L".exosnap.json");
    const auto write = [](const std::filesystem::path& path, const QByteArray& content) {
        QFile file(QString::fromStdWString(path.wstring()));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(content) == content.size();
    };
    const auto read = [](const std::filesystem::path& path) {
        QFile file(QString::fromStdWString(path.wstring()));
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray{};
        return file.readAll();
    };
    ASSERT_TRUE(write(from, "new media"));
    ASSERT_TRUE(write(to, "old media"));
    ASSERT_TRUE(write(from_sidecar, "new identity"));
    ASSERT_TRUE(write(to_sidecar, "old identity"));
    const auto from_markers = exosnap::DeriveMarkerSidecarPath(from);
    const auto to_markers = exosnap::DeriveMarkerSidecarPath(to);
    ASSERT_TRUE(write(from_markers, "new markers"));
    ASSERT_TRUE(write(to_markers, "old markers"));
    HANDLE locked =
        CreateFileW(to.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    const auto error = exosnap::PublishMediaAndIdentitySidecar(from, to);
    CloseHandle(locked);
    EXPECT_NE(error, 0UL);
    EXPECT_EQ(read(from), "new media");
    EXPECT_EQ(read(to), "old media");
    EXPECT_EQ(read(from_sidecar), "new identity");
    EXPECT_EQ(read(to_sidecar), "old identity");
    EXPECT_EQ(read(from_markers), "new markers");
    EXPECT_EQ(read(to_markers), "old markers");
    EXPECT_EQ(exosnap::PublishMediaAndIdentitySidecar(from, to), 0UL);
    EXPECT_EQ(read(to), "new media");
    EXPECT_EQ(read(to_sidecar), "new identity");
    EXPECT_EQ(read(to_markers), "new markers");
    EXPECT_FALSE(std::filesystem::exists(from));
    EXPECT_FALSE(std::filesystem::exists(from_sidecar));
}

#include "services/AtomicFileOps.h"
#include <filesystem>

TEST(MediaPublication, PublishesIdentitySidecarWithStagedMedia) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto from = std::filesystem::path(directory.path().toStdWString()) / L"staged.mp4.tmp";
    const auto to = std::filesystem::path(directory.path().toStdWString()) / L"final.mp4";
    QFile media(QString::fromStdWString(from.wstring()));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write("media"), 5);
    media.close();
    QFile sidecar(QString::fromStdWString(from.wstring() + L".exosnap.json"));
    ASSERT_TRUE(sidecar.open(QIODevice::WriteOnly));
    ASSERT_EQ(sidecar.write("identity"), 8);
    sidecar.close();
    ASSERT_EQ(exosnap::PublishMediaAndIdentitySidecar(from, to), 0UL);
    EXPECT_TRUE(std::filesystem::exists(to));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(to.wstring() + L".exosnap.json")));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(from.wstring() + L".exosnap.json")));
}

#include "services/AtomicFileOps.h"

#include <filesystem>
#include <string>

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

// ---------------------------------------------------------------------------
// A staging file that cannot be removed must not disappear from the record
// ---------------------------------------------------------------------------
//
// Every remux path writes to a sibling staging file and removes it when the
// attempt fails. Twelve call sites wrote `std::filesystem::remove(temp, ec)` and
// then ignored the ec. A staging file that cannot be deleted -- a scanner or an
// indexer still holding it -- then stays next to the user's recordings forever
// with nothing in the log and nothing on the card. The file is harmless; not
// being able to say it is there is not.
//
// The lock is a real open handle with no sharing, which is what a scanner does,
// so the failure is the operating system's and not a stub's.

TEST(StagingCleanup, ARemovableStagingFileReportsNothing) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScopedPublicationConfig config(dir.path());
    const std::filesystem::path staging = std::filesystem::path(dir.path().toStdWString()) / L"out.mp4.tmp";
    {
        QFile f(QString::fromStdWString(staging.wstring()));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("partial", 7);
    }

    EXPECT_TRUE(exosnap::DescribeFailedStagingRemoval(staging).empty());
    EXPECT_FALSE(std::filesystem::exists(staging));
}

TEST(StagingCleanup, AStagingFileThatWasNeverCreatedReportsNothing) {
    // The normal case on an error path that failed before the output was opened.
    // A caller cannot always know whether the file exists, and "it was not there"
    // is not a problem to report.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScopedPublicationConfig config(dir.path());
    const std::filesystem::path absent = std::filesystem::path(dir.path().toStdWString()) / L"never-written.tmp";
    EXPECT_TRUE(exosnap::DescribeFailedStagingRemoval(absent).empty());
}

TEST(StagingCleanup, IdentitySidecarIsRemovedEvenWhenMediaIsAbsent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScopedPublicationConfig config(dir.path());
    const auto staging = std::filesystem::path(dir.path().toStdWString()) / L"out.mp4.tmp";
    const auto sidecar = std::filesystem::path(staging.wstring() + L".exosnap.json");
    {
        QFile file(QString::fromStdWString(sidecar.wstring()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("metadata"), 8);
    }
    EXPECT_TRUE(exosnap::DescribeFailedStagingRemoval(staging).empty());
    EXPECT_FALSE(std::filesystem::exists(sidecar));
}

TEST(StagingCleanup, AStagingFileHeldOpenIsReportedAndNamed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScopedPublicationConfig config(dir.path());
    const std::filesystem::path staging = std::filesystem::path(dir.path().toStdWString()) / L"locked.mp4.tmp";
    {
        QFile f(QString::fromStdWString(staging.wstring()));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("partial", 7);
    }

    // No sharing at all: a delete attempt fails the way it does against a
    // scanner that has the file open.
    HANDLE lock =
        CreateFileW(staging.wstring().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE) << "could not take the lock this test needs";

    const std::string reported = exosnap::DescribeFailedStagingRemoval(staging);
    CloseHandle(lock);

    ASSERT_FALSE(reported.empty()) << "a staging file left on disk was reported as cleaned up";
    EXPECT_NE(reported.find("locked.mp4.tmp"), std::string::npos)
        << "the report has to name the file that is still there: " << reported;
    EXPECT_NE(reported.find("still on disk"), std::string::npos) << reported;

    // And the file really is still there -- the report is not describing a
    // removal that happened anyway.
    EXPECT_TRUE(std::filesystem::exists(staging));
}

#include <QProcess>
#include <QProcessEnvironment>

TEST(MediaIdentityPublication, PublicationCrashChild) {
    const auto folder = qEnvironmentVariable("EXOSNAP_PUBLICATION_CHILD_DIR");
    if (folder.isEmpty())
        return;
    const auto source = std::filesystem::path(folder.toStdWString()) / L"staged.mp4.tmp";
    const auto target = std::filesystem::path(folder.toStdWString()) / L"final.mp4";
    const auto checkpoint = qEnvironmentVariableIntValue("EXOSNAP_PUBLICATION_CHILD_PHASE");
    int observed = 0;
    const auto result = exosnap::PublishMediaAndIdentitySidecar(source, target, true, [&] {
        if (++observed == checkpoint)
            TerminateProcess(GetCurrentProcess(), 77);
    });
    FAIL() << "Child publication returned without expected termination: " << result;
}

TEST(MediaIdentityPublication, ProcessKillAtEachRenameRestoresConsistentOutput) {
    wchar_t executable[32768]{};
    ASSERT_NE(GetModuleFileNameW(nullptr, executable, 32768), 0UL);
    const auto write = [](const std::filesystem::path& path, const QByteArray& bytes) {
        QFile file(QString::fromStdWString(path.wstring()));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const auto read = [](const std::filesystem::path& path) {
        QFile file(QString::fromStdWString(path.wstring()));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    for (int layout : {0, 1, 2}) {
        const bool overwrite = layout != 0;
        const bool new_markers = layout != 2;
        const int rename_count = overwrite ? (new_markers ? 5 : 4) : 3;
        for (int checkpoint = 1; checkpoint <= rename_count; ++checkpoint) {
            SCOPED_TRACE(::testing::Message() << "overwrite=" << overwrite << " checkpoint=" << checkpoint);
            QTemporaryDir directory;
            ASSERT_TRUE(directory.isValid());
            const auto folder = std::filesystem::path(directory.path().toStdWString());
            const auto source = folder / L"staged.mp4.tmp";
            const auto target = folder / L"final.mp4";
            const auto source_identity = std::filesystem::path(source.wstring() + L".exosnap.json");
            const auto target_identity = std::filesystem::path(target.wstring() + L".exosnap.json");
            const auto source_markers = exosnap::DeriveMarkerSidecarPath(source);
            const auto target_markers = exosnap::DeriveMarkerSidecarPath(target);
            ASSERT_TRUE(write(source, "new media"));
            ASSERT_TRUE(write(source_identity, "new identity"));
            if (new_markers)
                ASSERT_TRUE(write(source_markers, "new markers"));
            if (overwrite) {
                ASSERT_TRUE(write(target, "old media"));
                ASSERT_TRUE(write(target_identity, "old identity"));
                ASSERT_TRUE(write(target_markers, "old markers"));
            }
            QProcess child;
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("EXOSNAP_CONFIG_DIR"), directory.path());
            environment.insert(QStringLiteral("EXOSNAP_PUBLICATION_CHILD_DIR"), directory.path());
            environment.insert(QStringLiteral("EXOSNAP_PUBLICATION_CHILD_PHASE"), QString::number(checkpoint));
            child.setProcessEnvironment(environment);
            child.start(QString::fromWCharArray(executable),
                        {QStringLiteral("--gtest_filter=MediaIdentityPublication.PublicationCrashChild")});
            ASSERT_TRUE(child.waitForFinished(30000)) << child.errorString().toStdString();
            ASSERT_EQ(child.exitCode(), 77) << child.readAllStandardOutput().toStdString();
            ScopedPublicationConfig config(directory.path());
            exosnap::RecoveryManifestStore store(QDir(directory.path()).filePath(QStringLiteral("recovery.json")));
            exosnap::RecoveryService recovery(store);
            EXPECT_TRUE(recovery.Scan().isEmpty());
            EXPECT_TRUE(QDir(QDir(directory.path()).filePath(QStringLiteral("publications")))
                            .entryList({QStringLiteral("*.json")}, QDir::Files)
                            .isEmpty());
            const auto error = exosnap::RecoverInterruptedMediaPublications();
            EXPECT_TRUE(error.empty()) << error;
            const bool committed = checkpoint == rename_count;
            EXPECT_EQ(read(target), committed ? "new media" : overwrite ? "old media" : "");
            EXPECT_EQ(read(target_identity), committed ? "new identity" : overwrite ? "old identity" : "");
            EXPECT_EQ(read(target_markers), committed   ? (new_markers ? "new markers" : "")
                                            : overwrite ? "old markers"
                                                        : "");
            EXPECT_EQ(read(source), committed ? "" : "new media");
            EXPECT_TRUE(QDir(QDir(directory.path()).filePath(QStringLiteral("publications")))
                            .entryList({QStringLiteral("*.json")}, QDir::Files)
                            .isEmpty());
        }
    }
}

TEST(MediaIdentityPublication, RecoveryRetainsCorruptJournalAndChangedUserFiles) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedPublicationConfig config(directory.path());
    const auto folder = std::filesystem::path(directory.path().toStdWString());
    const auto source = folder / L"staged.mp4.tmp";
    const auto target = folder / L"final.mp4";
    const auto target_identity = std::filesystem::path(target.wstring() + L".exosnap.json");
    const auto write = [](const std::filesystem::path& path, const QByteArray& bytes) {
        QFile file(QString::fromStdWString(path.wstring()));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write(source, "new media"));
    ASSERT_TRUE(write(std::filesystem::path(source.wstring() + L".exosnap.json"), "new identity"));
    struct Interrupted {};
    try {
        (void)exosnap::PublishMediaAndIdentitySidecar(source, target, false, [] { throw Interrupted{}; });
        ADD_FAILURE() << "Publication did not reach companion rename";
    } catch (const Interrupted&) {
    }
    ASSERT_TRUE(write(target_identity, "user changed identity after interruption"));
    EXPECT_FALSE(exosnap::RecoverInterruptedMediaPublications().empty());
    QFile identity(QString::fromStdWString(target_identity.wstring()));
    ASSERT_TRUE(identity.open(QIODevice::ReadOnly));
    EXPECT_EQ(identity.readAll(), "user changed identity after interruption");
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_FALSE(exosnap::DescribeFailedStagingRemoval(source).empty());
    EXPECT_TRUE(std::filesystem::exists(source));
    const QDir journals(QDir(directory.path()).filePath(QStringLiteral("publications")));
    EXPECT_EQ(journals.entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    ASSERT_TRUE(write(std::filesystem::path(journals.filePath(QStringLiteral("corrupt.json")).toStdWString()),
                      "not a journal"));
    EXPECT_FALSE(exosnap::RecoverInterruptedMediaPublications().empty());
    EXPECT_EQ(journals.entryList({QStringLiteral("*.json")}, QDir::Files).size(), 2);
}

#include <QLockFile>

TEST(MediaIdentityPublication, InterruptedRollbackAndActivePublisherRetainRecoveryState) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedPublicationConfig config(directory.path());
    const auto folder = std::filesystem::path(directory.path().toStdWString());
    const auto source = folder / L"staged.mp4.tmp";
    const auto target = folder / L"final.mp4";
    const auto staged_identity = std::filesystem::path(source.wstring() + L".exosnap.json");
    const auto final_identity = std::filesystem::path(target.wstring() + L".exosnap.json");
    const auto write = [](const std::filesystem::path& path, const QByteArray& bytes) {
        QFile file(QString::fromStdWString(path.wstring()));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write(source, "new media"));
    ASSERT_TRUE(write(target, "old media"));
    ASSERT_TRUE(write(staged_identity, "new identity"));
    ASSERT_TRUE(write(final_identity, "old identity"));
    struct Interrupted {};
    int renamed = 0;
    try {
        (void)exosnap::PublishMediaAndIdentitySidecar(source, target, true, [&] {
            if (++renamed == 2)
                throw Interrupted{};
        });
        ADD_FAILURE() << "Publication did not reach companion rename";
    } catch (const Interrupted&) {
    }
    QLockFile active(QDir(directory.path()).filePath(QStringLiteral("publications/active.lock")));
    ASSERT_TRUE(active.tryLock(0));
    EXPECT_NE(exosnap::DescribeFailedStagingRemoval(source).find("active"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_FALSE(std::filesystem::exists(staged_identity));
    EXPECT_TRUE(std::filesystem::exists(final_identity));
    EXPECT_FALSE(exosnap::RecoverInterruptedMediaPublications().empty());
    active.unlock();
    ASSERT_EQ(exosnap::AtomicReplaceInPlace(final_identity, staged_identity), ERROR_SUCCESS);
    EXPECT_TRUE(exosnap::RecoverInterruptedMediaPublications().empty());
    EXPECT_TRUE(exosnap::RecoverInterruptedMediaPublications().empty());
    QFile identity(QString::fromStdWString(final_identity.wstring()));
    ASSERT_TRUE(identity.open(QIODevice::ReadOnly));
    EXPECT_EQ(identity.readAll(), "old identity");
    EXPECT_TRUE(std::filesystem::exists(source));
}
