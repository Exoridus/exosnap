// Contract tests for the observability serialization boundary.
//
// Everything here runs from fixtures: no recording, no GPU, no window. That is
// the point -- the properties under test are about HONESTY, and the states in
// which a payload is most likely to lie (nothing measured yet, a metric that
// cannot exist on this backend, a probe that has not run) are exactly the states
// a live run does not spend much time in.
//
// The recurring assertion is the same one in every group: an unmeasured value is
// `null` with an availability word beside it, never 0/false/"".

#include "observability/DiagnosticsResultsJson.h"
#include "observability/EnvironmentSnapshot.h"
#include "observability/EventQuery.h"
#include "observability/PipelineSnapshotJson.h"
#include "observability/SettingsSnapshot.h"
#include "observability/WindowIdentity.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/EngineLogBridge.h"
#include "diagnostics/StructuredLog.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <filesystem>
#include <string>

using namespace exosnap;
using namespace exosnap::observability;

namespace {

QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "observability_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

QJsonValue At(const QJsonObject& root, std::initializer_list<const char*> path) {
    QJsonValue value = root;
    for (const char* key : path) {
        if (!value.isObject())
            return QJsonValue(QJsonValue::Undefined);
        value = value.toObject().value(QString::fromLatin1(key));
    }
    return value;
}

// A snapshot in the shape a healthy 60 fps recording produces. Individual cases
// take this and break exactly the one thing they are about, so a failure names
// the property rather than the fixture.
recorder_core::RecordingDiagnosticsSnapshot HealthyRecording() {
    recorder_core::RecordingDiagnosticsSnapshot s;
    s.valid = true;
    s.session_generation = 7;
    s.lifecycle = recorder_core::DiagnosticsLifecycle::Recording;
    s.elapsed_seconds = 12.5;
    s.health = recorder_core::PipelineHealth::Good;
    s.bottleneck = recorder_core::PipelineBottleneck::None;

    s.capture.target_fps = 60.0;
    s.capture.actual_fps = 59.98;
    s.capture.frames_captured = 750;
    s.capture.frames_emitted = 749;
    s.capture.frames_dropped_coalesced = 40; // benign
    s.capture.frames_dropped_cfr = 2;        // benign
    s.capture.source_type = recorder_core::CaptureSourceType::Display;

    s.capture.present_cadence_availability = recorder_core::MetricAvailability::Available;
    s.capture.source_present_interval_ms = 8.33;
    s.capture.source_present_jitter_ms = 1.2;
    s.capture.source_coalesce_ratio = 1.4;

    s.capture.acquire_availability = recorder_core::MetricAvailability::Available;
    s.capture.acquire_latest_ms = 0.8;
    s.capture.acquire_average_ms = 0.7;
    s.capture.acquire_peak_ms = 2.1;

    s.compositor.active = true;
    s.compositor.latest_ms = 0.5;
    s.compositor.frames_composed = 749;

    s.video_encoder.frames_encoded = 748;
    s.video_encoder.frames_submitted = 748;
    s.video_encoder.latest_ms = 3.0;
    s.video_encoder.p99_ms = 3.1;
    s.video_encoder.output_fps = 59.9;
    s.video_encoder.codec = recorder_core::VideoCodec::Av1;
    s.video_encoder.width = 2560;
    s.video_encoder.height = 1440;

    s.video_timing.availability = recorder_core::MetricAvailability::Available;
    s.video_timing.tick_p50_ms = 4.0;
    s.video_timing.tick_p99_ms = 6.0;
    s.video_timing.budget_ms = 16.67;

    s.audio.active = true;
    s.audio.sample_rate = 48000;
    s.audio.channels = 2;
    s.audio.track_count = 1;
    s.audio.codec = recorder_core::AudioCodec::Opus;

    s.av_drift_availability = recorder_core::MetricAvailability::Available;
    s.av_drift_ms = 0.4;
    s.av_drift_raw_ms = 1.9;
    s.peak_av_drift_availability = recorder_core::MetricAvailability::Available;
    s.peak_av_drift_ms = 1.7;

    s.mux.packets_processed = 800;
    s.mux.bytes_written = 1024 * 1024;
    s.disk.bytes_written = 1024 * 1024;
    s.disk.output_target = "D:\\";
    s.disk_fill_eta_seconds = 21600.0;
    return s;
}

// ---------------------------------------------------------------------------
// pipeline.snapshot
// ---------------------------------------------------------------------------

TEST(PipelineSnapshotJson, IdleAnswersWithoutFabricatingAHealthyPipeline) {
    const QJsonObject json = PipelineSnapshotToJson(recorder_core::RecordingDiagnosticsSnapshot{});

    EXPECT_FALSE(json.value(QStringLiteral("valid")).toBool());
    EXPECT_EQ(json.value(QStringLiteral("lifecycle")).toString(), QStringLiteral("idle"));
    EXPECT_EQ(json.value(QStringLiteral("health")).toString(), QStringLiteral("Idle"));
    EXPECT_TRUE(json.value(QStringLiteral("elapsedSeconds")).isNull());
    // The whole point: an idle process must not answer with a complete, all-zero
    // pipeline, which reads as a recording that dropped nothing.
    EXPECT_FALSE(json.contains(QStringLiteral("capture")));
    EXPECT_FALSE(json.contains(QStringLiteral("encoder")));
    EXPECT_FALSE(json.contains(QStringLiteral("disk")));
}

TEST(PipelineSnapshotJson, ProblemDropsUseTheEngineDefinitionAndNotTheTotal) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.capture.frames_dropped_backpressure = 3;
    s.capture.frames_dropped_processing_failure = 1;

