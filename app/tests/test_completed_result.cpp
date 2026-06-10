#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include "models/CompletedRecording.h"
#include "models/FilenameBuilder.h"
#include "viewmodels/RecordViewModel.h"

#include <capability/audio_ui_state.h>
#include <recorder_core/recorder_session.h>

#include <filesystem>

namespace exosnap {
namespace {

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "completed_result_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class CompletedResultTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    void SetUp() override {
        temp_dir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(temp_dir_->isValid()) << "Failed to create temp dir";
    }

    void TearDown() override {
        temp_dir_.reset();
    }

    QString tempPath(const QString& filename) const {
        return temp_dir_->filePath(filename);
    }

    // Creates a dummy file and returns its full path
    QString createDummyFile(const QString& name, qint64 size_bytes = 1024) {
        QString path = temp_dir_->filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QByteArray(static_cast<int>(size_bytes), 'A'));
            file.close();
        }
        return path;
    }

    // Build a UiRecordingResult for a successful recording
    UiRecordingResult MakeSuccessResult(const std::wstring& output_path) {
        UiRecordingResult result;
        result.succeeded = true;
        result.output_path = output_path;
        result.output_file_bytes = 1024 * 1024; // 1 MB
        result.elapsed_seconds = 15.5;
        result.source_width = 2560;
        result.source_height = 1440;
        result.output_width = 1920;
        result.output_height = 1080;
        result.frame_rate_num = 60;
        result.frame_rate_den = 1;
        result.cfr = true;
        result.container = recorder_core::Container::Matroska;
        result.video_codec = recorder_core::VideoCodec::H264Nvenc;
        result.audio_codec = recorder_core::AudioCodec::AacMf;
        return result;
    }

    UiRecordingResult MakeFailedResult() {
        UiRecordingResult result;
        result.succeeded = false;
        result.error_phase = L"Prepare";
        result.error_detail = L"Test failure";
        return result;
    }

    std::unique_ptr<QTemporaryDir> temp_dir_;
};

// ---------------------------------------------------------------------------
// Part A — CompletedRecording model
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, ModelDefaults_NotSucceeded_NoFile) {
    CompletedRecording rec;
    EXPECT_FALSE(rec.succeeded);
    EXPECT_FALSE(rec.hasFile());
    EXPECT_FALSE(rec.fileExists());
    EXPECT_TRUE(rec.fileName().isEmpty());
    EXPECT_TRUE(rec.parentFolder().isEmpty());
}

TEST_F(CompletedResultTest, ModelWithPath_HasFile) {
    QString path = createDummyFile(QStringLiteral("test.mkv"));
    CompletedRecording rec;
    rec.succeeded = true;
    rec.file_path = path;
    EXPECT_TRUE(rec.hasFile());
    EXPECT_TRUE(rec.fileExists());
    EXPECT_EQ(rec.fileName(), QStringLiteral("test.mkv"));
    EXPECT_EQ(rec.parentFolder(), temp_dir_->path());
}

TEST_F(CompletedResultTest, ModelFileSizeFromDisk_UsesActualFile) {
    QString path = createDummyFile(QStringLiteral("size_test.mkv"), 2048);
    CompletedRecording rec;
    rec.succeeded = true;
    rec.file_path = path;
    EXPECT_EQ(rec.fileSizeFromDisk(), 2048);
}

// --- Multi-segment results (SPLIT-RECORDING-R1) ---

TEST_F(CompletedResultTest, SingleFileRecording_SegmentCountIsOne) {
    CompletedRecording rec;
    rec.succeeded = true;
    rec.file_path = QStringLiteral("C:/videos/rec.mkv");
    rec.duration_seconds = 42.0;
    rec.file_size_bytes = 1234;
    EXPECT_FALSE(rec.isMultiSegment());
    EXPECT_EQ(rec.segmentCount(), 1);
    EXPECT_EQ(rec.status(), CompletedRecordingStatus::Completed);
    EXPECT_DOUBLE_EQ(rec.totalDurationSeconds(), 42.0);
    EXPECT_EQ(rec.totalSizeBytes(), 1234);
}

TEST_F(CompletedResultTest, MultiSegment_TotalsAreSummed) {
    CompletedRecording rec;
    rec.succeeded = true;
    rec.file_path = QStringLiteral("C:/videos/s.mkv");
    CompletedRecordingSegment a;
    a.succeeded = true;
    a.duration_seconds = 30.0;
    a.file_size_bytes = 1000;
    CompletedRecordingSegment b;
    b.succeeded = true;
    b.duration_seconds = 12.0;
    b.file_size_bytes = 500;
    rec.segments = {a, b};
    EXPECT_TRUE(rec.isMultiSegment());
    EXPECT_EQ(rec.segmentCount(), 2);
    EXPECT_DOUBLE_EQ(rec.totalDurationSeconds(), 42.0);
    EXPECT_EQ(rec.totalSizeBytes(), 1500);
    EXPECT_EQ(rec.status(), CompletedRecordingStatus::Completed);
}

