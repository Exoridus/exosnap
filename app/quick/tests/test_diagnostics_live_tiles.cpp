// The Diagnostics live summary: four tiles that answer, while a recording is
// running, the questions the page could not answer in Simple mode at all, plus
// the four in-depth tiles the elevated present/DPC traces add.
//
// Two properties are under test. The TILE tone is a rendering of the engine's
// verdict and never a second opinion about it: a tile is amber because
// PipelineHealth says Warning and PipelineBottleneck names that tile's stage.
// The VALUE tint is a rendering of the session ledger: a number is green only
// while the check that owns it has never fired, amber once it has, and neutral
// when no check owns it at all.
//
// The scenarios are the same fixtures the visual harness seeds through
// applyLiveDiagnostics(), so a capture and these assertions describe one thing.

#include "diagnostics/DiagnosticsController.h"
#include "diagnostics/SessionLedger.h"
#include "observability/PipelineSnapshotJson.h"
#include "visual_tests/DiagnosticsLiveScenario.h"

#include <gtest/gtest.h>

#include <QJsonObject>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace exosnap::diagnostics {
namespace {

// Returns a COPY, not a pointer into the list. A pointer would dangle the moment
// a caller wrote Find(BuildLiveTiles(s), "encoder") -- the vector is a temporary
// and dies at the end of that expression.
LiveTile Find(const std::vector<LiveTile>& tiles, const std::string& key) {
    const auto it = std::find_if(tiles.begin(), tiles.end(), [&](const LiveTile& tile) { return tile.key == key; });
    return it == tiles.end() ? LiveTile{} : *it;
}

std::vector<LiveTile> TilesFor(const char* kind) {
    return BuildLiveTiles(visual::MakeDiagnosticsLiveSnapshot(QString::fromLatin1(kind)));
}

DiagnosticResult MeasuredProblem(const std::string& id) {
    DiagnosticResult result;
    result.id = id;
    result.tier = DiagnosticTier::MeasuredProblem;
    result.severity = DiagnosticSeverity::Notice;
    result.title = id;
    result.measured_value = 9.0;
    result.budget_value = 8.0;
    result.value_unit = "ms";
    return result;
}

// A ledger in which `id` has entered and is either still firing or has gone quiet.
SessionLedger LedgerWith(const std::string& id, bool active) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem(id)}, 1.0);
    ledger.Observe({MeasuredProblem(id)}, 1.5);
    if (!active)
        ledger.Observe({}, 2.0);
    return ledger;
}

TEST(DiagnosticsLiveTiles, AnIdlePipelineProducesNoTilesAtAll) {
    // Five tiles of em dashes would be worse than none: the page would look like
    // it is reporting on a recording that is not happening.
    EXPECT_TRUE(TilesFor("idle").empty());
    EXPECT_TRUE(BuildLiveTiles({}).empty());
}

TEST(DiagnosticsLiveTiles, AHealthyRecordingAnswersItsFourQuestionsCalmly) {
    const std::vector<LiveTile> tiles = TilesFor("healthy");
    ASSERT_EQ(tiles.size(), 4u);
    EXPECT_EQ(tiles[0].key, "framePacing");
    EXPECT_EQ(tiles[1].key, "encoder");
    EXPECT_EQ(tiles[2].key, "audioSync");
    EXPECT_EQ(tiles[3].key, "storage");
    // Nothing is wrong, so nothing is coloured. The 622 coalesced drops in the
    // fixture are benign and must not reach the tile.
    for (const LiveTile& tile : tiles)
        EXPECT_EQ(tile.tone, TileTone::Neutral) << tile.key;
}

TEST(DiagnosticsLiveTiles, PipelineHealthTileIsGone) {
    // The health word and the bottleneck name are the engine's own vocabulary and
    // belong to the pipeline stage cards. A tile repeating them told the reader
    // there was a problem without ever saying which measurement said so.
    for (const char* kind : {"healthy", "encoder", "disk", "judder", "degraded", "paused", "split"})
        EXPECT_EQ(Find(TilesFor(kind), "pipelineHealth").key, std::string()) << kind;
}

