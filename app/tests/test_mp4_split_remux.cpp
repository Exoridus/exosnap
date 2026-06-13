// MP4-SPLIT-REMUX-R1 focused tests
//
// Tests cover:
//   1. Disk threshold computation: base threshold, additive reserve
//   2. Per-segment manifest lifecycle: Add → UpdateFinalized → Remove
//   3. Crash before remux (finalized=false entry survives)
//   4. Crash during remux (finalized=true entry survives)
//   5. Three-segment independent lifecycle
//   6. Single-file MP4 session lifecycle (regression guard)
//   7. Segment path derivation for .mkv.tmp convention
//   8. DeriveTransientMkvPath produces .mkv.tmp
//   9. Output settings split mode propagation through coordinator
//  10. IsSplitPending / IsRemuxing coordinator state guards
//
// No real GPU, D3D11, or NVENC access required.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QString>

#include <filesystem>

#include <recorder_core/recorder_session.h>

#include "diagnostics/DiskSpaceThresholds.h"
#include "models/OutputSettingsModel.h"
#include "services/RecordingCoordinator.h"
#include "settings/RecoveryManifestStore.h"

namespace exosnap {
namespace {

// ─── QCoreApplication init ────────────────────────────────────────────────────

class Mp4SplitRemuxTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "mp4_split_remux_tests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }
};

// ─── Helper: unique temp store path ──────────────────────────────────────────

static QString UniqueTempStorePath() {
    const QString temp = QDir::tempPath();
    static int s_counter = 0;
    return QDir(temp).filePath(QStringLiteral("exosnap_mp4split_test_%1.json").arg(++s_counter));
}

static RecoveryManifestEntry MakeManifestEntry(const QString& id, const QString& artefact, const QString& container,
                                               bool finalized = false) {
    RecoveryManifestEntry e;
    e.id = id;
    e.artefact_path = artefact;
    e.intended_container = container;
    e.final_output_path = artefact;
    e.started_at = QStringLiteral("2026-06-13T00:00:00Z");
    e.finalized = finalized;
    return e;
}

// ─── 1. ComputeHardStopThreshold: base equals kHardStopFreeBytes ─────────────

TEST_F(Mp4SplitRemuxTest, ComputeHardStopThreshold_ZeroReserve_EqualsBase) {
    EXPECT_EQ(diagnostics::ComputeHardStopThreshold(0), diagnostics::kHardStopFreeBytes);
}

// ─── 2. ComputeHardStopThreshold: reserve added to base ──────────────────────

TEST_F(Mp4SplitRemuxTest, ComputeHardStopThreshold_AddsReserve) {
    const uint64_t reserve = 500ULL * 1024 * 1024; // 500 MB
    const uint64_t expected = diagnostics::kHardStopFreeBytes + reserve;
    EXPECT_EQ(diagnostics::ComputeHardStopThreshold(reserve), expected);
}

// ─── 3. Disk threshold: conservative sum for two pending segments ─────────────
//
// When two segments are awaiting remux (e.g., 300 MB + 200 MB) plus the current
// live segment (300 MB estimated), the threshold grows by 800 MB total.

TEST_F(Mp4SplitRemuxTest, ComputeHardStopThreshold_TwoSegmentsPlusCurrent) {
    const uint64_t seg0_bytes = 300ULL * 1024 * 1024;
    const uint64_t seg1_bytes = 200ULL * 1024 * 1024;
    const uint64_t current_bytes = 300ULL * 1024 * 1024;
    const uint64_t reserve = seg0_bytes + seg1_bytes + current_bytes; // 800 MB

    const uint64_t threshold = diagnostics::ComputeHardStopThreshold(reserve);
    EXPECT_EQ(threshold, diagnostics::kHardStopFreeBytes + reserve);

    // A machine with only 1.2 GB free would be stopped (threshold ~1.3 GB).
    const uint64_t free_1200mb = 1200ULL * 1024 * 1024;
    EXPECT_GT(threshold, free_1200mb);
}

// ─── 4. Per-segment manifest lifecycle: Add → UpdateFinalized → Remove ────────

TEST_F(Mp4SplitRemuxTest, ManifestLifecycle_AddFinalizeRemove) {
    const QString store_path = UniqueTempStorePath();
    RecoveryManifestStore store(store_path);

    // Segment 0: created before recording starts, finalized=false.
    ASSERT_TRUE(store.Add(
        MakeManifestEntry(QStringLiteral("seg0"), QStringLiteral("/tmp/rec.mkv.tmp"), QStringLiteral("mp4"))));
    EXPECT_EQ(store.Entries().size(), 1);
    EXPECT_FALSE(store.Entries()[0].finalized);

    // Segment 0 MKV closed: mark finalized before remux starts.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("seg0"), true));
    EXPECT_TRUE(store.Entries()[0].finalized);

    // Segment 1: created when segment 0 completes.
    ASSERT_TRUE(store.Add(
        MakeManifestEntry(QStringLiteral("seg1"), QStringLiteral("/tmp/rec.mkv_part-002.tmp"), QStringLiteral("mp4"))));
    EXPECT_EQ(store.Entries().size(), 2);

    // Segment 0 remux success: remove its entry.
    ASSERT_TRUE(store.Remove(QStringLiteral("seg0")));
    EXPECT_EQ(store.Entries().size(), 1);
    EXPECT_EQ(store.Entries()[0].id, QStringLiteral("seg1"));

    // Segment 1 lifecycle completes.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("seg1"), true));
    ASSERT_TRUE(store.Remove(QStringLiteral("seg1")));
    EXPECT_TRUE(store.Entries().isEmpty());

    QFile::remove(store_path);
}

// ─── 5. Crash before finalize: finalized=false entry survives ─────────────────

TEST_F(Mp4SplitRemuxTest, ManifestLifecycle_CrashBeforeFinalize_EntryPersists) {
    const QString store_path = UniqueTempStorePath();
    {
        RecoveryManifestStore store(store_path);
        ASSERT_TRUE(store.Add(
            MakeManifestEntry(QStringLiteral("live"), QStringLiteral("/tmp/live.mkv.tmp"), QStringLiteral("mp4"))));
        // Crash: no UpdateFinalized, no Remove.
    }

    RecoveryManifestStore store2(store_path);
    const auto entries = store2.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_FALSE(entries[0].finalized);
    EXPECT_EQ(entries[0].intended_container, QStringLiteral("mp4"));

    QFile::remove(store_path);
}

// ─── 6. Crash during remux: finalized=true entry survives ────────────────────

TEST_F(Mp4SplitRemuxTest, ManifestLifecycle_CrashDuringRemux_FinalizedEntryPersists) {
    const QString store_path = UniqueTempStorePath();
    {
        RecoveryManifestStore store(store_path);
        ASSERT_TRUE(store.Add(
            MakeManifestEntry(QStringLiteral("seg0"), QStringLiteral("/tmp/rec.mkv.tmp"), QStringLiteral("mp4"))));
        ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("seg0"), true));
        // Crash during remux: do NOT call Remove.
    }

    RecoveryManifestStore store2(store_path);
    const auto entries = store2.Entries();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].id, QStringLiteral("seg0"));
    EXPECT_TRUE(entries[0].finalized);

    QFile::remove(store_path);
}