    const QJsonObject json = PipelineSnapshotToJson(s);
    // 3 + 1, NOT 3 + 1 + 40 + 2. The benign categories are reported separately
    // and must never be folded into the number the product calls a drop.
    EXPECT_EQ(At(json, {"capture", "problemDrops"}).toDouble(), 4.0);
    EXPECT_EQ(At(json, {"capture", "totalDrops"}).toDouble(), 46.0);
    EXPECT_EQ(At(json, {"capture", "coalescedDrops"}).toDouble(), 40.0);
    EXPECT_EQ(At(json, {"capture", "cfrDrops"}).toDouble(), 2.0);
    EXPECT_EQ(At(json, {"capture", "problemDrops"}).toDouble(),
              static_cast<double>(s.capture.frames_dropped_problem()));
}

TEST(PipelineSnapshotJson, WindowCaptureReportsPresentCadenceAsUnsupportedNotMerelyMissing) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.capture.source_type = recorder_core::CaptureSourceType::Window;
    s.capture.present_cadence_availability = recorder_core::MetricAvailability::Unavailable;

    const QJsonObject json = PipelineSnapshotToJson(s);
    // WGC exposes no present timestamp at all, so this never becomes available by
    // waiting. "unsupported" and "unavailable" send a reader to two different
    // places and the payload has to tell them apart.
    EXPECT_EQ(At(json, {"sourcePresentation", "cadenceAvailability"}).toString(), QStringLiteral("unsupported"));
    EXPECT_TRUE(At(json, {"sourcePresentation", "presentIntervalMs"}).isNull());
    EXPECT_TRUE(At(json, {"sourcePresentation", "presentJitterMs"}).isNull());
}

TEST(PipelineSnapshotJson, DisplayCaptureBeforeWarmUpReportsCadenceAsUnavailable) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.capture.present_cadence_availability = recorder_core::MetricAvailability::Unavailable;

    const QJsonObject json = PipelineSnapshotToJson(s);
    EXPECT_EQ(At(json, {"sourcePresentation", "cadenceAvailability"}).toString(), QStringLiteral("unavailable"));
}

TEST(PipelineSnapshotJson, PresentModeIsNullUntilPresentMonObservedOne) {
    const QJsonObject json = PipelineSnapshotToJson(HealthyRecording());
    // Never a fabricated "composed": an unobserved present mode is an absence.
    EXPECT_TRUE(At(json, {"sourcePresentation", "presentMode"}).isNull());
    EXPECT_TRUE(At(json, {"sourcePresentation", "tearing"}).isNull());
    EXPECT_EQ(At(json, {"sourcePresentation", "modeAvailability"}).toString(), QStringLiteral("unavailable"));
}

TEST(PipelineSnapshotJson, TimingsAreNamedAsCpuSubmissionAndNeverAsGpuTime) {
    const QJsonObject json = PipelineSnapshotToJson(HealthyRecording());

    EXPECT_EQ(At(json, {"compositor", "timingSemantics"}).toString(), QStringLiteral("cpuSubmission"));
    EXPECT_EQ(At(json, {"encoder", "timingSemantics"}).toString(), QStringLiteral("submitToBitstreamReady"));
    EXPECT_EQ(At(json, {"captureTiming", "semantics"}).toString(), QStringLiteral("cpuAcquireAndCopy"));
    EXPECT_EQ(At(json, {"disk", "latencySemantics"}).toString(), QStringLiteral("bufferedWriteCall"));
    // This pipeline takes no GPU timestamp. If a key ever appears that claims
    // one, it has to be backed by a real measurement first.
    EXPECT_FALSE(json.value(QStringLiteral("compositor")).toObject().contains(QStringLiteral("gpuTimeMs")));
}

TEST(PipelineSnapshotJson, EncoderInitIsOmittedUntilAnEncoderWasConfigured) {
    const QJsonObject before = PipelineSnapshotToJson(HealthyRecording());
    EXPECT_FALSE(At(before, {"encoderInit", "valid"}).toBool());
    // A default-constructed EncoderInitInfo is full of plausible defaults (AV1,
    // P4, CQ 0). Publishing them would answer "what is running" with a guess.
    EXPECT_FALSE(before.value(QStringLiteral("encoderInit")).toObject().contains(QStringLiteral("preset")));

    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.encoder_init.valid = true;
    s.encoder_init.codec = recorder_core::VideoCodec::Av1;
    s.encoder_init.preset = recorder_core::NvencPreset::P6;
    s.encoder_init.rc_mode = recorder_core::RateControlMode::ConstantQuality;
    s.encoder_init.cq = 17;
    s.encoder_init.gop_length = 120;
    s.encoder_init.bit_depth = recorder_core::BitDepth::Bit10;

    const QJsonObject after = PipelineSnapshotToJson(s);
    EXPECT_TRUE(At(after, {"encoderInit", "valid"}).toBool());
    EXPECT_EQ(At(after, {"encoderInit", "codec"}).toString(), QStringLiteral("AV1"));
    EXPECT_EQ(At(after, {"encoderInit", "preset"}).toString(), QStringLiteral("P6"));
    EXPECT_EQ(At(after, {"encoderInit", "rateControl"}).toString(), QStringLiteral("constantQuality"));
    EXPECT_EQ(At(after, {"encoderInit", "cq"}).toDouble(), 17.0);
    EXPECT_EQ(At(after, {"encoderInit", "bitDepth"}).toDouble(), 10.0);
}

TEST(PipelineSnapshotJson, RawResidualAndSkewStayThreeSeparateFacts) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.clock_slaving_active = true;
    s.clock_slaving_ppm = 42.0;
    s.duration_skew_availability = recorder_core::MetricAvailability::Unavailable;

    const QJsonObject json = PipelineSnapshotToJson(s);
    EXPECT_EQ(At(json, {"avTiming", "avDriftMs"}).toDouble(), 0.4);
    EXPECT_EQ(At(json, {"avTiming", "rawAvDriftMs"}).toDouble(), 1.9);
    EXPECT_EQ(At(json, {"avTiming", "peakAvDriftMs"}).toDouble(), 1.7);
    EXPECT_TRUE(At(json, {"avTiming", "clockSlavingActive"}).toBool());
    EXPECT_EQ(At(json, {"avTiming", "clockSlavingPpm"}).toDouble(), 42.0);
    // Duration skew is a different measurement with its own availability, and an
    // unavailable one must not borrow the drift's zero.
    EXPECT_TRUE(At(json, {"avTiming", "durationSkewMs"}).isNull());
    EXPECT_EQ(At(json, {"avTiming", "durationSkewAvailability"}).toString(), QStringLiteral("unavailable"));
}