TEST(DiagnosticsLiveTiles, TheEncoderTileReportsWhatIsRunningAndNotWhatWasRequested) {
    const std::vector<LiveTile> tiles = TilesFor("healthy");
    const LiveTile encoder = Find(tiles, "encoder");
    // From EncoderInitInfo -- the encoder's own initialization record.
    EXPECT_NE(encoder.value.find("AV1"), std::string::npos);
    EXPECT_NE(encoder.value.find("P6"), std::string::npos);
    EXPECT_NE(encoder.value.find("CQ 17"), std::string::npos);
    // p99 against the frame budget, which is where the headroom question lives.
    EXPECT_NE(encoder.sub.find("p99"), std::string::npos);
    EXPECT_NE(encoder.sub.find("16.67"), std::string::npos);
    EXPECT_EQ(encoder.detail, "Backlog 0");
}

TEST(DiagnosticsLiveTiles, AnUnconfiguredEncoderFallsBackToTheStreamCodecAndNotToADefaultPreset) {
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.encoder_init = {}; // valid == false: nothing has been configured
    s.video_encoder.codec = exosnap::engine::VideoCodec::Hevc;

    const LiveTile encoder = Find(BuildLiveTiles(s), "encoder");
    EXPECT_EQ(encoder.value, "HEVC");
    // A default-constructed EncoderInitInfo would have answered "AV1 · P4 · CQ 0",
    // which is a plausible-looking sentence about an encoder nobody started.
    EXPECT_EQ(encoder.value.find("P4"), std::string::npos);
    EXPECT_EQ(encoder.value.find("CQ"), std::string::npos);
}

TEST(DiagnosticsLiveTiles, EncoderPressureColoursOnlyTheEncoderTile) {
    const std::vector<LiveTile> tiles = TilesFor("encoder");
    ASSERT_EQ(tiles.size(), 4u);

    // The encoder tile carries the engine's attribution. Nothing else moves,
    // because the engine did not blame it.
    EXPECT_EQ(Find(tiles, "encoder").tone, TileTone::Notice);
    EXPECT_EQ(Find(tiles, "framePacing").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "audioSync").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "storage").tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, ABottleneckInAHealthyPipelineDoesNotColourAnything) {
    // The engine names a bottleneck long before it calls the pipeline unwell.
    // Colouring on attribution alone would paint a tile amber on a recording the
    // engine reported as Good, which is the one thing this surface must not do.
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.bottleneck = exosnap::engine::PipelineBottleneck::VideoEncoder;
    s.health = exosnap::engine::PipelineHealth::Good;

    for (const LiveTile& tile : BuildLiveTiles(s))
        EXPECT_EQ(tile.tone, TileTone::Neutral) << tile.key;
}

TEST(DiagnosticsLiveTiles, DiskPressureColoursStorageAndReportsTheShrinkingEstimate) {
    const std::vector<LiveTile> tiles = TilesFor("disk");
    const LiveTile storage = Find(tiles, "storage");
    EXPECT_EQ(storage.tone, TileTone::Notice);
    EXPECT_EQ(storage.value, "21 MiB/s");
    EXPECT_NE(storage.sub.find("Write failures 1"), std::string::npos);
    EXPECT_EQ(storage.sub_tinted, "peak write 190 ms");
    EXPECT_NE(storage.detail.find("Est. remaining"), std::string::npos);
    EXPECT_NE(storage.detail.find("15"), std::string::npos) << "900 s is 15 minutes";
}

TEST(DiagnosticsLiveTiles, AnUnavailableRemainingTimeSaysSoInsteadOfShowingZero) {
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.disk_fill_eta_seconds = -1.0;

    const LiveTile storage = Find(BuildLiveTiles(s), "storage");
    EXPECT_EQ(storage.detail, "Remaining time unavailable");
}

