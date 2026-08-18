// The Diagnostics live summary: five tiles that answer, while a recording is
// running, the questions the page could not answer in Simple mode at all.
//
// The property under test throughout is that the tiles are a RENDERING of the
// engine's verdict and never a second opinion about it. A tile is amber because
// PipelineHealth says Warning and PipelineBottleneck names that tile's stage --
// not because a threshold was re-implemented here.
//
// The scenarios are the same fixtures the visual harness seeds through
// applyLiveDiagnostics(), so a capture and these assertions describe one thing.

#include "diagnostics/DiagnosticsController.h"
#include "observability/PipelineSnapshotJson.h"
#include "visual_tests/DiagnosticsLiveScenario.h"

#include <gtest/gtest.h>

#include <QJsonObject>

#include <algorithm>
#include <string>

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

TEST(DiagnosticsLiveTiles, AnIdlePipelineProducesNoTilesAtAll) {
    // Five tiles of em dashes would be worse than none: the page would look like
    // it is reporting on a recording that is not happening.
    EXPECT_TRUE(TilesFor("idle").empty());
    EXPECT_TRUE(BuildLiveTiles({}).empty());
}

TEST(DiagnosticsLiveTiles, AHealthyRecordingAnswersAllFiveQuestionsCalmly) {
    const std::vector<LiveTile> tiles = TilesFor("healthy");
    ASSERT_EQ(tiles.size(), 5u);

    const LiveTile health = Find(tiles, "pipelineHealth");
    EXPECT_EQ(health.value, "Good");
    EXPECT_EQ(health.sub, "No sustained bottleneck");
    EXPECT_EQ(health.detail, "Problem drops 0");
    // Nothing is wrong, so nothing is coloured. The 622 coalesced drops in the
    // fixture are benign and must not reach the tile.
    for (const LiveTile& tile : tiles)
        EXPECT_EQ(tile.tone, TileTone::Neutral) << tile.key;
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
    recorder_core::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.encoder_init = {}; // valid == false: nothing has been configured
    s.video_encoder.codec = recorder_core::VideoCodec::Hevc;

    const LiveTile encoder = Find(BuildLiveTiles(s), "encoder");
    EXPECT_EQ(encoder.value, "HEVC");
    // A default-constructed EncoderInitInfo would have answered "AV1 · P4 · CQ 0",
    // which is a plausible-looking sentence about an encoder nobody started.
    EXPECT_EQ(encoder.value.find("P4"), std::string::npos);
    EXPECT_EQ(encoder.value.find("CQ"), std::string::npos);
}

TEST(DiagnosticsLiveTiles, EncoderPressureColoursOnlyTheEncoderTile) {
    const std::vector<LiveTile> tiles = TilesFor("encoder");
    ASSERT_EQ(tiles.size(), 5u);

    // The health tile carries the verdict; the encoder tile carries the
    // attribution. Nothing else moves, because the engine did not blame it.
    EXPECT_EQ(Find(tiles, "pipelineHealth").tone, TileTone::Notice);
    EXPECT_EQ(Find(tiles, "pipelineHealth").value, "Warning");
    EXPECT_EQ(Find(tiles, "pipelineHealth").sub, "Video encoder");
    // The engine's own sentence, not one written by the presentation layer.
    EXPECT_EQ(Find(tiles, "pipelineHealth").detail, "Encoder latency is approaching the frame budget.");
    EXPECT_EQ(Find(tiles, "encoder").tone, TileTone::Notice);
    EXPECT_EQ(Find(tiles, "framePacing").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "audioSync").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "storage").tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, ABottleneckInAHealthyPipelineDoesNotColourAnything) {
    // The engine names a bottleneck long before it calls the pipeline unwell.
    // Colouring on attribution alone would paint a tile amber on a recording the
    // engine reported as Good, which is the one thing this surface must not do.
    recorder_core::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.bottleneck = recorder_core::PipelineBottleneck::VideoEncoder;
    s.health = recorder_core::PipelineHealth::Good;

    for (const LiveTile& tile : BuildLiveTiles(s))
        EXPECT_EQ(tile.tone, TileTone::Neutral) << tile.key;
}

TEST(DiagnosticsLiveTiles, DiskPressureColoursStorageAndReportsTheShrinkingEstimate) {
    const std::vector<LiveTile> tiles = TilesFor("disk");
    const LiveTile storage = Find(tiles, "storage");
    EXPECT_EQ(storage.tone, TileTone::Notice);
    EXPECT_EQ(storage.value, "21 MiB/s");
    EXPECT_EQ(storage.sub, "Write failures 1");
    EXPECT_NE(storage.detail.find("Est. remaining"), std::string::npos);
    EXPECT_NE(storage.detail.find("15"), std::string::npos) << "900 s is 15 minutes";
}