TEST(PipelineSnapshotJson, NegativeDiskEtaIsUnavailableRatherThanZeroSecondsLeft) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.disk_fill_eta_seconds = -1.0;

    const QJsonObject json = PipelineSnapshotToJson(s);
    EXPECT_TRUE(At(json, {"disk", "fillEtaSeconds"}).isNull());
    // Same value on the healthy fixture is a real measurement and stays one.
    EXPECT_EQ(At(PipelineSnapshotToJson(HealthyRecording()), {"disk", "fillEtaSeconds"}).toDouble(), 21600.0);
}

TEST(PipelineSnapshotJson, ResamplerDrainReportsOnlyTracksThatReachedTheirDrain) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.audio.resampler_drain_recorded[0] = true;
    s.audio.resampler_drained_frames[0] = 512;
    s.audio.resampler_undrained_frames[0] = 0;
    // Track 1 never drained. Its zeros are not a measurement and must not appear.
    s.audio.resampler_drain_recorded[1] = false;

    const QJsonArray tracks = At(PipelineSnapshotToJson(s), {"audio", "resamplerDrain"}).toArray();
    ASSERT_EQ(tracks.size(), 1);
    EXPECT_EQ(tracks.at(0).toObject().value(QStringLiteral("track")).toInt(), 0);
    EXPECT_EQ(tracks.at(0).toObject().value(QStringLiteral("drainedFrames")).toDouble(), 512.0);
}

TEST(PipelineSnapshotJson, BottleneckAndHealthUseTheEnginesOwnVocabulary) {
    recorder_core::RecordingDiagnosticsSnapshot s = HealthyRecording();
    s.health = recorder_core::PipelineHealth::Warning;
    s.bottleneck = recorder_core::PipelineBottleneck::VideoEncoder;
    s.bottleneck_reason = "Encoder latency is approaching the frame budget.";

    const QJsonObject json = PipelineSnapshotToJson(s);
    EXPECT_EQ(json.value(QStringLiteral("health")).toString(), QStringLiteral("Warning"));
    EXPECT_EQ(json.value(QStringLiteral("bottleneck")).toString(), QStringLiteral("VideoEncoder"));
    EXPECT_EQ(json.value(QStringLiteral("bottleneckReason")).toString(),
              QStringLiteral("Encoder latency is approaching the frame budget."));
    // The strings come from recorder_core::ToString, so a renamed enumerator
    // changes both sides at once instead of silently changing the wire.
    EXPECT_EQ(json.value(QStringLiteral("bottleneck")).toString(),
              QString::fromLatin1(recorder_core::ToString(s.bottleneck)));
}

// ---------------------------------------------------------------------------
// diagnostics.results
// ---------------------------------------------------------------------------

diagnostics::DiagnosticResult MakeBlockerWithFix() {
    diagnostics::DiagnosticResult result;
    result.id = "hdr.h264.blocker";
    result.group = diagnostics::DiagnosticGroup::GpuEncoder;
    result.severity = diagnostics::DiagnosticSeverity::Blocker;
    result.tier = diagnostics::DiagnosticTier::Blocker;
    result.title = "H.264 cannot carry HDR10";
    result.summary = "Pick HEVC or AV1, or turn HDR off.";
    result.current_value = "H.264 + HDR10";
    result.recommendation = "Switch the video codec.";
    result.affected_features = {"recording"};
    result.timestamp = 1234;
    result.fix_action = diagnostics::FixAction{"fix.codec.hevc", "Switch to HEVC", diagnostics::FixAction::Safety::Auto,
                                               true, "Video codec -> HEVC"};
    return result;
}

TEST(DiagnosticsResultsJson, TierSeverityAndFixActionSurviveSerialization) {
    diagnostics::DiagnosticChecklist checklist;
    checklist.results.push_back(MakeBlockerWithFix());
    checklist.has_blocker = true;

    const QJsonObject json =
        DiagnosticsResultsToJson(checklist, {}, /*checked=*/true, /*checking=*/false, /*elevated=*/false);
    const QJsonObject result = json.value(QStringLiteral("results")).toArray().at(0).toObject();

    EXPECT_EQ(result.value(QStringLiteral("id")).toString(), QStringLiteral("hdr.h264.blocker"));
    EXPECT_EQ(result.value(QStringLiteral("severity")).toString(), QStringLiteral("blocker"));
    // The tier is declared at the diagnosis site precisely so nothing downstream
    // re-derives visibility from an id allowlist. Losing it here would hand that
    // job straight back to every consumer.
    EXPECT_EQ(result.value(QStringLiteral("tier")).toString(), QStringLiteral("blocker"));
    EXPECT_EQ(result.value(QStringLiteral("group")).toString(), QStringLiteral("gpuEncoder"));
    EXPECT_TRUE(result.value(QStringLiteral("alwaysVisible")).toBool());
    EXPECT_FALSE(result.value(QStringLiteral("bundlesIntoTip")).toBool());

    const QJsonObject fix = result.value(QStringLiteral("fixAction")).toObject();
    EXPECT_EQ(fix.value(QStringLiteral("id")).toString(), QStringLiteral("fix.codec.hevc"));
    EXPECT_EQ(fix.value(QStringLiteral("safety")).toString(), QStringLiteral("auto"));
    EXPECT_TRUE(fix.value(QStringLiteral("reversible")).toBool());
    EXPECT_EQ(fix.value(QStringLiteral("changesSummary")).toString(), QStringLiteral("Video codec -> HEVC"));
}