TEST(DiagnosticsLiveTiles, PresentDiagnosticsNameTheirOwnAbsence) {
    const LiveTile off = Find(TilesFor("present-unavailable"), "framePacing");
    // Not blank and not an em dash: an unexplained "unavailable" reads as a
    // defect, and the cause is an opt-in the user controls.
    EXPECT_NE(off.detail.find("Present diagnostics unavailable"), std::string::npos);
    EXPECT_NE(off.detail.find("opt-in"), std::string::npos);

    const LiveTile on = Find(TilesFor("healthy"), "framePacing");
    EXPECT_EQ(on.detail, "Independent flip \xc2\xb7 no tearing");
    // The two scenarios differ in the present gate and nothing else, so the
    // measurement above it has to be identical.
    EXPECT_EQ(on.value, off.value);
    EXPECT_EQ(on.sub, off.sub);
}

TEST(DiagnosticsLiveTiles, FramePacingCarriesTargetOutputAndSourceJitter) {
    const LiveTile pacing = Find(TilesFor("judder"), "framePacing");
    EXPECT_EQ(pacing.value, "58.10 fps");
    EXPECT_NE(pacing.sub.find("Target 60 fps"), std::string::npos);
    EXPECT_NE(pacing.sub.find("jitter 7.9 ms"), std::string::npos);
    EXPECT_NE(pacing.detail.find("tearing active"), std::string::npos);
    EXPECT_EQ(pacing.tone, TileTone::Notice);
}