// ─── 7. Three-segment independent lifecycle ───────────────────────────────────

TEST_F(Mp4SplitRemuxTest, ManifestLifecycle_ThreeSegmentsIndependent) {
    const QString store_path = UniqueTempStorePath();
    RecoveryManifestStore store(store_path);

    // Segment 0 created at StartRecording.
    ASSERT_TRUE(
        store.Add(MakeManifestEntry(QStringLiteral("s0"), QStringLiteral("/tmp/rec.mkv.tmp"), QStringLiteral("mp4"))));

    // Segment 0 completes: finalize + create segment 1 entry.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("s0"), true));
    ASSERT_TRUE(store.Add(
        MakeManifestEntry(QStringLiteral("s1"), QStringLiteral("/tmp/rec.mkv_part-002.tmp"), QStringLiteral("mp4"))));
    EXPECT_EQ(store.Entries().size(), 2);

    // Segment 0 remux succeeds: remove s0.
    ASSERT_TRUE(store.Remove(QStringLiteral("s0")));
    EXPECT_EQ(store.Entries().size(), 1);

    // Segment 1 completes: finalize + create segment 2 entry.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("s1"), true));
    ASSERT_TRUE(store.Add(
        MakeManifestEntry(QStringLiteral("s2"), QStringLiteral("/tmp/rec.mkv_part-003.tmp"), QStringLiteral("mp4"))));
    EXPECT_EQ(store.Entries().size(), 2);

    // Segment 1 remux succeeds: remove s1.
    ASSERT_TRUE(store.Remove(QStringLiteral("s1")));
    EXPECT_EQ(store.Entries().size(), 1);
    EXPECT_EQ(store.Entries()[0].id, QStringLiteral("s2"));

    // Session ends. Segment 2 finalized and remux succeeds.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("s2"), true));
    ASSERT_TRUE(store.Remove(QStringLiteral("s2")));
    EXPECT_TRUE(store.Entries().isEmpty());

    QFile::remove(store_path);
}

// ─── 8. Single-file MP4 session lifecycle (regression guard) ─────────────────