TEST(DiagnosticsResultsJson, AResultWithoutAFixSaysSoRatherThanCarryingAnEmptyOne) {
    diagnostics::DiagnosticChecklist checklist;
    diagnostics::DiagnosticResult fact;
    fact.id = "gpu.model";
    fact.tier = diagnostics::DiagnosticTier::Fact;
    fact.title = "GPU";
    checklist.results.push_back(fact);

    const QJsonObject json = DiagnosticsResultsToJson(checklist, {}, true, false, false);
    const QJsonObject result = json.value(QStringLiteral("results")).toArray().at(0).toObject();
    EXPECT_TRUE(result.value(QStringLiteral("fixAction")).isNull());
    // An empty detail is absent, not "". Distinguishing them is what lets a
    // reader tell "no evidence line" from "an empty evidence line".
    EXPECT_TRUE(result.value(QStringLiteral("detail")).isNull());
    EXPECT_FALSE(result.value(QStringLiteral("alwaysVisible")).toBool());
}

TEST(DiagnosticsResultsJson, AnUncheckedProcessIsNotAHealthyOne) {
    const QJsonObject json = DiagnosticsResultsToJson({}, {}, /*checked=*/false, /*checking=*/true, false);
    EXPECT_FALSE(json.value(QStringLiteral("checked")).toBool());
    EXPECT_TRUE(json.value(QStringLiteral("checking")).toBool());
    EXPECT_TRUE(json.value(QStringLiteral("results")).toArray().isEmpty());
    // An empty result list means the same thing on a perfect machine and on one
    // nobody has looked at. `checked` is the only field that separates them.
    EXPECT_EQ(json.value(QStringLiteral("worstSeverity")).toString(), QStringLiteral("pass"));
}

TEST(DiagnosticsResultsJson, OptimisationBundlesIntoTheTipChipAndIsNotAlwaysVisible) {
    diagnostics::DiagnosticChecklist checklist;
    diagnostics::DiagnosticResult tip;
    tip.id = "encoder.headroom";
    tip.tier = diagnostics::DiagnosticTier::Optimisation;
    tip.severity = diagnostics::DiagnosticSeverity::Notice;
    checklist.results.push_back(tip);

    const QJsonObject result = DiagnosticsResultsToJson(checklist, {}, true, false, false)
                                   .value(QStringLiteral("results"))
                                   .toArray()
                                   .at(0)
                                   .toObject();
    EXPECT_EQ(result.value(QStringLiteral("tier")).toString(), QStringLiteral("optimisation"));
    EXPECT_TRUE(result.value(QStringLiteral("bundlesIntoTip")).toBool());
    EXPECT_FALSE(result.value(QStringLiteral("alwaysVisible")).toBool());
}

// ---------------------------------------------------------------------------
// settings.snapshot
// ---------------------------------------------------------------------------

TEST(SettingsSnapshotJson, RequestedAndEffectiveDifferencesAreReportedFieldByField) {
    SettingsSnapshotInputs inputs;
    inputs.requested = MakeDefaultPreset().config;
    inputs.requested.output.container = capability::Container::Mp4;
    inputs.requested.output.video_codec = capability::VideoCodec::Av1;
    inputs.requested.output.audio_codec = capability::AudioCodec::Opus;

    // What MP4 reconciliation actually produces (ADR 0010).
    inputs.effective = inputs.requested;
    inputs.effective.output.video_codec = capability::VideoCodec::H264;
    inputs.effective.output.audio_codec = capability::AudioCodec::Aac;

    inputs.capabilities_probed = true;
    inputs.resolution.succeeded = true;
    inputs.resolution.adjustments.push_back({"videoCodec", "AV1", "H.264", "MP4 does not carry AV1"});

    const QJsonObject json = SettingsSnapshotToJson(inputs);
    const QJsonArray differences = json.value(QStringLiteral("differences")).toArray();

    ASSERT_EQ(differences.size(), 2) << "one entry per field that actually differs";
    bool saw_video = false;
    for (const QJsonValue& entry : differences) {
        const QJsonObject difference = entry.toObject();
        if (difference.value(QStringLiteral("field")).toString() != QLatin1String("video.videoCodec"))
            continue;
        saw_video = true;
        EXPECT_EQ(difference.value(QStringLiteral("requested")).toString(), QStringLiteral("AV1"));
        EXPECT_EQ(difference.value(QStringLiteral("effective")).toString(), QStringLiteral("H.264"));
        // The resolver's own explanation, not a guess assembled here.
        EXPECT_EQ(difference.value(QStringLiteral("reason")).toString(), QStringLiteral("MP4 does not carry AV1"));
    }
    EXPECT_TRUE(saw_video);
}

TEST(SettingsSnapshotJson, AnUnprobedMachineDoesNotClaimTheConfigurationWasValidated) {
    SettingsSnapshotInputs inputs;
    inputs.requested = MakeDefaultPreset().config;
    inputs.effective = inputs.requested;
    inputs.capabilities_probed = false;

    const QJsonObject constraints = SettingsSnapshotToJson(inputs).value(QStringLiteral("constraints")).toObject();
    EXPECT_FALSE(constraints.value(QStringLiteral("evaluated")).toBool());
    // An empty adjustment list on an unprobed machine would read as "the hardware
    // was checked and nothing needed changing".
    EXPECT_TRUE(constraints.value(QStringLiteral("valid")).isNull());
}