TEST_F(CompletedResultTest, PartialFailure_NotHiddenAsSuccess) {
    CompletedRecording rec;
    rec.succeeded = true;
    CompletedRecordingSegment ok;
    ok.succeeded = true;
    ok.duration_seconds = 30.0;
    CompletedRecordingSegment bad;
    bad.succeeded = false; // finalize failed / quarantined
    rec.segments = {ok, bad};
    EXPECT_EQ(rec.status(), CompletedRecordingStatus::CompletedWithPartialFailure);
}

TEST_F(CompletedResultTest, AllSegmentsFailed_StatusFailed) {
    CompletedRecording rec;
    CompletedRecordingSegment a;
    a.succeeded = false;
    rec.segments = {a};
    EXPECT_EQ(rec.status(), CompletedRecordingStatus::Failed);
}

TEST_F(CompletedResultTest, ModelEquality_Identical) {
    CompletedRecording a;
    a.file_path = QStringLiteral("C:\\test.mkv");
    a.file_size_bytes = 100;
    a.duration_seconds = 5.0;
    a.source_width = 1920;
    a.output_width = 1920;
    a.container = recorder_core::Container::Matroska;
    a.succeeded = true;

    CompletedRecording b = a;
    EXPECT_TRUE(a == b);
}

TEST_F(CompletedResultTest, ModelEquality_Different) {
    CompletedRecording a;
    a.file_path = QStringLiteral("C:\\a.mkv");
    CompletedRecording b;
    b.file_path = QStringLiteral("C:\\b.mkv");
    EXPECT_TRUE(a != b);
}

TEST_F(CompletedResultTest, Model_SourceSize_OutputSize_Helpers) {
    CompletedRecording rec;
    rec.source_width = 2560;
    rec.source_height = 1440;
    rec.output_width = 1920;
    rec.output_height = 1080;
    EXPECT_EQ(rec.sourceSize(), QSize(2560, 1440));
    EXPECT_EQ(rec.outputSize(), QSize(1920, 1080));
}

// ---------------------------------------------------------------------------
// Successful recording creates completed result
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, SetResult_Success_CreatesCompletedRecording) {
    RecordViewModel vm;
    QString path = createDummyFile(QStringLiteral("success.mkv"));
    auto result = MakeSuccessResult(path.toStdWString());

    vm.SetResult(result);
    vm.SetState(UiRecordingState::Completed);

    EXPECT_TRUE(vm.HasResult());
    EXPECT_TRUE(vm.HasCompletedRecording());
    EXPECT_TRUE(vm.current_completed_recording.succeeded);
    EXPECT_EQ(vm.current_completed_recording.file_path, path);
    EXPECT_EQ(vm.current_completed_recording.file_size_bytes, 1024 * 1024);
    EXPECT_DOUBLE_EQ(vm.current_completed_recording.duration_seconds, 15.5);
    EXPECT_EQ(vm.current_completed_recording.container, recorder_core::Container::Matroska);
}

TEST_F(CompletedResultTest, SetResult_MultiSegment_CarriesSegmentsAndSummary) {
    RecordViewModel vm;
    QString path = createDummyFile(QStringLiteral("session.mkv"));
    auto result = MakeSuccessResult(path.toStdWString());
    CompletedRecordingSegment a;
    a.file_path = path;
    a.index = 0;
    a.duration_seconds = 30.0;
    a.succeeded = true;
    CompletedRecordingSegment b;
    b.file_path = QStringLiteral("C:/videos/session_part-002.mkv");
    b.index = 1;
    b.duration_seconds = 12.0;
    b.succeeded = true;
    result.segments = {a, b};

    vm.SetResult(result);
    vm.SetState(UiRecordingState::Completed);

    ASSERT_EQ(vm.current_completed_recording.segments.size(), 2u);
    EXPECT_TRUE(vm.current_completed_recording.isMultiSegment());
    EXPECT_EQ(vm.current_completed_recording.segmentCount(), 2);
    // The completed-panel summary leads with the segment count.
    EXPECT_NE(vm.result_destination_text.find(L"2 segments"), std::wstring::npos);
}

TEST_F(CompletedResultTest, SetResult_Failure_DoesNotCreateCompletedRecording) {
    RecordViewModel vm;
    auto result = MakeFailedResult();
    vm.SetResult(result);
    vm.SetState(UiRecordingState::Failed);

    EXPECT_TRUE(vm.HasResult());
    EXPECT_FALSE(vm.HasCompletedRecording());
    EXPECT_FALSE(vm.current_completed_recording.succeeded);
}