TEST_F(Mp4SplitRemuxTest, ManifestLifecycle_SingleFileMp4_FullLifecycle) {
    const QString store_path = UniqueTempStorePath();
    RecoveryManifestStore store(store_path);

    RecoveryManifestEntry e;
    e.id = QStringLiteral("single-session");
    e.artefact_path = QStringLiteral("/tmp/output.mkv.tmp");
    e.intended_container = QStringLiteral("mp4");
    e.final_output_path = QStringLiteral("/tmp/output.mp4");
    e.started_at = QStringLiteral("2026-06-13T00:00:00Z");
    e.finalized = false;
    ASSERT_TRUE(store.Add(e));

    // Engine stops: MKV is clean; finalize before remux.
    ASSERT_TRUE(store.UpdateFinalized(QStringLiteral("single-session"), true));
    EXPECT_TRUE(store.Entries()[0].finalized);

    // Remux succeeds: remove entry.
    ASSERT_TRUE(store.Remove(QStringLiteral("single-session")));
    EXPECT_TRUE(store.Entries().isEmpty());

    QFile::remove(store_path);
}

// ─── 9. DeriveSegmentPath for .mkv.tmp follows expected naming ───────────────

TEST_F(Mp4SplitRemuxTest, DeriveSegmentPath_MkvTmpConvention) {
    using recorder_core::DeriveSegmentPath;
    using recorder_core::DeriveTransientMkvPath;

    const std::filesystem::path mp4 = std::filesystem::temp_directory_path() / L"recording.mp4";
    const std::filesystem::path transient = DeriveTransientMkvPath(mp4);

    // Segment 0 keeps the base path.
    EXPECT_EQ(DeriveSegmentPath(transient, 0), transient);

    // Segment 1 differs from base and has same extension (.tmp).
    const std::filesystem::path seg1 = DeriveSegmentPath(transient, 1);
    EXPECT_NE(seg1, transient);
    EXPECT_EQ(seg1.extension(), transient.extension());

    // Corresponding MP4 segment 0 is the base path; segment 1 has .mp4.
    const std::filesystem::path mp4_seg0 = DeriveSegmentPath(mp4, 0);
    const std::filesystem::path mp4_seg1 = DeriveSegmentPath(mp4, 1);
    EXPECT_EQ(mp4_seg0, mp4);
    EXPECT_EQ(mp4_seg1.extension(), std::filesystem::path(L".mp4"));
    EXPECT_NE(mp4_seg1, mp4);
}

// ─── 10. DeriveTransientMkvPath produces .mkv.tmp suffix ─────────────────────

TEST_F(Mp4SplitRemuxTest, DeriveTransientMkvPath_HasMkvTmpSuffix) {
    const std::filesystem::path mp4 = std::filesystem::temp_directory_path() / L"my_recording.mp4";
    const std::filesystem::path transient = recorder_core::DeriveTransientMkvPath(mp4);

    EXPECT_EQ(transient.extension(), std::filesystem::path(L".tmp"));
    const std::wstring full = transient.wstring();
    ASSERT_GE(full.size(), 8u);
    EXPECT_EQ(full.substr(full.size() - 8), L".mkv.tmp");
}

// ─── 11. Split mode Off propagates to coordinator ────────────────────────────

TEST_F(Mp4SplitRemuxTest, SetOutputSettings_SplitModeOff_PropagatesCorrectly) {
    RecordingCoordinator coordinator;
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Off;
    coordinator.SetOutputSettings(settings);

    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.mode, recorder_core::RecordingSplitMode::Off);
}

// ─── 12. Split mode 15Min propagates to coordinator ──────────────────────────

TEST_F(Mp4SplitRemuxTest, SetOutputSettings_SplitMode15Min_PropagatesCorrectly) {
    RecordingCoordinator coordinator;
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Every15Min;
    coordinator.SetOutputSettings(settings);

    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.mode, recorder_core::RecordingSplitMode::Duration);
    EXPECT_EQ(split.duration_ms, 15ULL * 60 * 1000);
}

// ─── 13. Split mode Custom propagates custom minutes ─────────────────────────

TEST_F(Mp4SplitRemuxTest, SetOutputSettings_SplitModeCustom_PropagatesCorrectly) {
    RecordingCoordinator coordinator;
    OutputSettingsModel settings = OutputSettingsModel::Defaults();
    settings.container = capability::Container::Mp4;
    settings.split.mode = SplitRecordingMode::Custom;
    settings.split.custom_minutes = 42;
    coordinator.SetOutputSettings(settings);

    const auto split = coordinator.SplitSettings();
    EXPECT_EQ(split.mode, recorder_core::RecordingSplitMode::Duration);
    EXPECT_EQ(split.duration_ms, 42ULL * 60 * 1000);
}

// ─── 14. IsSplitPending returns false when no session is active ──────────────

TEST_F(Mp4SplitRemuxTest, IsSplitPending_FalseWhenNoSessionActive) {
    RecordingCoordinator coordinator;
    EXPECT_FALSE(coordinator.IsSplitPending());
}

// ─── 15. IsRemuxing and CancelRemux are safe to call when idle ───────────────

TEST_F(Mp4SplitRemuxTest, CancelRemux_SafeWhenIdle) {
    RecordingCoordinator coordinator;
    EXPECT_FALSE(coordinator.IsRemuxing());
    coordinator.CancelRemux();
    EXPECT_FALSE(coordinator.IsRemuxing());
}

} // namespace
} // namespace exosnap