TEST(SettingsSnapshotJson, RunningLevelComesFromTheEncoderAndNotFromASecondCopyOfTheConfig) {
    SettingsSnapshotInputs inputs;
    inputs.requested = MakeDefaultPreset().config;
    inputs.requested.output.nvenc_preset = recorder_core::NvencPreset::P7;
    inputs.effective = inputs.requested;
    inputs.effective.output.nvenc_preset = recorder_core::NvencPreset::P6;

    // No encoder configured yet: `running` is invalid, not a mirror of effective.
    const QJsonObject idle = SettingsSnapshotToJson(inputs).value(QStringLiteral("running")).toObject();
    EXPECT_FALSE(idle.value(QStringLiteral("valid")).toBool());
    EXPECT_FALSE(idle.contains(QStringLiteral("encoderPreset")));

    inputs.running.valid = true;
    inputs.running.preset = recorder_core::NvencPreset::P6;
    inputs.running.cq = 17;
    inputs.running_live = true;

    const QJsonObject json = SettingsSnapshotToJson(inputs);
    EXPECT_EQ(At(json, {"requested", "video", "encoderPreset"}).toString(), QStringLiteral("P7"));
    EXPECT_EQ(At(json, {"effective", "video", "encoderPreset"}).toString(), QStringLiteral("P6"));
    EXPECT_EQ(At(json, {"running", "encoderPreset"}).toString(), QStringLiteral("P6"));
    EXPECT_TRUE(At(json, {"running", "live"}).toBool());
}

TEST(SettingsSnapshotJson, TheOutputFolderIsReportedAsItsRootAndNeverInFull) {
    SettingsSnapshotInputs inputs;
    inputs.requested = MakeDefaultPreset().config;
    inputs.requested.output.output_folder = std::filesystem::path(L"D:\\Users\\someone\\Videos\\ExoSnap");
    inputs.effective = inputs.requested;

    const QJsonObject json = SettingsSnapshotToJson(inputs);
    const QString root = At(json, {"requested", "output", "folderRoot"}).toString();
    EXPECT_FALSE(root.contains(QStringLiteral("someone")));
    EXPECT_TRUE(root.startsWith(QStringLiteral("D:")));
    EXPECT_TRUE(At(json, {"requested", "output", "folderConfigured"}).toBool());
    // The whole payload, not just that one field: no part of it may carry the
    // user's directory.
    EXPECT_FALSE(QString::fromUtf8(QJsonDocument(json).toJson()).contains(QStringLiteral("someone")));
}

TEST(SettingsSnapshotJson, AudioRowOrderAndMergeFlagsSurvive) {
    SettingsSnapshotInputs inputs;
    inputs.requested = MakeDefaultPreset().config;
    inputs.requested.audio.source_rows = {
        {recorder_core::AudioSourceKind::App, false, false, 0.0f, false},
        {recorder_core::AudioSourceKind::Sys, true, false, -3.0f, false},
        {recorder_core::AudioSourceKind::Mic, true, true, 0.0f, true},
    };
    inputs.effective = inputs.requested;

    const QJsonArray rows = At(SettingsSnapshotToJson(inputs), {"requested", "audio", "rows"}).toArray();
    ASSERT_EQ(rows.size(), 3);
    // Order IS the model (APP, SYS, MIC) and merge_with_above only means anything
    // relative to the row above, so a set would lose the setting entirely.
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("app"));
    EXPECT_EQ(rows.at(2).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("mic"));
    EXPECT_TRUE(rows.at(2).toObject().value(QStringLiteral("mergeWithAbove")).toBool());
    EXPECT_TRUE(rows.at(2).toObject().value(QStringLiteral("muted")).toBool());
    EXPECT_EQ(rows.at(1).toObject().value(QStringLiteral("gainDb")).toDouble(), -3.0);
}

// ---------------------------------------------------------------------------
// environment.snapshot
// ---------------------------------------------------------------------------

TEST(EnvironmentSnapshotJson, AnUnscannedAdapterListIsNotAnAbsentGpu) {
    EnvironmentSnapshotInputs inputs;
    inputs.adapters_scanned = false;

    const QJsonObject gpu = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("gpu")).toObject();
    EXPECT_FALSE(gpu.value(QStringLiteral("scanned")).toBool());
    EXPECT_EQ(gpu.value(QStringLiteral("adapterAvailability")).toString(), QStringLiteral("unavailable"));
    EXPECT_TRUE(gpu.value(QStringLiteral("adapters")).toArray().isEmpty());
}

TEST(EnvironmentSnapshotJson, AnUnprobedAdapterReportsNullCodecFlagsRatherThanFalse) {
    EnvironmentSnapshotInputs inputs;
    inputs.adapters_scanned = true;
    capability::AdapterInfo adapter;
    adapter.name = "NVIDIA GeForce RTX 5070 Ti";
    adapter.vendor = capability::AdapterVendor::Nvidia;
    adapter.kind = capability::AdapterKind::Discrete;
    inputs.adapters.push_back(adapter);
    inputs.adapter_capabilities.push_back({}); // probed == false
    inputs.active_adapter_index = 0;

    const QJsonObject entry = EnvironmentSnapshotToJson(inputs)
                                  .value(QStringLiteral("gpu"))
                                  .toObject()
                                  .value(QStringLiteral("adapters"))
                                  .toArray()
                                  .at(0)
                                  .toObject();
    EXPECT_EQ(entry.value(QStringLiteral("vendor")).toString(), QStringLiteral("nvidia"));
    EXPECT_TRUE(entry.value(QStringLiteral("activeEncoder")).toBool());

    const QJsonObject encoder = entry.value(QStringLiteral("encoder")).toObject();
    EXPECT_FALSE(encoder.value(QStringLiteral("probed")).toBool());
    // "This GPU cannot encode AV1" is a strong claim. Nothing asked it.
    EXPECT_TRUE(encoder.value(QStringLiteral("av1")).isNull());
    EXPECT_TRUE(encoder.value(QStringLiteral("hevc")).isNull());
}