TEST(DiagnosticsLiveTiles, AnUnavailableRemainingTimeSaysSoInsteadOfShowingZero) {
    recorder_core::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
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
    EXPECT_EQ(Find(tiles, "pipelineHealth").tone, TileTone::Neutral);
    EXPECT_EQ(Find(tiles, "pipelineHealth").value, "Good");
}

TEST(DiagnosticsLiveTiles, AudioSyncReportsUnavailableDriftRatherThanPerfectSync) {
    recorder_core::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.av_drift_availability = recorder_core::MetricAvailability::Unavailable;
    s.peak_av_drift_availability = recorder_core::MetricAvailability::Unavailable;

    const LiveTile audio = Find(BuildLiveTiles(s), "audioSync");
    // "+0.0 ms" would claim a measurement of perfect sync on a recording whose
    // drift nothing measured -- a multi-source merge mixes device clocks and
    // does not report at all.
    EXPECT_EQ(audio.value, "Unavailable");
    EXPECT_EQ(audio.sub, "48 kHz \xc2\xb7 Stereo");
}

TEST(DiagnosticsLiveTiles, ARecordingWithoutAudioSaysSoInsteadOfShowingZeroDrift) {
    recorder_core::RecordingDiagnosticsSnapshot s = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    s.audio.active = false;

    const LiveTile audio = Find(BuildLiveTiles(s), "audioSync");
    EXPECT_EQ(audio.value, "No audio");
    EXPECT_EQ(audio.tone, TileTone::Neutral);
}

TEST(DiagnosticsLiveTiles, APausedRecordingStillHasALivePipelineToReportOn) {
    const std::vector<LiveTile> tiles = TilesFor("paused");
    ASSERT_EQ(tiles.size(), 5u);
    EXPECT_EQ(Find(tiles, "pipelineHealth").value, "Good");
}

TEST(DiagnosticsLiveTiles, ACompletedSessionHasNoLiveSummaryEvenThoughItsNumbersAreReal) {
    // The final snapshot is valid and full of real measurements, but they answer
    // "how did it go" -- which the Edit review step owns. Left on a surface
    // headed "live", they would report a stopped recording as a running one.
    EXPECT_TRUE(TilesFor("post").empty());

    recorder_core::RecordingDiagnosticsSnapshot failed = visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    failed.lifecycle = recorder_core::DiagnosticsLifecycle::Failed;
    EXPECT_TRUE(BuildLiveTiles(failed).empty());
}

TEST(DiagnosticsLiveTiles, TheTileAndTheStructuredSnapshotNeverDisagree) {
    // The consistency requirement, pinned rather than hoped for: an agent reading
    // pipeline.snapshot and a user reading the Diagnostics page must not be able
    // to reach opposite conclusions about the same recording. Both read the SAME
    // RecordingDiagnosticsSnapshot, so the only way they could differ is if one
    // of them classified for itself -- which is exactly what this asserts they
    // do not do.
    for (const char* kind : {"healthy", "encoder", "disk", "judder", "degraded", "paused", "split"}) {
        const recorder_core::RecordingDiagnosticsSnapshot snapshot =
            visual::MakeDiagnosticsLiveSnapshot(QString::fromLatin1(kind));
        const QJsonObject json = observability::PipelineSnapshotToJson(snapshot);
        const LiveTile health = Find(BuildLiveTiles(snapshot), "pipelineHealth");

        EXPECT_EQ(QString::fromStdString(health.value), json.value(QStringLiteral("health")).toString()) << kind;
        // Tone follows the health word, so a Good pipeline can never be painted
        // as a warning and a Warning one can never be painted as calm.
        const bool unwell = json.value(QStringLiteral("health")).toString() == QLatin1String("Warning") ||
                            json.value(QStringLiteral("health")).toString() == QLatin1String("Critical");
        EXPECT_EQ(health.tone != TileTone::Neutral, unwell) << kind;
    }
}

TEST(DiagnosticsLiveTiles, TheTileSetIsStableAcrossIdenticalSnapshots) {
    // The adapter publishes only on a real change, and that comparison is over
    // these values. Two identical snapshots producing unequal tiles would make
    // the page repaint five tiles twice a second forever.
    EXPECT_EQ(TilesFor("healthy"), TilesFor("healthy"));
    EXPECT_NE(TilesFor("healthy"), TilesFor("encoder"));
}

} // namespace
} // namespace exosnap::diagnostics