TEST(DiagnosticsLiveTiles, ALostAudioSourceIsANoticeEvenWhileThePipelineIsHealthy) {
    const std::vector<LiveTile> tiles = TilesFor("degraded");
    const LiveTile audio = Find(tiles, "audioSync");
    // ADR 0046: the recording keeps running, so this is a calm measured notice
    // and never escalates past one -- but it is not silent either.
    EXPECT_EQ(audio.tone, TileTone::Notice);
    EXPECT_NE(audio.detail.find("1 source(s) silent"), std::string::npos);
    // Nothing else is coloured: the engine still calls the pipeline healthy.
    EXPECT_EQ(Find(tiles, "framePacing").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "storage").tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, AudioSyncSeparatesAFaultedMeasurementFromAnAbsentOne) {
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.av_drift_availability = exosnap::engine::MetricAvailability::Faulted;

    const LiveTile audio = Find(BuildLiveTiles(s), "audioSync");
    // "Unavailable" invites waiting for a number that already arrived and was
    // wrong. The tile says which of the two happened, and says what broke
    // without claiming a defect in the recording that nothing measured.
    EXPECT_EQ(audio.value, "Not measurable");
    EXPECT_NE(audio.detail.find("audio device stopped reporting its clock"), std::string::npos);
    EXPECT_EQ(audio.tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, AudioSyncReportsUnavailableDriftRatherThanPerfectSync) {
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.av_drift_availability = exosnap::engine::MetricAvailability::Unavailable;
    s.peak_av_drift_availability = exosnap::engine::MetricAvailability::Unavailable;

    const LiveTile audio = Find(BuildLiveTiles(s), "audioSync");
    // "+0.0 ms" would claim a measurement of perfect sync on a recording whose
    // drift nothing measured -- a multi-source merge mixes device clocks and
    // does not report at all.
    EXPECT_EQ(audio.value, "Unavailable");
    EXPECT_EQ(audio.sub, "48 kHz \xc2\xb7 Stereo");
}

TEST(DiagnosticsLiveTiles, ARecordingWithoutAudioSaysSoInsteadOfShowingZeroDrift) {
    exosnap::engine::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.audio.active = false;

    const LiveTile audio = Find(BuildLiveTiles(s), "audioSync");
    EXPECT_EQ(audio.value, "No audio");
    EXPECT_EQ(audio.tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, APausedRecordingStillHasALivePipelineToReportOn) {
    const std::vector<LiveTile> tiles = TilesFor("paused");
    ASSERT_EQ(tiles.size(), 4u);
    EXPECT_EQ(Find(tiles, "framePacing").value, "59.98 fps");
}

TEST(DiagnosticsLiveTiles, ACompletedSessionHasNoLiveSummaryEvenThoughItsNumbersAreReal) {
    // The final snapshot is valid and full of real measurements, but they answer
    // "how did it go" -- which the Edit review step owns. Left on a surface
    // headed "live", they would report a stopped recording as a running one.
    EXPECT_TRUE(TilesFor("post").empty());

    exosnap::engine::RecordingDiagnosticsSnapshot failed =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    failed.lifecycle = exosnap::engine::DiagnosticsLifecycle::Failed;
    EXPECT_TRUE(BuildLiveTiles(failed).empty());
}

TEST(DiagnosticsLiveTiles, TheTilesAndTheStructuredSnapshotNeverDisagree) {
    // The consistency requirement, pinned rather than hoped for: an agent reading
    // pipeline.snapshot and a user reading the Diagnostics page must not be able
    // to reach opposite conclusions about the same recording. Both read the SAME
    // RecordingDiagnosticsSnapshot, so the only way they could differ is if one
    // of them classified for itself -- which is exactly what this asserts they
    // do not do.
    const std::map<QString, std::string> stage_of = {
        {QStringLiteral("Capture"), "framePacing"},  {QStringLiteral("Compositor"), "framePacing"},
        {QStringLiteral("VideoEncoder"), "encoder"}, {QStringLiteral("Audio"), "audioSync"},
        {QStringLiteral("Muxer"), "storage"},        {QStringLiteral("Disk"), "storage"},
    };
    for (const char* kind : {"healthy", "encoder", "disk", "judder", "paused", "split"}) {
        const exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
            visual::MakeDiagnosticsLiveSnapshot(QString::fromLatin1(kind));
        const QJsonObject json = observability::PipelineSnapshotToJson(snapshot);
        const std::vector<LiveTile> tiles = BuildLiveTiles(snapshot);
        const QString health = json.value(QStringLiteral("health")).toString();
        const bool unwell = health == QLatin1String("Warning") || health == QLatin1String("Critical");
        const auto stage = stage_of.find(json.value(QStringLiteral("bottleneck")).toString());

        if (unwell && stage != stage_of.end()) {
            EXPECT_NE(Find(tiles, stage->second).tone, TileTone::Neutral) << kind;
        } else {
            // A Good pipeline can never be painted as a warning. The two
            // self-standing measured notices (a lost audio source, a write
            // failure) have their own fixtures and are asserted separately.
            for (const LiveTile& tile : tiles)
                EXPECT_EQ(tile.tone, TileTone::Neutral) << kind << "/" << tile.key;
        }
    }
}

TEST(DiagnosticsLiveTiles, TheTileSetIsStableAcrossIdenticalSnapshots) {
    // The adapter publishes only on a real change, and that comparison is over
    // these values. Two identical snapshots producing unequal tiles would make
    // the page repaint five tiles twice a second forever.
    EXPECT_EQ(TilesFor("healthy"), TilesFor("healthy"));
    EXPECT_NE(TilesFor("healthy"), TilesFor("encoder"));
}

// ── Value tint, budgets, session figures ────────────────────────────────────────

TEST(DiagnosticsLiveTiles, FramePacingValueIsGreenOnlyWhileNoCheckThatOwnsItHasFired) {
    const exosnap::engine::RecordingDiagnosticsSnapshot healthy =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));

    const SessionLedger clean;
    const LiveTile calm = Find(BuildLiveTiles(LiveTileInputs{healthy, clean}), "framePacing");
    EXPECT_EQ(calm.value_tone, ValueTone::Ok);
    EXPECT_EQ(calm.sub_tone, ValueTone::Ok);

    const SessionLedger firing = LedgerWith("rec.001", /*active=*/true);
    const LiveTile now = Find(BuildLiveTiles(LiveTileInputs{healthy, firing}), "framePacing");
    EXPECT_EQ(now.value_tone, ValueTone::Warn);
    EXPECT_EQ(now.sub_tone, ValueTone::Warn);
    EXPECT_EQ(now.sub_tinted, "jitter 1.2 ms");

    // Quiet again: the big value is about now and goes back to green, the small
    // sub-value is about the session and stays amber.
    const SessionLedger quiet = LedgerWith("rec.001", /*active=*/false);
    const LiveTile after = Find(BuildLiveTiles(LiveTileInputs{healthy, quiet}), "framePacing");
    EXPECT_EQ(after.value_tone, ValueTone::Ok);
    EXPECT_EQ(after.sub_tone, ValueTone::Warn);
}