TEST(EnvironmentSnapshotJson, HdrOffAndHdrUnknownAreDifferentPayloads) {
    EnvironmentSnapshotInputs inputs;
    ScreenFacts screen;
    screen.name = QStringLiteral("27GL850");
    screen.width = 2560;
    screen.height = 1440;
    screen.refresh_hz = 144.0;
    inputs.screens.push_back(screen);

    // No DXGI probe: HDR is unknown, and the payload must not answer "false".
    const QJsonObject unknown = EnvironmentSnapshotToJson(inputs)
                                    .value(QStringLiteral("displays"))
                                    .toObject()
                                    .value(QStringLiteral("screens"))
                                    .toArray()
                                    .at(0)
                                    .toObject();
    EXPECT_TRUE(unknown.value(QStringLiteral("hdrActive")).isNull());
    EXPECT_TRUE(unknown.value(QStringLiteral("automaticColorManagement")).isNull());
    EXPECT_EQ(unknown.value(QStringLiteral("colorAvailability")).toString(), QStringLiteral("unavailable"));

    capability::DisplayHdrFacts facts;
    facts.name = "\\\\.\\DISPLAY1";
    // The join key: DXGI names the monitor "\\.\DISPLAY1" and Qt names the same
    // monitor "27GL850", so only the DisplayConfig friendly name connects them.
    facts.friendly_name = "27GL850";
    facts.hdr_active = false;
    facts.wide_color_enforced = true;
    inputs.capabilities.probed = true;
    inputs.capabilities.runtime.displays.push_back(facts);

    const QJsonObject measured = EnvironmentSnapshotToJson(inputs)
                                     .value(QStringLiteral("displays"))
                                     .toObject()
                                     .value(QStringLiteral("screens"))
                                     .toArray()
                                     .at(0)
                                     .toObject();
    EXPECT_FALSE(measured.value(QStringLiteral("hdrActive")).isNull());
    EXPECT_FALSE(measured.value(QStringLiteral("hdrActive")).toBool());
    EXPECT_TRUE(measured.value(QStringLiteral("automaticColorManagement")).toBool());
    EXPECT_EQ(measured.value(QStringLiteral("colorAvailability")).toString(), QStringLiteral("available"));
}

// The DXGI output walk and Qt's screen list are two independent enumerations of
// the same monitors. A positional join reads correctly only while the two happen
// to agree, and reports one monitor's HDR state under the other's name the moment
// they do not -- silently, because both payloads are well-formed. So the fixture
// hands the DXGI facts over in the OPPOSITE order and asserts HDR still lands on
// the panel that has it.
TEST(EnvironmentSnapshotJson, DisplayFactsJoinByNameNotByEnumerationOrder) {
    EnvironmentSnapshotInputs inputs;
    ScreenFacts sdr_screen;
    sdr_screen.name = QStringLiteral("27GL650F");
    inputs.screens.push_back(sdr_screen);
    ScreenFacts hdr_screen;
    hdr_screen.name = QStringLiteral("27GL850");
    inputs.screens.push_back(hdr_screen);

    capability::DisplayHdrFacts hdr;
    hdr.name = "\\\\.\\DISPLAY1";
    hdr.friendly_name = "27GL850";
    hdr.hdr_active = true;
    hdr.max_luminance_nits = 600.0f;
    capability::DisplayHdrFacts sdr;
    sdr.name = "\\\\.\\DISPLAY2";
    sdr.friendly_name = "27GL650F";
    sdr.hdr_active = false;

    inputs.capabilities.probed = true;
    // Same length as the screen list -- which is exactly the case the old
    // index-based join accepted as unambiguous.
    inputs.capabilities.runtime.displays = {hdr, sdr};

    const QJsonArray screens = EnvironmentSnapshotToJson(inputs)
                                   .value(QStringLiteral("displays"))
                                   .toObject()
                                   .value(QStringLiteral("screens"))
                                   .toArray();
    ASSERT_EQ(screens.size(), 2);

    const QJsonObject sdr_entry = screens.at(0).toObject();
    EXPECT_EQ(sdr_entry.value(QStringLiteral("name")).toString(), QStringLiteral("27GL650F"));
    EXPECT_FALSE(sdr_entry.value(QStringLiteral("hdrActive")).isNull());
    EXPECT_FALSE(sdr_entry.value(QStringLiteral("hdrActive")).toBool());

    const QJsonObject hdr_entry = screens.at(1).toObject();
    EXPECT_EQ(hdr_entry.value(QStringLiteral("name")).toString(), QStringLiteral("27GL850"));
    EXPECT_TRUE(hdr_entry.value(QStringLiteral("hdrActive")).toBool());
    EXPECT_EQ(hdr_entry.value(QStringLiteral("maxLuminanceNits")).toDouble(), 600.0);
}

// No name, no join. The alternative is inheriting whichever DXGI entry sits at the
// same index, which is how an SDR monitor comes to report a neighbour's HDR state.
TEST(EnvironmentSnapshotJson, AnUnnamedDxgiDisplayMatchesNothingRatherThanItsNeighbour) {
    EnvironmentSnapshotInputs inputs;
    ScreenFacts screen;
    screen.name = QStringLiteral("27GL850");
    inputs.screens.push_back(screen);

    capability::DisplayHdrFacts facts;
    facts.name = "\\\\.\\DISPLAY1";
    facts.friendly_name = ""; // DisplayConfig answered nothing for this path
    facts.hdr_active = true;
    inputs.capabilities.probed = true;
    inputs.capabilities.runtime.displays.push_back(facts);

    const QJsonObject entry = EnvironmentSnapshotToJson(inputs)
                                  .value(QStringLiteral("displays"))
                                  .toObject()
                                  .value(QStringLiteral("screens"))
                                  .toArray()
                                  .at(0)
                                  .toObject();
    EXPECT_TRUE(entry.value(QStringLiteral("hdrActive")).isNull());
    // Probed, and this display was not among what the probe could name.
    EXPECT_EQ(entry.value(QStringLiteral("colorAvailability")).toString(), QStringLiteral("unsupported"));
}