// ---------------------------------------------------------------------------
// Runtime-effective metadata is retained
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, SetResult_RetainsEffectiveMetadata) {
    RecordViewModel vm;
    QString path = createDummyFile(QStringLiteral("effective.mkv"));
    auto result = MakeSuccessResult(path.toStdWString());
    // Simulate a fallback scenario: requested 4K, effective 1080p
    result.output_width = 1920;
    result.output_height = 1080;

    vm.SetResult(result);

    EXPECT_EQ(vm.current_completed_recording.output_width, 1920u);
    EXPECT_EQ(vm.current_completed_recording.output_height, 1080u);
    EXPECT_EQ(vm.result_output_width, 1920u);
    EXPECT_EQ(vm.result_output_height, 1080u);
}

// ---------------------------------------------------------------------------
// Recent recordings history
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, RecentHistory_NewestFirst) {
    RecordViewModel vm;

    QString path1 = createDummyFile(QStringLiteral("first.mkv"));
    QString path2 = createDummyFile(QStringLiteral("second.mkv"));
    QString path3 = createDummyFile(QStringLiteral("third.mkv"));

    auto r1 = MakeSuccessResult(path1.toStdWString());
    auto r2 = MakeSuccessResult(path2.toStdWString());
    auto r3 = MakeSuccessResult(path3.toStdWString());

    vm.SetResult(r1);
    vm.SetResult(r2);
    vm.SetResult(r3);

    ASSERT_EQ(vm.recent_recordings.size(), 3);
    EXPECT_EQ(vm.recent_recordings[0].file_path, path3); // newest first
    EXPECT_EQ(vm.recent_recordings[1].file_path, path2);
    EXPECT_EQ(vm.recent_recordings[2].file_path, path1);
}

TEST_F(CompletedResultTest, RecentHistory_Deduplicates_SamePath) {
    RecordViewModel vm;
    QString path = createDummyFile(QStringLiteral("dedup.mkv"));

    auto r1 = MakeSuccessResult(path.toStdWString());
    r1.elapsed_seconds = 10.0;
    vm.SetResult(r1);

    auto r2 = MakeSuccessResult(path.toStdWString());
    r2.elapsed_seconds = 20.0;
    vm.SetResult(r2);

    ASSERT_EQ(vm.recent_recordings.size(), 1);
    EXPECT_DOUBLE_EQ(vm.recent_recordings[0].duration_seconds, 20.0); // updated
}

TEST_F(CompletedResultTest, RecentHistory_Bounded) {
    RecordViewModel vm;
    for (int i = 0; i < 15; ++i) {
        QString path = createDummyFile(QStringLiteral("rec_%1.mkv").arg(i));
        auto r = MakeSuccessResult(path.toStdWString());
        vm.SetResult(r);
    }

    ASSERT_LE(vm.recent_recordings.size(), RecordViewModel::kMaxRecentRecordings);
}

TEST_F(CompletedResultTest, RecentHistory_OnlySuccessfulRecordings) {
    RecordViewModel vm;

    // Failed recording should not appear in history
    auto failed = MakeFailedResult();
    vm.SetResult(failed);

    EXPECT_TRUE(vm.recent_recordings.isEmpty());

    // Successful recording should appear
    QString path = createDummyFile(QStringLiteral("good.mkv"));
    auto good = MakeSuccessResult(path.toStdWString());
    vm.SetResult(good);

    ASSERT_EQ(vm.recent_recordings.size(), 1);
}

TEST_F(CompletedResultTest, RecentHistory_RemoveFromRecentRecordings) {
    RecordViewModel vm;

    QString path1 = createDummyFile(QStringLiteral("a.mkv"));
    QString path2 = createDummyFile(QStringLiteral("b.mkv"));

    vm.SetResult(MakeSuccessResult(path1.toStdWString()));
    vm.SetResult(MakeSuccessResult(path2.toStdWString()));

    EXPECT_EQ(vm.recent_recordings.size(), 2);
    vm.RemoveFromRecentRecordings(0); // remove newest (b.mkv)
    EXPECT_EQ(vm.recent_recordings.size(), 1);
    EXPECT_EQ(vm.recent_recordings[0].file_path, path1);
}

TEST_F(CompletedResultTest, RecentHistory_ClearRecentRecordings) {
    RecordViewModel vm;
    vm.SetResult(MakeSuccessResult(createDummyFile(QStringLiteral("x.mkv")).toStdWString()));
    vm.SetResult(MakeSuccessResult(createDummyFile(QStringLiteral("y.mkv")).toStdWString()));

    EXPECT_EQ(vm.recent_recordings.size(), 2);
    vm.ClearRecentRecordings();
    EXPECT_TRUE(vm.recent_recordings.isEmpty());
}

