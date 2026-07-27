// The session report is the on-disk support artifact for one recording. These
// tests pin the pure builder (deterministic JSON, honest "unavailable" instead of
// fabricated zeros, segment list, error phase, peak drift) and the writer's prune.

#include <gtest/gtest.h>

#include "diagnostics/SessionReport.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace exosnap::diagnostics {
namespace {

SessionReportInputs MakeInputs() {
    SessionReportInputs in;
    in.recording_session_id = QStringLiteral("rec-1234");
    in.launch_session_id = QStringLiteral("launch-abcd");
    in.capture_backend = QStringLiteral("dxgi-od");
    in.bit_depth = 10;
    in.chroma = QStringLiteral("4:2:0");
    in.color_range = QStringLiteral("limited");
    in.hdr_mode = QStringLiteral("tonemap-sdr");

    in.result.succeeded = true;
    in.result.output_path = L"C:/Users/somebody/Videos/ExoSnap_2026.mkv";
    in.result.output_file_bytes = 123456;
    in.result.elapsed_seconds = 42.5;
    in.result.output_width = 1920;
    in.result.output_height = 1080;
    in.result.frame_rate_num = 60;
    in.result.frame_rate_den = 1;
    in.result.cfr = true;
    in.result.container = recorder_core::Container::Matroska;
    in.result.video_codec = recorder_core::VideoCodec::Av1;
    in.result.audio_codec = recorder_core::AudioCodec::Opus;
    in.output_filename = QStringLiteral("ExoSnap_2026.mkv");

    in.has_snapshot = true;
    auto& s = in.snapshot;
    s.capture.frames_dropped_cfr = 3;
    s.capture.frames_dropped_processing_failure = 7;
    s.capture.frames_duplicated = 5;
    s.video_encoder.frames_submitted = 2540;
    s.video_encoder.frames_encoded = 2540;
    s.duration_skew_ms = 12.0;
    s.duration_skew_availability = recorder_core::MetricAvailability::Available;
    s.av_drift_ms = -4.0;
    s.av_drift_availability = recorder_core::MetricAvailability::Available;
    s.peak_av_drift_ms = 9.0;
    s.peak_av_drift_availability = recorder_core::MetricAvailability::Available;
    s.av_drift_raw_ms = -14.0;
    s.clock_slaving_ppm = 120.0;
    s.clock_slaving_active = true;
    s.elapsed_seconds = 42.5;
    s.capture.target_fps = 60.0;
    s.capture.frames_emitted = 2540;
    s.capture.frame_interval_ms = 16.7;
    s.capture.interval_observed = recorder_core::MetricAvailability::Unavailable;
    s.audio.track_count = 2;
    s.audio.degraded_sources = 1;
    s.audio.source_degraded = true;
    s.audio.source_degraded_occurred = true;
    s.audio.resampler_drain_recorded = {true, true, false};
    s.audio.resampler_drained_frames = {441, 0, 0};
    s.audio.resampler_undrained_frames = {0, 0, 0};
    s.encoder_init.valid = true;
    s.encoder_init.codec = recorder_core::VideoCodec::Av1;
    s.encoder_init.preset = recorder_core::NvencPreset::P5;
    s.encoder_init.rc_mode = recorder_core::RateControlMode::VariableBitrate;
    s.encoder_init.target_bitrate_kbps = 20000;
    s.encoder_init.gop_length = 120;
    return in;
}

QJsonObject Parse(const QByteArray& bytes) {
    return QJsonDocument::fromJson(bytes).object();
}

TEST(SessionReport, CarriesIdsFormatAndConfig) {
    const QJsonObject o = Parse(BuildSessionReportJson(MakeInputs()));
    EXPECT_EQ(o[QStringLiteral("recording_session_id")].toString(), QStringLiteral("rec-1234"));
    EXPECT_EQ(o[QStringLiteral("launch_session_id")].toString(), QStringLiteral("launch-abcd"));
    EXPECT_TRUE(o[QStringLiteral("succeeded")].toBool());
    EXPECT_EQ(o[QStringLiteral("output_filename")].toString(), QStringLiteral("ExoSnap_2026.mkv"));

    const QJsonObject fmt = o[QStringLiteral("output_format")].toObject();
    EXPECT_EQ(fmt[QStringLiteral("container")].toString(), QStringLiteral("MKV"));
    EXPECT_EQ(fmt[QStringLiteral("video_codec")].toString(), QStringLiteral("AV1"));
    EXPECT_EQ(fmt[QStringLiteral("audio_codec")].toString(), QStringLiteral("Opus"));

    const QJsonObject cfg = o[QStringLiteral("config")].toObject();
    EXPECT_EQ(cfg[QStringLiteral("capture_backend")].toString(), QStringLiteral("dxgi-od"));
    EXPECT_EQ(cfg[QStringLiteral("bit_depth")].toInt(), 10);
}

TEST(SessionReport, CarriesEncoderInitAndPeakDrift) {
    const QJsonObject o = Parse(BuildSessionReportJson(MakeInputs()));
    const QJsonObject enc = o[QStringLiteral("encoder_init")].toObject();
    EXPECT_EQ(enc[QStringLiteral("preset")].toString(), QStringLiteral("P5"));
    EXPECT_EQ(enc[QStringLiteral("rate_control")].toString(), QStringLiteral("VBR"));
    EXPECT_EQ(enc[QStringLiteral("gop_length")].toInt(), 120);

    const QJsonObject counters = o[QStringLiteral("counters")].toObject();
    EXPECT_DOUBLE_EQ(counters[QStringLiteral("peak_av_drift_ms")].toDouble(), 9.0);
    EXPECT_DOUBLE_EQ(counters[QStringLiteral("duration_skew_ms")].toDouble(), 12.0);
}

TEST(SessionReport, UnavailableInsteadOfFakeZero) {
    SessionReportInputs in = MakeInputs();
    in.snapshot.duration_skew_availability = recorder_core::MetricAvailability::Unavailable;
    in.snapshot.peak_av_drift_availability = recorder_core::MetricAvailability::Unavailable;
    const QJsonObject counters = Parse(BuildSessionReportJson(in))[QStringLiteral("counters")].toObject();
    EXPECT_EQ(counters[QStringLiteral("duration_skew_ms")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(counters[QStringLiteral("peak_av_drift_ms")].toString(), QStringLiteral("unavailable"));
}

TEST(SessionReport, NoSnapshotYieldsUnavailableSections) {
    SessionReportInputs in = MakeInputs();
    in.has_snapshot = false;
    const QJsonObject o = Parse(BuildSessionReportJson(in));
    EXPECT_EQ(o[QStringLiteral("counters")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(o[QStringLiteral("encoder_init")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(o[QStringLiteral("audio")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(o[QStringLiteral("video_pacing")].toString(), QStringLiteral("unavailable"));
}

TEST(SessionReport, CarriesRawDriftAndClockSlaving) {
    // A soak run has to be able to tell "slaving corrected a lot, little residual
    // remained" from "nothing was corrected": raw drift, the ppm the controller
    // ended on, and the applied compensation (raw - residual) are all reported.
    const QJsonObject counters = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("counters")].toObject();
    EXPECT_DOUBLE_EQ(counters[QStringLiteral("av_drift_raw_ms")].toDouble(), -14.0);
    EXPECT_DOUBLE_EQ(counters[QStringLiteral("clock_slaving_ppm")].toDouble(), 120.0);
    EXPECT_DOUBLE_EQ(counters[QStringLiteral("clock_slaving_compensation_ms")].toDouble(), -10.0); // -14 - (-4)
    EXPECT_TRUE(counters[QStringLiteral("clock_slaving_active")].toBool());
}

TEST(SessionReport, ClockSlavingFollowsDriftAvailability) {
    // Raw drift and ppm come from the same track av_drift_ms is taken from, so an
    // unmeasured drift must not be published as a fabricated 0 ppm.
    SessionReportInputs in = MakeInputs();
    in.snapshot.av_drift_availability = recorder_core::MetricAvailability::Unavailable;
    const QJsonObject counters = Parse(BuildSessionReportJson(in))[QStringLiteral("counters")].toObject();
    EXPECT_EQ(counters[QStringLiteral("av_drift_ms")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(counters[QStringLiteral("av_drift_raw_ms")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(counters[QStringLiteral("clock_slaving_ppm")].toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(counters[QStringLiteral("clock_slaving_compensation_ms")].toString(), QStringLiteral("unavailable"));
}

TEST(SessionReport, CarriesAudioDegradationAndResamplerDrain) {
    const QJsonObject audio = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("audio")].toObject();
    EXPECT_EQ(audio[QStringLiteral("track_count")].toInt(), 2);
    EXPECT_EQ(audio[QStringLiteral("degraded_sources_at_end")].toInt(), 1);
    EXPECT_TRUE(audio[QStringLiteral("degraded_occurred")].toBool());

    // One entry per configured track, never per array slot.
    const QJsonArray drain = audio[QStringLiteral("resampler_drain")].toArray();
    ASSERT_EQ(drain.size(), 2);
    EXPECT_EQ(drain[0].toObject()[QStringLiteral("track")].toInt(), 0);
    EXPECT_EQ(drain[0].toObject()[QStringLiteral("drained_frames")].toInt(), 441);
    EXPECT_EQ(drain[0].toObject()[QStringLiteral("undrained_frames")].toInt(), 0);
    EXPECT_EQ(drain[1].toObject()[QStringLiteral("drained_frames")].toInt(), 0);
}

TEST(SessionReport, ResamplerDrainIsUnavailableWhenTheDrainNeverRan) {
    // A failed session (or a worker that missed its join) never reaches the drain,
    // so its counters sit at their initial 0. Printing 0/0 would claim a clean
    // drain that never happened — same honest-diagnostics rule as the metrics above.
    SessionReportInputs in = MakeInputs();
    in.result.succeeded = false;
    in.snapshot.audio.resampler_drain_recorded = {false, false, false};
    const QJsonArray drain = Parse(BuildSessionReportJson(in))[QStringLiteral("audio")]
                                 .toObject()[QStringLiteral("resampler_drain")]
                                 .toArray();
    ASSERT_EQ(drain.size(), 2);
    for (const auto& entry : drain) {
        EXPECT_EQ(entry.toObject()[QStringLiteral("drained_frames")].toString(), QStringLiteral("unavailable"));
        EXPECT_EQ(entry.toObject()[QStringLiteral("undrained_frames")].toString(), QStringLiteral("unavailable"));
    }
}

TEST(SessionReport, ResamplerDrainIsPerTrackAvailable) {
    // One track drained, the other died before its drain: the healthy track keeps
    // its figure instead of being suppressed with the broken one.
    SessionReportInputs in = MakeInputs();
    in.snapshot.audio.resampler_drain_recorded = {true, false, false};
    const QJsonArray drain = Parse(BuildSessionReportJson(in))[QStringLiteral("audio")]
                                 .toObject()[QStringLiteral("resampler_drain")]
                                 .toArray();
    ASSERT_EQ(drain.size(), 2);
    EXPECT_EQ(drain[0].toObject()[QStringLiteral("drained_frames")].toInt(), 441);
    EXPECT_EQ(drain[1].toObject()[QStringLiteral("drained_frames")].toString(), QStringLiteral("unavailable"));
}

TEST(SessionReport, ResamplerDrainReportsUndrainedTail) {
    SessionReportInputs in = MakeInputs();
    in.snapshot.audio.resampler_undrained_frames = {17, 0, 0};
    const QJsonArray drain = Parse(BuildSessionReportJson(in))[QStringLiteral("audio")]
                                 .toObject()[QStringLiteral("resampler_drain")]
                                 .toArray();
    ASSERT_GE(drain.size(), 1);
    EXPECT_EQ(drain[0].toObject()[QStringLiteral("undrained_frames")].toInt(), 17);
}

TEST(SessionReport, AudioSectionSurvivesAnUnconfiguredTrackCount) {
    // A failure before the audio plan reached the engine leaves track_count 0 —
    // the section must then be empty, not a fabricated three-track list.
    SessionReportInputs in = MakeInputs();
    in.snapshot.audio.track_count = 0;
    const QJsonObject audio = Parse(BuildSessionReportJson(in))[QStringLiteral("audio")].toObject();
    EXPECT_EQ(audio[QStringLiteral("track_count")].toInt(), 0);
    EXPECT_TRUE(audio[QStringLiteral("resampler_drain")].toArray().isEmpty());
}

TEST(SessionReport, CarriesVideoPacing) {
    const QJsonObject pacing = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("video_pacing")].toObject();
    EXPECT_TRUE(pacing[QStringLiteral("cfr")].toBool());
    EXPECT_DOUBLE_EQ(pacing[QStringLiteral("target_fps")].toDouble(), 60.0);
    // Whole-session average (2540 emitted / 42.5 s), not the terminal snapshot's
    // instantaneous rate — that one would measure the finalize gap.
    EXPECT_NEAR(pacing[QStringLiteral("average_emitted_fps")].toDouble(), 2540.0 / 42.5, 1e-9);
    // CFR capture observes no interval; it must say so instead of echoing the target.
    EXPECT_EQ(pacing[QStringLiteral("frame_interval_ms")].toString(), QStringLiteral("unavailable"));

    // The pacing outcome keeps its existing home (no renames, no duplication).
    const QJsonObject counters = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("counters")].toObject();
    EXPECT_EQ(counters[QStringLiteral("frames_duplicated")].toInt(), 5);
    EXPECT_EQ(counters[QStringLiteral("frames_dropped")].toObject()[QStringLiteral("cfr")].toInt(), 3);
    // Frames lost to a failed conversion are their own bucket, so a report can tell
    // benign pacing apart from picture the session actually lost.
    EXPECT_EQ(counters[QStringLiteral("frames_dropped")].toObject()[QStringLiteral("processing_failure")].toInt(), 7);
}

TEST(SessionReport, KeyframePredictionMismatchesAreReported) {
    // Warn-only during the recording, so the end-of-session total is the only
    // place a soak run can see the enforced keyframe cadence diverged at all.
    SessionReportInputs in = MakeInputs();
    in.snapshot.video_encoder.keyframe_prediction_mismatches = 7;
    const QJsonObject counters = Parse(BuildSessionReportJson(in))[QStringLiteral("counters")].toObject();
    EXPECT_EQ(counters[QStringLiteral("encoder_keyframe_prediction_mismatches")].toInt(), 7);
}

TEST(SessionReport, CleanSessionReportsZeroKeyframePredictionMismatches) {
    // A clean session must print an explicit 0, not omit the key: a missing key
    // is indistinguishable from an older report that never carried the counter.
    const QJsonObject counters = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("counters")].toObject();
    ASSERT_TRUE(counters.contains(QStringLiteral("encoder_keyframe_prediction_mismatches")));
    EXPECT_EQ(counters[QStringLiteral("encoder_keyframe_prediction_mismatches")].toInt(), 0);
}

TEST(SessionReport, OutputTsMismatchesAreNotReportedUnderAnyName) {
    // The outputTimeStamp check aborts the encode before the packet is filled
    // in, so its counter is structurally always 0. Reporting it would advertise
    // a check that had a chance to fire. Scan every counter key rather than
    // guessing one spelling, so the omission holds however it gets named.
    const QJsonObject counters = Parse(BuildSessionReportJson(MakeInputs()))[QStringLiteral("counters")].toObject();
    for (auto it = counters.begin(); it != counters.end(); ++it) {
        EXPECT_FALSE(it.key().contains(QStringLiteral("output_ts"), Qt::CaseInsensitive))
            << "counters key '" << it.key().toStdString()
            << "' reports the outputTimeStamp mismatch counter, which can never be non-zero "
               "(the mismatch aborts the encode). See ADR 0053.";
    }
}

TEST(SessionReport, PacingAverageIsUnavailableWithoutElapsedTime) {
    // An immediate failure has no elapsed time to divide by — no fabricated 0 fps.
    SessionReportInputs in = MakeInputs();
    in.snapshot.elapsed_seconds = 0.0;
    const QJsonObject pacing = Parse(BuildSessionReportJson(in))[QStringLiteral("video_pacing")].toObject();
    EXPECT_EQ(pacing[QStringLiteral("average_emitted_fps")].toString(), QStringLiteral("unavailable"));
}

TEST(SessionReport, ObservedFrameIntervalIsReportedOnVfr) {
    SessionReportInputs in = MakeInputs();
    in.snapshot.capture.interval_observed = recorder_core::MetricAvailability::Available;
    const QJsonObject pacing = Parse(BuildSessionReportJson(in))[QStringLiteral("video_pacing")].toObject();
    EXPECT_DOUBLE_EQ(pacing[QStringLiteral("frame_interval_ms")].toDouble(), 16.7);
}

TEST(SessionReport, SegmentsAndErrorPhase) {
    SessionReportInputs in = MakeInputs();
    in.result.succeeded = false;
    in.result.error_phase = L"VideoEncode";
    in.result.hresult_text = L"0x80004005";
    in.result.error_detail = L"NVENC configure failed";
    CompletedRecordingSegment seg;
    seg.index = 0;
    seg.duration_seconds = 30.0;
    seg.file_size_bytes = 5000;
    seg.succeeded = true;
    in.result.segments.push_back(seg);

    const QJsonObject o = Parse(BuildSessionReportJson(in));
    const QJsonArray segments = o[QStringLiteral("segments")].toArray();
    ASSERT_EQ(segments.size(), 1);
    EXPECT_DOUBLE_EQ(segments[0].toObject()[QStringLiteral("duration_seconds")].toDouble(), 30.0);
    EXPECT_TRUE(segments[0].toObject()[QStringLiteral("finalized")].toBool());

    const QJsonObject err = o[QStringLiteral("error")].toObject();
    EXPECT_EQ(err[QStringLiteral("phase")].toString(), QStringLiteral("VideoEncode"));
    EXPECT_EQ(err[QStringLiteral("detail")].toString(), QStringLiteral("NVENC configure failed"));
}

TEST(SessionReport, DeterministicForFixedInputs) {
    const auto a = BuildSessionReportJson(MakeInputs());
    const auto b = BuildSessionReportJson(MakeInputs());
    EXPECT_EQ(a, b);
}

TEST(SessionReport, WriterUsesTheSessionIdAndPrunesToKeepN) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString reports_dir = tmp.path() + QStringLiteral("/reports");

    // The writer names the file after the provided id (no store involved), so the
    // id set unconditionally by the coordinator is what identifies the report.
    SessionReportInputs in = MakeInputs();
    in.recording_session_id = QStringLiteral("only-id");
    QString written;
    ASSERT_TRUE(WriteSessionReport(reports_dir, in, /*keep_n=*/10, &written));
    EXPECT_TRUE(written.endsWith(QStringLiteral("session-only-id.json")));

    // Write 12 distinct reports; prune must leave exactly keep_n = 10.
    for (int i = 0; i < 12; ++i) {
        SessionReportInputs each = MakeInputs();
        each.recording_session_id = QStringLiteral("rec-%1").arg(i);
        ASSERT_TRUE(WriteSessionReport(reports_dir, each, /*keep_n=*/10));
    }
    const int remaining = QDir(reports_dir).entryList({QStringLiteral("session-*.json")}, QDir::Files).size();
    EXPECT_EQ(remaining, 10);
}

} // namespace
} // namespace exosnap::diagnostics