TEST(EnvironmentSnapshotJson, PresentUnavailabilityNamesItsCauseInGateOrder) {
    EnvironmentSnapshotInputs inputs;

    const QJsonObject no_opt_in = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("present")).toObject();
    EXPECT_EQ(no_opt_in.value(QStringLiteral("availability")).toString(), QStringLiteral("requiresOptIn"));

    inputs.present.opt_in = true;
    const QJsonObject no_elevation = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("present")).toObject();
    EXPECT_EQ(no_elevation.value(QStringLiteral("availability")).toString(), QStringLiteral("requiresElevation"));

    inputs.present.elevated = true;
    const QJsonObject nothing_seen = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("present")).toObject();
    EXPECT_EQ(nothing_seen.value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));
    EXPECT_EQ(nothing_seen.value(QStringLiteral("reason")).toString(), QStringLiteral("noPresentObserved"));
    EXPECT_TRUE(nothing_seen.value(QStringLiteral("mode")).isNull());
    EXPECT_TRUE(nothing_seen.value(QStringLiteral("tearing")).isNull());

    diagnostics::PresentSample sample;
    sample.available = true;
    sample.mode = diagnostics::PresentMode::IndependentFlip;
    sample.tearing = true;
    sample.present_count = 900;
    sample.discarded_count = 0;
    inputs.present.available = true;
    inputs.present.sample = sample;

    const QJsonObject measured = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("present")).toObject();
    EXPECT_EQ(measured.value(QStringLiteral("availability")).toString(), QStringLiteral("available"));
    EXPECT_EQ(measured.value(QStringLiteral("mode")).toString(), QStringLiteral("independentFlip"));
    EXPECT_TRUE(measured.value(QStringLiteral("tearing")).toBool());
    EXPECT_EQ(measured.value(QStringLiteral("presentCount")).toDouble(), 900.0);
}

TEST(EnvironmentSnapshotJson, AudioEndpointsCarryNamesAndDefaultsButNoEndpointIds) {
    EnvironmentSnapshotInputs inputs;
    inputs.audio_observed = true;
    inputs.audio_inputs.push_back({QStringLiteral("Microphone (Yeti)"), true});
    inputs.audio_outputs.push_back({QStringLiteral("Speakers (Realtek)"), true});

    const QJsonObject audio = EnvironmentSnapshotToJson(inputs).value(QStringLiteral("audio")).toObject();
    EXPECT_EQ(audio.value(QStringLiteral("availability")).toString(), QStringLiteral("available"));
    const QJsonObject input = audio.value(QStringLiteral("inputs")).toArray().at(0).toObject();
    EXPECT_EQ(input.value(QStringLiteral("name")).toString(), QStringLiteral("Microphone (Yeti)"));
    EXPECT_TRUE(input.value(QStringLiteral("default")).toBool());
    // The WASAPI endpoint id answers nothing the friendly name does not, so it
    // never leaves the process.
    EXPECT_FALSE(input.contains(QStringLiteral("id")));
    EXPECT_FALSE(input.contains(QStringLiteral("deviceId")));
}

// ---------------------------------------------------------------------------
// windows.snapshot
// ---------------------------------------------------------------------------

TEST(WindowIdentity, EveryOverlayObjectNameMapsToItsOwnRole) {
    EXPECT_EQ(WindowRoleForObjectName(QString(), true), QStringLiteral("main"));
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayRecording"), false),
              QStringLiteral("recordingOverlay"));
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayDiagnostics"), false),
              QStringLiteral("diagnosticsOverlay"));
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayQuickControls"), false),
              QStringLiteral("quickControls"));
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayNotificationToast"), false),
              QStringLiteral("notificationToast"));
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayCountdown"), false), QStringLiteral("countdown"));
    // A new overlay must not be filed under an existing role by a prefix match.
    EXPECT_EQ(WindowRoleForObjectName(QStringLiteral("quickOverlayRecordingSomethingElse"), false),
              QStringLiteral("unknown"));
    EXPECT_EQ(WindowRoleForObjectName(QString(), false), QStringLiteral("unknown"));
}

TEST(WindowIdentity, SnapshotPublishesRoleUniquenessAndTheIdentityContract) {
    std::vector<WindowFacts> windows;
    windows.push_back({QStringLiteral("main"), QString(), QStringLiteral("ExoSnap"), true, true, true, {}, {}});
    windows.push_back({QStringLiteral("notificationToast"),
                       QStringLiteral("quickOverlayNotificationToast"),
                       QStringLiteral("ExoSnap Overlay \xE2\x80\x94 Notification"),
                       false,
                       false,
                       false,
                       {},
                       {}});

    const QJsonObject json = WindowSnapshotToJson(windows, 4242);
    EXPECT_EQ(json.value(QStringLiteral("count")).toInt(), 2);
    EXPECT_TRUE(json.value(QStringLiteral("rolesUnique")).toBool());
    EXPECT_TRUE(json.value(QStringLiteral("titlesUnique")).toBool());
    // The contract is data, so no consumer has to be told it out of band.
    EXPECT_EQ(json.value(QStringLiteral("identity")).toString(), QStringLiteral("role+processId"));
    EXPECT_EQ(json.value(QStringLiteral("processId")).toDouble(), 4242.0);

    const QJsonObject toast = json.value(QStringLiteral("windows")).toArray().at(1).toObject();
    EXPECT_EQ(toast.value(QStringLiteral("role")).toString(), QStringLiteral("notificationToast"));
    EXPECT_FALSE(toast.value(QStringLiteral("visible")).toBool());
    // A hidden window has no platform window, and the snapshot must not have
    // created one to answer.
    EXPECT_FALSE(toast.value(QStringLiteral("nativeWindowCreated")).toBool());
    EXPECT_FALSE(toast.contains(QStringLiteral("native")));
    EXPECT_EQ(toast.value(QStringLiteral("processId")).toDouble(), 4242.0);
}

