#include "OverlayTelemetryAdapter.h"

#include <QCoreApplication>
#include <gtest/gtest.h>
#include <memory>

namespace exosnap::quick {
namespace {

using namespace std::chrono_literals;

class OverlayTelemetryTest : public testing::Test {
  protected:
    int argc = 1;
    char program[16] = "overlay-test";
    char* argv[2] = {program, nullptr};
    std::unique_ptr<QCoreApplication> application;
    OverlayTelemetryAdapter::Clock::time_point now{};
    OverlayTelemetryAdapter adapter{[this] { return now; }};
    OverlayTelemetryInputs input;
    int publications = 0;

    void SetUp() override {
        if (!QCoreApplication::instance())
            application = std::make_unique<QCoreApplication>(argc, argv);
        input.recording_active = true;
        input.state = models::RecordingOverlayState::Recording;
        input.elapsed = QStringLiteral("00:00:01");
        QObject::connect(&adapter, &OverlayTelemetryAdapter::snapshotChanged, [&] { ++publications; });
    }
    void advance(std::chrono::milliseconds duration) {
        now += duration;
        adapter.publishPending();
    }
};

TEST_F(OverlayTelemetryTest, FirstChangedSnapshotIsImmediateAndIdenticalInputIsSilent) {
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    EXPECT_EQ(adapter.snapshot().value("elapsedText").toString(), input.elapsed);
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    EXPECT_FALSE(adapter.hasPendingPublication());
}

TEST_F(OverlayTelemetryTest, ChangesCoalesceAtDeadlineWithLatestValues) {
    adapter.submit(input);
    now += 20ms;
    input.elapsed = QStringLiteral("00:00:02");
    adapter.submit(input);
    now += 30ms;
    input.elapsed = QStringLiteral("00:00:03");
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(adapter.hasPendingPublication());
    advance(149ms);
    EXPECT_EQ(publications, 1);
    advance(1ms);
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(adapter.snapshot().value("elapsedText").toString(), input.elapsed);
    EXPECT_FALSE(adapter.hasPendingPublication());
    advance(10s);
    EXPECT_EQ(publications, 2);
}

TEST_F(OverlayTelemetryTest, RevertingToPublishedSnapshotCancelsPendingWake) {
    adapter.submit(input);
    input.elapsed = QStringLiteral("00:00:02");
    adapter.submit(input);
    input.elapsed = QStringLiteral("00:00:01");
    adapter.submit(input);
    EXPECT_FALSE(adapter.hasPendingPublication());
    advance(1s);
    EXPECT_EQ(publications, 1);
}

TEST_F(OverlayTelemetryTest, ElapsedOnlyIgnoresHiddenTelemetryAndPublishesOncePerDisplayedSecond) {
    adapter.submit(input);
    for (int tick = 1; tick <= 100; ++tick) {
        now += 10ms;
        input.size = QString::number(tick);
        input.fps = QString::number(tick);
        adapter.submit(input);
    }
    EXPECT_EQ(publications, 1);
    input.elapsed = QStringLiteral("00:00:02");
    adapter.submit(input);
    EXPECT_EQ(publications, 2);
    EXPECT_FALSE(adapter.hasPendingPublication());
}

TEST_F(OverlayTelemetryTest, FormattedEqualityAndUnavailableValuesAreComparedVisibly) {
    input.diagnostics_active = true;
    input.diagnostics.fps = true;
    input.fps = QStringLiteral("60.0");
    adapter.submit(input);
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    input.fps.clear();
    adapter.submit(input);
    advance(200ms);
    EXPECT_EQ(publications, 2);
    EXPECT_TRUE(adapter.snapshot().value("fpsText").toString().isEmpty());
    input.fps = QStringLiteral("60.0");
    adapter.submit(input);
    advance(200ms);
    EXPECT_EQ(publications, 3);
}

TEST_F(OverlayTelemetryTest, PauseResumeWarningAndHiddenBypassPendingTelemetry) {
    adapter.submit(input);
    now += 1ms;
    input.elapsed = QStringLiteral("00:00:02");
    adapter.submit(input);
    for (const auto state : {models::RecordingOverlayState::Paused, models::RecordingOverlayState::Recording,
                             models::RecordingOverlayState::Warning, models::RecordingOverlayState::Recording,
                             models::RecordingOverlayState::Hidden}) {
        const int before = publications;
        input.state = state;
        input.recording_active = state != models::RecordingOverlayState::Hidden;
        adapter.submit(input);
        EXPECT_EQ(publications, before + 1);
        EXPECT_FALSE(adapter.hasPendingPublication());
    }
    advance(1s);
    EXPECT_EQ(publications, 6);
}

TEST_F(OverlayTelemetryTest, MuteAndDegradedAudioPublishImmediately) {
    input.diagnostics_active = true;
    adapter.submit(input);
    now += 1ms;
    input.mic_muted = true;
    adapter.submit(input);
    EXPECT_EQ(publications, 2);
    input.mic_muted = false;
    input.mic_degraded = true;
    adapter.submit(input);
    EXPECT_EQ(publications, 3);
    input.mic_degraded = false;
    adapter.submit(input);
    EXPECT_EQ(publications, 4);
}

TEST_F(OverlayTelemetryTest, HealthUsesEngineVerdictAndAttributionWithoutNewThresholds) {
    input.diagnostics_active = true;
    input.diagnostics.health = true;
    input.health = engine::PipelineHealth::Good;
    input.bottleneck = engine::PipelineBottleneck::VideoEncoder;
    adapter.submit(input);
    EXPECT_EQ(adapter.snapshot().value("healthText").toString(), QStringLiteral("OK"));
    EXPECT_FALSE(adapter.snapshot().value("healthWarning").toBool());
    input.health = engine::PipelineHealth::Warning;
    adapter.submit(input);
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(adapter.snapshot().value("healthText").toString(), QStringLiteral("ENC!"));
    EXPECT_TRUE(adapter.snapshot().value("healthWarning").toBool());
    input.health = engine::PipelineHealth::Unavailable;
    adapter.submit(input);
    EXPECT_TRUE(adapter.snapshot().value("healthText").toString().isEmpty());
}

TEST_F(OverlayTelemetryTest, SourceAndContentChangesBypassDeadline) {
    adapter.submit(input);
    input.source_identity = QStringLiteral("display:2");
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    input.recording.source_name = true;
    input.source_name = QStringLiteral("Display 2");
    adapter.submit(input);
    EXPECT_EQ(publications, 2);
    input.source_identity = QStringLiteral("display:1");
    input.source_name = QStringLiteral("Display 1");
    adapter.submit(input);
    EXPECT_EQ(publications, 3);
}

TEST_F(OverlayTelemetryTest, OrdinaryRapidChangesHaveAtLeastTwoHundredMillisecondsBetweenPublications) {
    input.diagnostics_active = true;
    input.diagnostics.fps = true;
    adapter.submit(input);
    for (int tick = 1; tick <= 1000; ++tick) {
        now += 1ms;
        input.fps = QString::number(tick);
        adapter.submit(input);
        adapter.publishPending();
        EXPECT_LE(publications, 1 + tick / 200);
    }
    EXPECT_EQ(publications, 6);
    EXPECT_EQ(adapter.snapshot().value("fpsText").toString(), input.fps);
}

TEST_F(OverlayTelemetryTest, HiddenContentDoesNotWakeAfterTerminalState) {
    adapter.submit(input);
    input.state = models::RecordingOverlayState::Hidden;
    input.recording_active = false;
    adapter.submit(input);
    const int hidden_publications = publications;
    for (int tick = 0; tick < 100; ++tick) {
        now += 10ms;
        input.elapsed = QString::number(tick);
        input.fps = QString::number(tick);
        input.size = QString::number(tick);
        adapter.submit(input);
        adapter.publishPending();
    }
    EXPECT_EQ(publications, hidden_publications);
    EXPECT_FALSE(adapter.hasPendingPublication());
}

TEST_F(OverlayTelemetryTest, EquivalentUnavailableRepresentationsDoNotPublish) {
    input.diagnostics_active = true;
    input.diagnostics.fps = true;
    input.diagnostics.size = true;
    adapter.submit(input);
    input.fps = QString(QChar(0x2014));
    input.size = QString(QChar(0x2014));
    adapter.submit(input);
    EXPECT_EQ(publications, 1);
    EXPECT_FALSE(adapter.hasPendingPublication());
    advance(200ms);
    EXPECT_EQ(publications, 1);
}

} // namespace
} // namespace exosnap::quick