TEST(DiagnosticsLiveTiles, StorageThroughputHasNoOwnerAndStaysNeutral) {
    const exosnap::engine::RecordingDiagnosticsSnapshot disk =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("disk"));

    const SessionLedger clean;
    const LiveTile calm = Find(BuildLiveTiles(LiveTileInputs{disk, clean}), "storage");
    // 92 MiB/s is not fast or slow, it is what is being written. No check owns it.
    EXPECT_EQ(calm.value_tone, ValueTone::Neutral);

    const SessionLedger stalling = LedgerWith("rec.disk.writestall", /*active=*/true);
    const LiveTile stalled = Find(BuildLiveTiles(LiveTileInputs{disk, stalling}), "storage");
    EXPECT_EQ(stalled.value_tone, ValueTone::Neutral);
    EXPECT_EQ(stalled.sub_tone, ValueTone::Warn);
}

TEST(DiagnosticsLiveTiles, EncoderP99CarriesTheBudgetAndTheContentionCheckOwnsIt) {
    const exosnap::engine::RecordingDiagnosticsSnapshot healthy =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    const SessionLedger clean;
    const LiveTile encoder = Find(BuildLiveTiles(LiveTileInputs{healthy, clean}), "encoder");
    ASSERT_TRUE(encoder.budget.has_value());
    EXPECT_DOUBLE_EQ(*encoder.budget, 1000.0 / 60.0);
    EXPECT_EQ(encoder.sub_tinted, "p99 3.1 ms");
    // A codec name is not a measurement, so nothing tints it.
    EXPECT_EQ(encoder.value_tone, ValueTone::Neutral);

    const SessionLedger contended = LedgerWith("rec.gpu.contention", /*active=*/false);
    EXPECT_EQ(Find(BuildLiveTiles(LiveTileInputs{healthy, contended}), "encoder").sub_tone, ValueTone::Warn);
}

TEST(DiagnosticsLiveTiles, SessionDetailCarriesTheWholeRunAndNotTheLastPublish) {
    const std::vector<LiveTile> tiles = TilesFor("healthy");
    // 11038 emitted frames over 184 s.
    EXPECT_EQ(Find(tiles, "framePacing").session_detail, "session avg 59.99 fps");
    // The engine's encoder percentiles are rolling-window, so the session figure
    // for the encoder is the rate it actually got through.
    EXPECT_EQ(Find(tiles, "encoder").session_detail, "session avg 59.98 fps encoded");
    // 1.5 GiB over 184 s, not the throughput of the last publish window.
    EXPECT_EQ(Find(tiles, "storage").session_detail, "session avg 8 MiB/s");
    EXPECT_EQ(Find(tiles, "audioSync").session_detail, "session peak 1.7 ms");
}

TEST(DiagnosticsLiveTiles, FourTilesWithoutDepthEightWithIt) {
    const exosnap::engine::RecordingDiagnosticsSnapshot healthy =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    const SessionLedger clean;

    PresentSample present;
    present.available = true;
    present.attributed = true;
    present.mode = PresentMode::IndependentFlip;
    present.present_count = 1000;
    present.discarded_count = 20;
    present.mode_flip_count = 2;
    present.present_interval_ms = 8.3;

    DpcLatencyReading dpc;
    dpc.available = true;
    dpc.max_latency_us = 420.0;
    dpc.avg_latency_us = 90.0;
    dpc.worst_driver = "nvlddmkm.sys";

    EXPECT_EQ(BuildLiveTiles(LiveTileInputs{healthy, clean}).size(), 4u);

    const std::vector<LiveTile> deep =
        BuildLiveTiles(LiveTileInputs{healthy, clean, /*in_depth=*/true, present, dpc, /*gpu_exec_p99_ms=*/4.2});
    ASSERT_EQ(deep.size(), 8u);
    EXPECT_EQ(Find(deep, "presentMode").value, "Independent flip");
    EXPECT_EQ(Find(deep, "presentHealth").value, "2.0% discarded");
    EXPECT_EQ(Find(deep, "dpcLatency").value, "420 \xc2\xb5s");
    EXPECT_EQ(Find(deep, "gpuTime").value, "4.20 ms");
    ASSERT_TRUE(Find(deep, "gpuTime").budget.has_value());
    EXPECT_DOUBLE_EQ(*Find(deep, "gpuTime").budget, 1000.0 / 60.0);
}