TEST(WindowIdentity, TwoTopLevelWindowsSharingATitleIsReportedRatherThanHidden) {
    // The Wave B defect, as data: several top-level windows inherited the
    // application title. Roles stay unique; the titles do not, and the snapshot
    // has to say so instead of leaving it to be noticed by eye.
    std::vector<WindowFacts> windows;
    windows.push_back({QStringLiteral("main"), QString(), QStringLiteral("ExoSnap"), true, true, true, {}, {}});
    windows.push_back({QStringLiteral("countdown"),
                       QStringLiteral("quickOverlayCountdown"),
                       QStringLiteral("ExoSnap"),
                       true,
                       true,
                       true,
                       {},
                       {}});

    const QJsonObject json = WindowSnapshotToJson(windows, 1);
    EXPECT_TRUE(json.value(QStringLiteral("rolesUnique")).toBool());
    EXPECT_FALSE(json.value(QStringLiteral("titlesUnique")).toBool());
}

// ---------------------------------------------------------------------------
// events.recent
// ---------------------------------------------------------------------------

class EventQueryTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
        diagnostics::AppLog::init();
    }
    void SetUp() override {
        InitializeEngineLogging();
    }
    void TearDown() override {
        ShutdownEngineLogging();
    }
};

TEST_F(EventQueryTest, ReturnsNewestFirstAndRespectsTheBound) {
    for (int i = 0; i < 5; ++i) {
        const std::string code = "wavec.event." + std::to_string(i);
        diagnostics::logEvent(diagnostics::LogSeverity::Info, "wavec.order", code);
    }

    EventQueryFilter filter;
    filter.subsystem = QStringLiteral("wavec.order");
    filter.max = 2;
    const QJsonObject json = QueryEvents(filter);

    const QJsonArray events = json.value(QStringLiteral("events")).toArray();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events.at(0).toObject().value(QStringLiteral("eventCode")).toString(), QStringLiteral("wavec.event.4"));
    EXPECT_EQ(events.at(1).toObject().value(QStringLiteral("eventCode")).toString(), QStringLiteral("wavec.event.3"));
    EXPECT_EQ(json.value(QStringLiteral("matched")).toDouble(), 5.0);
    // `truncated` is what lets a reader tell "that is all of them" from "that is
    // the newest slice".
    EXPECT_TRUE(json.value(QStringLiteral("truncated")).toBool());
}

TEST_F(EventQueryTest, FiltersBySeverityFloorAndByCorrelationId) {
    diagnostics::logEvent(diagnostics::LogSeverity::Info, "wavec.filter", "wavec.info",
                          {{"recordingSessionId", "rec-A"}});
    diagnostics::logEvent(diagnostics::LogSeverity::Error, "wavec.filter", "wavec.error",
                          {{"recordingSessionId", "rec-B"}});

    EventQueryFilter severity;
    severity.subsystem = QStringLiteral("wavec.filter");
    severity.min_severity = QStringLiteral("error");
    const QJsonArray errors = QueryEvents(severity).value(QStringLiteral("events")).toArray();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_EQ(errors.at(0).toObject().value(QStringLiteral("eventCode")).toString(), QStringLiteral("wavec.error"));

    EventQueryFilter correlated;
    correlated.subsystem = QStringLiteral("wavec.filter");
    correlated.recording_session_id = QStringLiteral("rec-A");
    const QJsonArray one_session = QueryEvents(correlated).value(QStringLiteral("events")).toArray();
    ASSERT_EQ(one_session.size(), 1);
    const QJsonObject event = one_session.at(0).toObject();
    EXPECT_EQ(event.value(QStringLiteral("recordingSessionId")).toString(), QStringLiteral("rec-A"));
    // Correlation ids are promoted out of the fields rather than parsed out of
    // the message text.
    EXPECT_EQ(event.value(QStringLiteral("fields")).toObject().value(QStringLiteral("recordingSessionId")).toString(),
              QStringLiteral("rec-A"));
    // An event that belongs to no update transaction says so.
    EXPECT_TRUE(event.value(QStringLiteral("updateTransactionId")).isNull());
}

TEST_F(EventQueryTest, TheLaunchSessionIdIsStampedOnEveryRecord) {
    diagnostics::logEvent(diagnostics::LogSeverity::Info, "wavec.launch", "wavec.stamped");

    EventQueryFilter filter;
    filter.subsystem = QStringLiteral("wavec.launch");
    const QJsonArray events = QueryEvents(filter).value(QStringLiteral("events")).toArray();
    ASSERT_FALSE(events.isEmpty());
    EXPECT_EQ(events.at(0).toObject().value(QStringLiteral("launchSessionId")).toString(),
              diagnostics::AppLog::sessionId());
}

TEST_F(EventQueryTest, AMistypedSeverityIsRejectedRatherThanWideningTheQuery) {
    QJsonObject params;
    params.insert(QStringLiteral("severity"), QStringLiteral("scream"));
    QString error;
    const EventQueryFilter filter = ParseEventQueryFilter(params, &error);
    EXPECT_EQ(filter.min_severity, QStringLiteral("scream"));
    // Falling back to "everything" would make a check that filters for errors
    // pass on a stream full of Info records.
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(error.contains(QStringLiteral("scream")));
}

TEST_F(EventQueryTest, TheQueryIsBoundedByTheRingAndNeverUnbounded) {
    EventQueryFilter filter;
    filter.max = 100000;
    const QJsonObject json = QueryEvents(filter);
    EXPECT_LE(json.value(QStringLiteral("max")).toInt(), kMaxEvents);
    EXPECT_LE(json.value(QStringLiteral("events")).toArray().size(), kMaxEvents);
}

} // namespace