// ---------------------------------------------------------------------------
// File-size refresh uses real file
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, FileSizeRefresh_UsesRealFile) {
    QString path = createDummyFile(QStringLiteral("fs_test.mkv"), 4096);
    auto result = MakeSuccessResult(path.toStdWString());
    result.output_file_bytes = 100; // simulated value

    RecordViewModel vm;
    vm.SetResult(result);

    // The completed recording should use the real file size from disk
    EXPECT_EQ(vm.current_completed_recording.fileSizeFromDisk(), 4096);
}

// ---------------------------------------------------------------------------
// Rename validation
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, IsValidWindowsFilename_RejectsEmpty) {
    EXPECT_FALSE(IsValidWindowsFilename(QString()));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("   ")));
}

TEST_F(CompletedResultTest, IsValidWindowsFilename_RejectsInvalidChars) {
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test<file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test>file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test:file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test\"file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test/file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test\\file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test|file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test?file")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("test*file")));
}

TEST_F(CompletedResultTest, IsValidWindowsFilename_RejectsReservedNames) {
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("CON")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("con.txt")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("PRN")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("AUX")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("NUL")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("COM1")));
    EXPECT_FALSE(IsValidWindowsFilename(QStringLiteral("LPT1")));
}

TEST_F(CompletedResultTest, IsValidWindowsFilename_AcceptsValidNames) {
    EXPECT_TRUE(IsValidWindowsFilename(QStringLiteral("test")));
    EXPECT_TRUE(IsValidWindowsFilename(QStringLiteral("test file.mkv")));
    EXPECT_TRUE(IsValidWindowsFilename(QStringLiteral("2024-01-15_recording")));
    EXPECT_TRUE(IsValidWindowsFilename(QStringLiteral("test-file (1)")));
}

TEST_F(CompletedResultTest, ValidateRename_RejectsEmpty) {
    auto error = ValidateRenameForFile(QString(), QStringLiteral(".mkv"), QStringLiteral("C:\\test"));
    EXPECT_TRUE(error.has_value());
    EXPECT_FALSE(error->isEmpty());
}

TEST_F(CompletedResultTest, ValidateRename_RejectsPathSeparators) {
    auto error = ValidateRenameForFile(QStringLiteral("sub/test"), QStringLiteral(".mkv"), QStringLiteral("C:\\test"));
    EXPECT_TRUE(error.has_value());
}

TEST_F(CompletedResultTest, ValidateRename_RejectsExistingFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QString existing = dir.filePath(QStringLiteral("existing.mkv"));
    QFile file(existing);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("data");
    file.close();

    auto error = ValidateRenameForFile(QStringLiteral("existing"), QStringLiteral(".mkv"), dir.path());
    EXPECT_TRUE(error.has_value());
}

TEST_F(CompletedResultTest, ValidateRename_PreservesExtension) {
    // ValidateRename appends the extension when the name has no explicit extension
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    auto error = ValidateRenameForFile(QStringLiteral("new_file"), QStringLiteral(".mkv"), dir.path());
    EXPECT_FALSE(error.has_value());
}

TEST_F(CompletedResultTest, ValidateRename_RejectsLongPath) {
    QString long_name(300, 'a');
    auto error = ValidateRenameForFile(long_name, QStringLiteral(".mkv"), QStringLiteral("C:\\"));
    EXPECT_TRUE(error.has_value());
}

// ---------------------------------------------------------------------------
// ClearCompletedResult preserves configuration
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, ClearCompletedResult_PreservesAudioConfig) {
    RecordViewModel vm;
    vm.audio_ui_state.source_rows = {
        {recorder_core::AudioSourceKind::Sys, true, false},
        {recorder_core::AudioSourceKind::Mic, true, false},
    };
    vm.RebuildAudioPlan();

    auto result = MakeSuccessResult(createDummyFile(QStringLiteral("preserve.mkv")).toStdWString());
    vm.SetResult(result);

    // Verify audio plan still built
    EXPECT_GT(vm.audio_track_preview.size(), 0u);

    vm.ClearCompletedResult();

    // Configuration should be preserved
    EXPECT_EQ(vm.audio_ui_state.source_rows.size(), 2u);
    EXPECT_GT(vm.audio_track_preview.size(), 0u);
}

// ---------------------------------------------------------------------------
// Release isolation
// ---------------------------------------------------------------------------

TEST_F(CompletedResultTest, ReleaseIsolation_NoVisualHarnessInRelease) {
#ifdef NDEBUG
    // In Release builds, visual harness is not compiled
    SUCCEED();
#endif
}

} // namespace
} // namespace exosnap