TEST(DiagnosticsLiveTiles, AnInDepthTileWithNoReadingIsAbsentAndNotUnavailable) {
    const exosnap::engine::RecordingDiagnosticsSnapshot healthy =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    const SessionLedger clean;
    // The switch's own sub-text states why the traces are not running; a tile of
    // em dashes would say it a second time and read as a defect.
    const std::vector<LiveTile> tiles = BuildLiveTiles(LiveTileInputs{healthy, clean, /*in_depth=*/true});
    EXPECT_EQ(tiles.size(), 5u);
    EXPECT_EQ(Find(tiles, "presentMode").key, std::string());
    EXPECT_EQ(Find(tiles, "dpcLatency").key, std::string());
    EXPECT_EQ(Find(tiles, "gpuTime").key, "gpuTime");
}

// ── Readiness encoder tile ──────────────────────────────────────────────────────

TEST(DiagnosticsReadinessTiles, EncoderTileNamesTheGpuTrimmedWithBackendBadgeAndCodecChips) {
    capability::CapabilitySet caps;
    caps.gpu_adapter_name = "NVIDIA GeForce RTX 5070 Ti";
    caps.video_codecs[capability::VideoCodec::H264] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::Av1] = {capability::SupportLevel::Available, ""};
    caps.video_codecs[capability::VideoCodec::Hevc] = {capability::SupportLevel::NotImplemented, ""};

    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    inputs.gpu_adapter_name = caps.gpu_adapter_name;
    inputs.caps = &caps;
    inputs.video_codec = capability::VideoCodec::Av1;
    inputs.container = capability::Container::Matroska;

    const std::vector<ReadinessTile> tiles = BuildReadinessTiles(inputs);
    const auto it =
        std::find_if(tiles.begin(), tiles.end(), [](const ReadinessTile& tile) { return tile.key == "encoder"; });
    ASSERT_NE(it, tiles.end());
    // The vendor is already stated by the badge; the tile names what the user
    // recognises on the box.
    EXPECT_EQ(it->value, "GeForce RTX 5070 Ti");
    EXPECT_EQ(it->head_badge, "NVENC");
    ASSERT_EQ(it->chips.size(), 3u);
    EXPECT_EQ(it->chips[0].label, "H.264");
    EXPECT_EQ(it->chips[1].label, "HEVC");
    EXPECT_EQ(it->chips[2].label, "AV1");
    EXPECT_TRUE(it->chips[2].selected);
    EXPECT_FALSE(it->chips[0].selected);
    // Availability is the capability matrix's answer, not a guess from the name.
    EXPECT_TRUE(it->chips[0].available);
    EXPECT_FALSE(it->chips[1].available);
    EXPECT_TRUE(it->chips[2].available);
}

TEST(DiagnosticsReadinessTiles, WithoutACapabilitySetTheCodecRowStaysEmpty) {
    ReadinessTileInputs inputs;
    inputs.data_ready = true;
    inputs.gpu_adapter_name = "Intel(R) Arc A770";
    const std::vector<ReadinessTile> tiles = BuildReadinessTiles(inputs);
    const auto it =
        std::find_if(tiles.begin(), tiles.end(), [](const ReadinessTile& tile) { return tile.key == "encoder"; });
    ASSERT_NE(it, tiles.end());
    EXPECT_EQ(it->value, "Arc A770");
    // No capability answers means no claim about what this GPU can encode.
    EXPECT_TRUE(it->chips.empty());
}
} // namespace
} // namespace exosnap::diagnostics
