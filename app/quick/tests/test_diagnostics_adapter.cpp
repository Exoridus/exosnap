// The QML boundary: probe results in, already-resolved presentation values out.
// The adapter never performs I/O here — probe results are injected — so these tests
// exercise the boundary shape, not the filesystem.

#include "DiagnosticIssueModel.h"
#include "DiagnosticsAdapter.h"
#include "LogsAdapter.h"
#include "PipelineStageModel.h"
#include "SessionLedgerModel.h"

#include "diagnostics/WindowTargetFacts.h"
#include "services/SupportBundleService.h"
#include "visual_tests/DiagnosticsLiveScenario.h"

#include <QCoreApplication>
#include <QUrl>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <vector>

using namespace exosnap;
using namespace exosnap::quick;

namespace {

// The adapters marshal worker results through QCoreApplication::instance() and
// AppLog resolves paths through QStandardPaths, so every case needs a live
// application object.
QCoreApplication* EnsureApplication() {
    if (auto* existing = QCoreApplication::instance())
        return existing;
    static int argc = 1;
    static char app_name[] = "diagnostics_adapter_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// Minimal signal counter — avoids pulling Qt Test into a plain gtest target.
class SignalCounter {
  public:
    template <typename Sender, typename Signal> SignalCounter(Sender* sender, Signal signal) {
        QObject::connect(sender, signal, sender, [this]() { ++count_; });
    }

    [[nodiscard]] int count() const noexcept {
        return count_;
    }

  private:
    int count_ = 0;
};

// Captures the first string payload alongside the count.
class StringSignalCounter {
  public:
    template <typename Sender, typename Signal> StringSignalCounter(Sender* sender, Signal signal) {
        QObject::connect(sender, signal, sender, [this](const QString& value) {
            ++count_;
            last_ = value;
        });
    }

    [[nodiscard]] int count() const noexcept {
        return count_;
    }
    [[nodiscard]] const QString& last() const noexcept {
        return last_;
    }

  private:
    int count_ = 0;
    QString last_;
};

diagnostics::DiagnosticsController::Config MakeConfig() {
    diagnostics::DiagnosticsController::Config config;
    config.output_folder = "C:/Users/Someone/Videos/ExoSnap";
    config.hotkeys_ok = true;
    config.hotkeys_summary = "None configured";
    config.config_summary.entries.push_back({"Container", "MKV"});
    config.cap_summary.entries.push_back({"NVENC AV1", "Available", "available", true});
    config.cap_summary.entries.push_back({"NVENC HEVC", "Available", "available", true});
    return config;
}

diagnostics::DiagnosticsController::ProbeResult MakeProbe() {
    diagnostics::DiagnosticsController::ProbeResult probe;
    probe.free_bytes = 512ULL * 1024 * 1024 * 1024;
    probe.total_bytes = 1024ULL * 1024 * 1024 * 1024;
    probe.filesystem_name = "NTFS";
    probe.output_path_writable = true;
    probe.drive_label = "C:";
    return probe;
}

QVariantMap TileWithKey(const QVariantList& tiles, const QString& key) {
    for (const QVariant& tile : tiles) {
        const QVariantMap map = tile.toMap();
        if (map.value(QStringLiteral("key")).toString() == key)
            return map;
    }
    return {};
}

// Captures the first bool payload alongside the count.
class BoolSignalCounter {
  public:
    template <typename Sender, typename Signal> BoolSignalCounter(Sender* sender, Signal signal) {
        QObject::connect(sender, signal, sender, [this](bool value) {
            ++count_;
            last_ = value;
        });
    }

    [[nodiscard]] int count() const noexcept {
        return count_;
    }
    [[nodiscard]] bool last() const noexcept {
        return last_;
    }

  private:
    int count_ = 0;
    bool last_ = false;
};

QVariantList SeriesOf(const DiagnosticsAdapter& adapter, const QString& key) {
    for (const QVariant& tile : adapter.liveTiles()) {
        const QVariantMap map = tile.toMap();
        if (map.value(QStringLiteral("key")).toString() == key)
            return map.value(QStringLiteral("series")).toList();
    }
    return {};
}

bool HasLedgerEntry(const SessionLedgerModel& model, const QString& id) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.data(model.index(row, 0), SessionLedgerModel::EntryIdRole).toString() == id)
            return true;
    }
    return false;
}

// The judder fixture sits at 7.9 ms, just under the check's 8 ms threshold; the
// ledger only has something to record once rec.001 actually fires. `elapsed_s`
// is what makes one snapshot distinct from the next: the ledger's entry rule
// counts measurements, so re-evaluating one snapshot must not advance it.
exosnap::engine::RecordingDiagnosticsSnapshot JudderSnapshot(double elapsed_s = 184.0) {
    exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("judder"));
    snapshot.capture.source_present_jitter_ms = 9.0;
    snapshot.elapsed_seconds = elapsed_s;
    return snapshot;
}

// A recording that has measured judder often enough for the ledger to hold it.
// The live rail is throttled to 2 Hz, so the snapshots are pushed a full second
// apart and each one is a genuinely new measurement.
void RecordJudder(DiagnosticsAdapter& adapter, int samples = 4) {
    for (int i = 0; i < samples; ++i) {
        adapter.applyLiveDiagnostics(JudderSnapshot(184.0 + i));
        adapter.refreshForTest();
    }
}

bool HasIssueWithId(DiagnosticsAdapter& adapter, const QString& id) {
    auto* model = qobject_cast<DiagnosticIssueModel*>(adapter.issues());
    if (model == nullptr)
        return false;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->data(model->index(row, 0), DiagnosticIssueModel::IssueIdRole).toString() == id)
            return true;
    }
    return false;
}

diagnostics::LogEntry MakeLogEntry(quint64 sequence, diagnostics::LogSeverity severity, const char* message) {
    diagnostics::LogEntry entry;
    entry.sequence = sequence;
    entry.timestamp = QDateTime(QDate(2026, 6, 8), QTime(14, 22, 31));
    entry.severity = severity;
    entry.category = QStringLiteral("Record");
    entry.message = QString::fromUtf8(message);
    return entry;
}

} // namespace

TEST(DiagnosticsAdapterTest, StartsNeutralWithoutTouchingTheFilesystem) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    EXPECT_EQ(adapter.verdictState(), QStringLiteral("neutral"));
    EXPECT_EQ(adapter.verdictHeadline(), QStringLiteral("Not checked yet"));
    EXPECT_FALSE(adapter.dataReady());
    EXPECT_FALSE(adapter.checking());
    EXPECT_EQ(adapter.lastCheckText(), QStringLiteral("Not checked yet"));
    // The four readiness tiles exist even before any data lands, so the row
    // is never half-built while the first probe runs.
    EXPECT_EQ(adapter.tiles().size(), 4);
}

TEST(DiagnosticsAdapterTest, ProbeResultDrivesTheDiskTile) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    adapter.applyProbeResultForTest(MakeProbe());

    const QVariantMap disk = TileWithKey(adapter.tiles(), QStringLiteral("disk"));
    EXPECT_EQ(disk.value(QStringLiteral("value")).toString(), QStringLiteral("512 GB"));
    EXPECT_TRUE(disk.value(QStringLiteral("hasUsageBar")).toBool());
    EXPECT_EQ(disk.value(QStringLiteral("usagePercent")).toInt(), 50);
    EXPECT_EQ(disk.value(QStringLiteral("sub")).toString(), QString::fromUtf8("free \xc2\xb7 C:"));
}

TEST(DiagnosticsAdapterTest, LastCheckTextIsStampedOnlyAfterAProbe) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    SignalCounter spy(&adapter, &DiagnosticsAdapter::lastCheckChanged);
    adapter.applyProbeResultForTest(MakeProbe());
    EXPECT_GE(spy.count(), 1);
    // The band has no Run check button, so its stamp states the recheck policy
    // rather than leaving the reader to wonder whether the page is stale.
    EXPECT_TRUE(adapter.lastCheckText().startsWith(QStringLiteral("Checked ")));
    EXPECT_TRUE(adapter.lastCheckText().contains(QStringLiteral("rechecks every 10 s")));
}

TEST(DiagnosticsAdapterTest, TheStampReportsTheSessionWhileOneIsRunning) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    adapter.applyLiveDiagnostics(visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy")));
    EXPECT_TRUE(adapter.lastCheckText().startsWith(QStringLiteral("Recording since ")));
    EXPECT_TRUE(adapter.lastCheckText().contains(QStringLiteral("live 5x/s")));
}

// A finished recording is reported by the Last session card, not by a fifth
// readiness tile: readiness answers what the machine can do next.
TEST(DiagnosticsAdapterTest, AFinishedRecordingDoesNotAddAReadinessTile) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    EXPECT_EQ(adapter.tiles().size(), 4);

    SignalCounter spy(&adapter, &DiagnosticsAdapter::hasLastRecordingChanged);
    adapter.setHasLastRecording(true);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(adapter.hasLastRecording());
    EXPECT_EQ(adapter.tiles().size(), 4);

    // Idempotent: a repeated push must not churn the tiles.
    adapter.setHasLastRecording(true);
    EXPECT_EQ(spy.count(), 1);
}

// ── QCR-110: the two host-side facts that had no producer at all ────────────────
//
// The engine's own coverage proves the cards are correct once the facts arrive.
// These pin the boundary the shipping frontend now feeds: a fact pushed through
// the adapter must reach the issue model, or the blocker fires with no card to
// explain it.

namespace {

// AV1/Opus in MKV — a clean profile, so anything that shows up is the card under
// test rather than an unrelated blocker.
diagnostics::DiagnosticsController::Config MakeCaptureConfig() {
    diagnostics::DiagnosticsController::Config config = MakeConfig();
    config.caps.video_codecs[capability::VideoCodec::Av1] = {capability::SupportLevel::Available, ""};
    config.caps.video_codecs[capability::VideoCodec::Hevc] = {capability::SupportLevel::Available, ""};
    config.caps.audio_codecs[capability::AudioCodec::Opus] = {capability::SupportLevel::Available, ""};
    config.user_config.container = capability::Container::Matroska;
    config.user_config.video_codec = capability::VideoCodec::Av1;
    config.user_config.audio_codec = capability::AudioCodec::Opus;
    config.user_config.color_range = capability::ColorRange::Limited;
    return config;
}

diagnostics::WindowTargetFacts FullscreenShapedFacts() {
    diagnostics::WindowTargetFacts facts;
    facts.valid = true;
    facts.visible = true;
    facts.window_rect = RECT{0, 0, 1920, 1080};
    facts.monitor_rect = RECT{0, 0, 1920, 1080};
    facts.style = WS_POPUP | WS_VISIBLE;
    return facts;
}

bool HasIssueTitled(DiagnosticsAdapter& adapter, const QString& needle) {
    auto* model = qobject_cast<DiagnosticIssueModel*>(adapter.issues());
    if (model == nullptr)
        return false;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->data(model->index(row, 0), DiagnosticIssueModel::TitleRole).toString().contains(needle))
            return true;
    }
    return false;
}

} // namespace

TEST(DiagnosticsAdapterTest, ProvenBlackWindowEvidenceRaisesTheExclusiveFullscreenCard) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("exclusive fullscreen")));

    // FullscreenShaped and the capture API produced nothing for >= 2 s.
    adapter.setCaptureWindowEvidence(FullscreenShapedFacts(),
                                     diagnostics::WindowHubEvidence{exosnap::engine::HubFrameKind::None, 5.0, 0.0,
                                                                    /*fresh_frame_since_fullscreen_shape=*/false});
    EXPECT_TRUE(HasIssueTitled(adapter, QStringLiteral("exclusive fullscreen")));

    // Retargeting to a monitor withdraws it — the card describes a selection.
    adapter.setCaptureWindowEvidence(std::nullopt, {});
    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("exclusive fullscreen")));
}

TEST(DiagnosticsAdapterTest, HdrTargetFactRaisesTheHdrBlockerCard) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    diagnostics::DiagnosticsController::Config config = MakeCaptureConfig();
    config.caps.video_codecs[capability::VideoCodec::H264] = {capability::SupportLevel::Available, ""};
    config.user_config.video_codec = capability::VideoCodec::H264;
    config.user_config.hdr_mode = exosnap::engine::HdrMode::Hdr10;
    adapter.setDiagnosticConfig(std::move(config));

    // The recording gate already blocks this pairing. Without the display fact
    // the card that explains WHY never appeared, which is the whole finding.
    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("cannot record HDR10")));
    adapter.setCaptureTargetHdrActive(true);
    EXPECT_TRUE(HasIssueTitled(adapter, QStringLiteral("cannot record HDR10")));

    // An SDR desktop is not a problem: the HDR10-native path never engages.
    adapter.setCaptureTargetHdrActive(false);
    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("cannot record HDR10")));
}

// ── ADR 0033: the DPC/ISR producer that reached nothing ─────────────────────────
//
// RecommendationEngine::checkDpcLatency has been correct and covered from the day it
// landed, and it still never fired in a shipping build: DpcLatencyProvider.cpp was
// compiled by no target, and nothing called the setter. These cases pin the two halves
// the frontend now owns — a producer IS sampled where the engine runs, and a producer
// that is not measuring reports nothing rather than a peak nobody is updating.

namespace {

// The reading a caller would get from the real kernel trace, without one. The interface
// exists exactly so this is possible: opening a machine-wide named ETW session needs
// elevation, and a unit test may not tear one out from under a running ExoSnap.
class FakeDpcProvider final : public diagnostics::IDpcLatencyProvider {
  public:
    [[nodiscard]] diagnostics::DpcLatencyReading Read() const override {
        ++reads_;
        return reading_;
    }

    void setReading(diagnostics::DpcLatencyReading reading) {
        reading_ = std::move(reading);
    }
    [[nodiscard]] int reads() const noexcept {
        return reads_;
    }

  private:
    diagnostics::DpcLatencyReading reading_;
    mutable int reads_ = 0;
};

diagnostics::DpcLatencyReading MeasuredSpike() {
    // 2.5 ms peak, well past the 1 ms threshold, attributed to a named driver.
    return diagnostics::DpcLatencyReading{2500.0, 180.0, "nvlddmkm.sys", /*available=*/true};
}

} // namespace

TEST(DiagnosticsAdapterTest, MeasuredDpcLatencyRaisesTheDriverCard) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("DPC/ISR latency")));

    FakeDpcProvider provider;
    provider.setReading(MeasuredSpike());
    adapter.setDpcLatencyProvider(&provider);

    EXPECT_TRUE(HasIssueTitled(adapter, QStringLiteral("DPC/ISR latency")))
        << "a measured kernel-latency spike has to reach the Diagnostics surface";
    EXPECT_GE(provider.reads(), 1) << "the provider is sampled where the engine runs";
}

TEST(DiagnosticsAdapterTest, DpcLatencyThatStoppedBeingMeasuredStopsBeingReported) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());

    FakeDpcProvider provider;
    provider.setReading(MeasuredSpike());
    adapter.setDpcLatencyProvider(&provider);
    ASSERT_TRUE(HasIssueTitled(adapter, QStringLiteral("DPC/ISR latency")));

    // The trace stops (opt-in withdrawn, session torn down by another process, ETW
    // buffer error). What the provider then returns is the default reading: available
    // false and 0 us — a figure a threshold check would read as "no problem" and a value
    // row would read as "measured 0 us". Neither is true, so the engine is handed no
    // reading at all, and the peak measured a moment ago leaves the page with it.
    provider.setReading({});
    adapter.setSelectedCaptureTarget(std::nullopt); // any refresh of the surface

    EXPECT_FALSE(HasIssueTitled(adapter, QStringLiteral("DPC/ISR latency")))
        << "an unavailable reading must not leave the last measured peak on the page";
}

TEST(DiagnosticsAdapterTest, SelectedCaptureTargetNamesTheDisplayTilesSubject) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());

    exosnap::engine::CaptureTarget window;
    window.kind = exosnap::engine::CaptureTarget::Kind::Window;
    window.native_id = 0x1234;
    window.description = "Some Game";
    adapter.setSelectedCaptureTarget(window);

    const QVariantMap tile = TileWithKey(adapter.tiles(), QStringLiteral("display"));
    ASSERT_FALSE(tile.isEmpty());
    EXPECT_TRUE(tile.value(QStringLiteral("sub")).toString().contains(QStringLiteral("Some Game")));
}

TEST(DiagnosticsAdapterTest, AMonitorTargetIsNamedNotSpelledAsADevicePath) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());

    exosnap::engine::CaptureTarget monitor;
    monitor.kind = exosnap::engine::CaptureTarget::Kind::Monitor;
    monitor.native_id = 0x1;
    monitor.description = R"(\\.\DISPLAY1)";
    adapter.setSelectedCaptureTarget(monitor);

    const QVariantMap tile = TileWithKey(adapter.tiles(), QStringLiteral("display"));
    ASSERT_FALSE(tile.isEmpty());
    EXPECT_TRUE(tile.value(QStringLiteral("sub")).toString().contains(QStringLiteral("Desktop - Display 1")));
    EXPECT_FALSE(tile.value(QStringLiteral("sub")).toString().contains(QStringLiteral("DISPLAY1")));
}

TEST(DiagnosticsAdapterTest, InvalidProfileProducesBlockerCards) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    auto config = MakeConfig();
    config.profile_validation.succeeded = false;
    config.profile_validation.invalidity.push_back({"video_codec", "AV1 is unavailable"});
    adapter.setDiagnosticConfig(std::move(config));

    EXPECT_TRUE(adapter.hasIssues());
    auto* model = qobject_cast<DiagnosticIssueModel*>(adapter.issues());
    ASSERT_NE(model, nullptr);
    ASSERT_GE(model->rowCount(), 1);
    const QModelIndex index = model->index(0, 0);
    EXPECT_EQ(model->data(index, DiagnosticIssueModel::ToneRole).toString(), QStringLiteral("blocker"));
    EXPECT_EQ(model->data(index, DiagnosticIssueModel::TitleRole).toString(),
              QStringLiteral("Video codec is not supported"));
}

// An Auto fix is never applied by the click itself: the adapter asks for a confirm
// and only acceptFix() emits the intent the composition root acts on.
TEST(DiagnosticsAdapterTest, AutoFixRequiresAnExplicitConfirmStep) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    SignalCounter confirm_spy(&adapter, &DiagnosticsAdapter::fixConfirmRequested);
    StringSignalCounter accept_spy(&adapter, &DiagnosticsAdapter::applyFixAccepted);

    adapter.applyFix(QStringLiteral("fix.color.range"));
    EXPECT_EQ(confirm_spy.count(), 1);
    EXPECT_EQ(accept_spy.count(), 0);

    adapter.acceptFix(QStringLiteral("fix.color.range"));
    ASSERT_EQ(accept_spy.count(), 1);
    EXPECT_EQ(accept_spy.last(), QStringLiteral("fix.color.range"));
}

TEST(DiagnosticsAdapterTest, EmptyFixIdsAreIgnored) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    SignalCounter confirm_spy(&adapter, &DiagnosticsAdapter::fixConfirmRequested);
    SignalCounter assisted_spy(&adapter, &DiagnosticsAdapter::assistedFixRequested);
    adapter.applyFix(QString());
    adapter.openAssistedFix(QString());
    EXPECT_EQ(confirm_spy.count(), 0);
    EXPECT_EQ(assisted_spy.count(), 0);
}

TEST(DiagnosticsAdapterTest, NavigationIsAnIntentNotAPageSwitch) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    SignalCounter logs(&adapter, &DiagnosticsAdapter::navigateToLogsRequested);

    adapter.openLogs();
    EXPECT_EQ(logs.count(), 1);
}

// The in-depth switch reports intent; the setting itself is owned by the
// composition root, which pushes the answer back through setInDepthEnabled().
TEST(DiagnosticsAdapterTest, TheInDepthSwitchAsksAndDoesNotDecide) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    BoolSignalCounter toggled(&adapter, &DiagnosticsAdapter::inDepthToggled);

    EXPECT_FALSE(adapter.inDepthEnabled());
    EXPECT_TRUE(adapter.inDepthAvailable());
    adapter.setInDepthEnabledFromUi(true);
    EXPECT_EQ(toggled.count(), 1);
    EXPECT_TRUE(toggled.last());
    // Nothing moved here: the switch stays where it is until the setting comes back.
    EXPECT_FALSE(adapter.inDepthEnabled());

    adapter.setInDepthEnabled(true);
    EXPECT_TRUE(adapter.inDepthEnabled());
    // The opt-in persists across launches; elevation does not. On in a standard
    // process there is no ETW session, and the sub-text is the one place the spec
    // says the gate is stated.
    EXPECT_EQ(adapter.inDepthStateText(), QStringLiteral("On \xc2\xb7 not measuring \xc2\xb7 needs an admin relaunch"));

    adapter.setElevated(true);
    EXPECT_EQ(adapter.inDepthStateText(), QStringLiteral("On \xc2\xb7 elevated \xc2\xb7 PresentMon + DPC/ISR trace"));
}

TEST(DiagnosticsAdapterTest, InDepthCannotBeChangedWhileRecording) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    BoolSignalCounter toggled(&adapter, &DiagnosticsAdapter::inDepthToggled);

    adapter.applyLiveDiagnostics(visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy")));
    EXPECT_TRUE(adapter.recording());
    EXPECT_FALSE(adapter.inDepthAvailable());
    EXPECT_EQ(adapter.inDepthStateText(), QStringLiteral("Off \xc2\xb7 cannot change while recording"));

    adapter.setInDepthEnabledFromUi(true);
    EXPECT_EQ(toggled.count(), 0);
}

TEST(DiagnosticsAdapterTest, ShowInLogNamesTheDiagnosticThatRaisedIt) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    StringSignalCounter spy(&adapter, &DiagnosticsAdapter::showInLogRequested);

    adapter.showInLog(QString());
    EXPECT_EQ(spy.count(), 0);
    adapter.showInLog(QStringLiteral("rec.001"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.last(), QStringLiteral("rec.001"));
}

// The sparkline is the last 60 snapshots and nothing older, and a new recording
// starts from an empty trend rather than inheriting the previous one.
TEST(DiagnosticsAdapterTest, SparklineSeriesAreCappedAndResetPerSession) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    exosnap::engine::RecordingDiagnosticsSnapshot snapshot =
        visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy"));
    for (int i = 0; i < 65; ++i) {
        snapshot.capture.actual_fps = 59.0 + (i % 10) * 0.1;
        adapter.applyLiveDiagnostics(snapshot);
    }
    adapter.refreshForTest();
    EXPECT_EQ(SeriesOf(adapter, QStringLiteral("framePacing")).size(), 60);

    snapshot.session_generation = 2;
    adapter.applyLiveDiagnostics(snapshot);
    adapter.refreshForTest();
    EXPECT_EQ(SeriesOf(adapter, QStringLiteral("framePacing")).size(), 1);
}

TEST(DiagnosticsAdapterTest, LiveTilesCarryTheTintOfTheCheckThatOwnsTheValue) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    adapter.applyLiveDiagnostics(visual::MakeDiagnosticsLiveSnapshot(QStringLiteral("healthy")));

    ASSERT_FALSE(adapter.liveTiles().isEmpty());
    for (const QVariant& tile : adapter.liveTiles()) {
        const QVariantMap map = tile.toMap();
        EXPECT_TRUE(map.contains(QStringLiteral("valueTone")))
            << map.value(QStringLiteral("key")).toString().toStdString();
        EXPECT_TRUE(map.contains(QStringLiteral("subTone")));
        EXPECT_TRUE(map.contains(QStringLiteral("series")));
        EXPECT_TRUE(map.contains(QStringLiteral("sessionDetail")));
    }
    // No check has fired, so every owned number reads as measured and inside budget.
    EXPECT_EQ(
        TileWithKey(adapter.liveTiles(), QStringLiteral("framePacing")).value(QStringLiteral("valueTone")).toString(),
        QStringLiteral("ok"));
    // A codec name is owned by nothing and must never carry a verdict colour.
    EXPECT_EQ(TileWithKey(adapter.liveTiles(), QStringLiteral("encoder")).value(QStringLiteral("valueTone")).toString(),
              QStringLiteral("neutral"));
}

// The ledger is republished on the live cadence with the same entries. A whole-
// list assignment would destroy every expanded card underneath the reader.
TEST(DiagnosticsAdapterTest, TheLedgerKeepsItsRowsAcrossEvaluations) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    adapter.applyProbeResultForTest(MakeProbe());

    // Four distinct snapshots: the entry rule counts measurements, not passes.
    RecordJudder(adapter);

    auto* ledger = qobject_cast<SessionLedgerModel*>(adapter.ledger());
    ASSERT_NE(ledger, nullptr);
    ASSERT_GT(ledger->rowCount(), 0);
    EXPECT_TRUE(HasLedgerEntry(*ledger, QStringLiteral("rec.001")));

    SignalCounter inserted(ledger, &QAbstractItemModel::rowsInserted);
    SignalCounter resets(ledger, &QAbstractItemModel::modelReset);
    adapter.refreshForTest();
    adapter.refreshForTest();
    EXPECT_EQ(inserted.count(), 0);
    EXPECT_EQ(resets.count(), 0);
}

// A measured problem is told once. While recording the ledger owns that story,
// so the card list must not repeat the entry above it.
TEST(DiagnosticsAdapterTest, MeasuredProblemsAreTheLedgersWhileRecording) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    adapter.applyProbeResultForTest(MakeProbe());
    RecordJudder(adapter);

    auto* ledger = qobject_cast<SessionLedgerModel*>(adapter.ledger());
    ASSERT_NE(ledger, nullptr);
    ASSERT_TRUE(HasLedgerEntry(*ledger, QStringLiteral("rec.001")));
    EXPECT_FALSE(HasIssueWithId(adapter, QStringLiteral("rec.001")));
}

// The entry rule counts measurements. refreshSnapshot() is reached from eight
// places -- a settings change, a display change, a probe result -- and every one
// of them re-evaluates the SAME snapshot, so a single spike must not enter.
TEST(DiagnosticsAdapterTest, ReEvaluatingOneSnapshotNeverRaisesALedgerEntry) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    adapter.applyProbeResultForTest(MakeProbe());

    adapter.applyLiveDiagnostics(JudderSnapshot(184.0));
    for (int i = 0; i < 6; ++i)
        adapter.refreshForTest();

    auto* ledger = qobject_cast<SessionLedgerModel*>(adapter.ledger());
    ASSERT_NE(ledger, nullptr);
    EXPECT_FALSE(HasLedgerEntry(*ledger, QStringLiteral("rec.001")))
        << "one measurement cannot satisfy the two-evaluation entry rule";

    adapter.applyLiveDiagnostics(JudderSnapshot(185.0));
    adapter.refreshForTest();
    EXPECT_TRUE(HasLedgerEntry(*ledger, QStringLiteral("rec.001")));
}

// Spec section 2: after Stop the band returns to the readiness verdict and says
// nothing about the session. Nothing else on that edge recomputes it.
TEST(DiagnosticsAdapterTest, StoppingReturnsTheBandToTheReadinessVerdict) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    adapter.applyProbeResultForTest(MakeProbe());

    RecordJudder(adapter);
    ASSERT_TRUE(adapter.verdictHeadline().startsWith(QStringLiteral("Recording")))
        << adapter.verdictHeadline().toStdString();

    exosnap::engine::RecordingDiagnosticsSnapshot done = JudderSnapshot(188.0);
    done.lifecycle = exosnap::engine::DiagnosticsLifecycle::Completed;
    adapter.applyLiveDiagnostics(done);

    EXPECT_FALSE(adapter.recording());
    EXPECT_FALSE(adapter.verdictHeadline().startsWith(QStringLiteral("Recording")))
        << adapter.verdictHeadline().toStdString();
}

// The session report is written on the recording thread and must never read the
// ledger from here. The freeze hands it over instead.
TEST(DiagnosticsAdapterTest, TheFrozenLedgerIsHandedOverOnceWhenTheRecordingEnds) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeCaptureConfig());
    adapter.applyProbeResultForTest(MakeProbe());

    SignalCounter frozen(&adapter, &DiagnosticsAdapter::sessionLedgerFrozen);
    RecordJudder(adapter);
    EXPECT_EQ(frozen.count(), 0) << "nothing is frozen while the recording runs";

    exosnap::engine::RecordingDiagnosticsSnapshot done = JudderSnapshot(188.0);
    done.lifecycle = exosnap::engine::DiagnosticsLifecycle::Completed;
    adapter.applyLiveDiagnostics(done);
    EXPECT_EQ(frozen.count(), 1);

    const std::vector<diagnostics::LedgerEntry> ledger = adapter.frozenLedger();
    ASSERT_EQ(ledger.size(), 1u);
    EXPECT_EQ(ledger.front().id, "rec.001");
    EXPECT_FALSE(ledger.front().active) << "a handed-over occurrence is closed";

    // A further terminal snapshot is not a second recording ending.
    adapter.applyLiveDiagnostics(done);
    EXPECT_EQ(frozen.count(), 1);
}

TEST(DiagnosticsAdapterTest, IdlePipelineShowsTheStaticReadinessStages) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    EXPECT_FALSE(adapter.pipelineLive());
    QAbstractListModel* stages = adapter.pipelineStages();
    ASSERT_NE(stages, nullptr);
    ASSERT_EQ(stages->rowCount(), 6);
    EXPECT_EQ(stages->data(stages->index(0), PipelineStageModel::StatusRole).toString(), QStringLiteral("planned"));
}

// QCR-604. The pipeline used to be a QVariantList, and a Repeater answers a
// whole-list assignment by destroying every delegate. The idle path republishes
// the identical six planned stages whenever the configuration is re-applied.
TEST(DiagnosticsAdapterTest, RepublishingTheSameIdlePipelineSaysNothing) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    auto* stages = qobject_cast<PipelineStageModel*>(adapter.pipelineStages());
    ASSERT_NE(stages, nullptr);
    SignalCounter resets(stages, &QAbstractItemModel::modelReset);
    SignalCounter changes(stages, &QAbstractItemModel::dataChanged);

    adapter.setDiagnosticConfig(MakeConfig());
    adapter.setDiagnosticConfig(MakeConfig());

    EXPECT_EQ(resets.count(), 0);
    EXPECT_EQ(changes.count(), 0);
}

TEST(DiagnosticsAdapterTest, LiveSnapshotSwitchesThePipelineToMeasuredStages) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    exosnap::engine::RecordingDiagnosticsSnapshot snapshot;
    snapshot.valid = true;
    snapshot.lifecycle = exosnap::engine::DiagnosticsLifecycle::Recording;
    snapshot.session_generation = 1;
    snapshot.capture.target_fps = 60.0;
    snapshot.capture.actual_fps = 59.4;
    adapter.applyLiveDiagnostics(snapshot);

    EXPECT_TRUE(adapter.pipelineLive());
    QAbstractListModel* stages = adapter.pipelineStages();
    ASSERT_NE(stages, nullptr);
    EXPECT_EQ(stages->data(stages->index(0), PipelineStageModel::ValueRole).toString(),
              QStringLiteral("59.4 / 60.0 fps"));
}

TEST(DiagnosticsAdapterTest, LiveTilesAppearWithARecordingAndVanishWhenItEnds) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    // Idle: the readiness tiles are still meaningful, the live summary is not.
    EXPECT_TRUE(adapter.liveTiles().isEmpty());

    exosnap::engine::RecordingDiagnosticsSnapshot snapshot;
    snapshot.valid = true;
    snapshot.lifecycle = exosnap::engine::DiagnosticsLifecycle::Recording;
    snapshot.session_generation = 1;
    snapshot.health = exosnap::engine::PipelineHealth::Good;
    snapshot.capture.target_fps = 60.0;
    snapshot.capture.actual_fps = 59.98;
    adapter.applyLiveDiagnostics(snapshot);

    ASSERT_EQ(adapter.liveTiles().size(), 4);
    EXPECT_EQ(adapter.liveTiles().at(0).toMap().value(QStringLiteral("key")).toString(), QStringLiteral("framePacing"));
    EXPECT_EQ(adapter.liveTiles().at(0).toMap().value(QStringLiteral("value")).toString(), QStringLiteral("59.98 fps"));

    // Leaving the recording lifecycle clears them on that very edge, not at the
    // next throttled tick: a live summary of a recording that has stopped is a
    // stale claim about something that is no longer happening.
    snapshot.lifecycle = exosnap::engine::DiagnosticsLifecycle::Completed;
    adapter.applyLiveDiagnostics(snapshot);
    EXPECT_TRUE(adapter.liveTiles().isEmpty());
}

TEST(DiagnosticsAdapterTest, SelfTestRowsOnlyAppearOnceAChecklistArrives) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    EXPECT_TRUE(adapter.selfTestRows().isEmpty());
    EXPECT_EQ(adapter.selfTestStatus(), QStringLiteral("Status: Not run"));

    auto probe = MakeProbe();
    probe.self_test_valid = true;
    diagnostics::DiagnosticResult pass;
    pass.title = "Self-test: Capture";
    pass.summary = "PASS";
    pass.severity = diagnostics::DiagnosticSeverity::Pass;
    probe.self_test.results.push_back(pass);
    adapter.applyProbeResultForTest(std::move(probe));

    ASSERT_EQ(adapter.selfTestRows().size(), 1);
    EXPECT_EQ(adapter.selfTestStatus(), QStringLiteral("Status: PASS"));
    EXPECT_FALSE(adapter.selfTestRows().at(0).toMap().value(QStringLiteral("notRun")).toBool());
}

TEST(DiagnosticsAdapterTest, BundleFileNameIsTimestampedZip) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    const QString name = adapter.defaultBundleFileName();
    EXPECT_TRUE(name.startsWith(QStringLiteral("exosnap-support-")));
    EXPECT_TRUE(name.endsWith(QStringLiteral(".zip")));
}

// ── LogsAdapter ─────────────────────────────────────────────────────────────────

TEST(LogsAdapterTest, FilterAndSearchDriveTheStatusLine) {
    EnsureApplication();
    LogsAdapter adapter;
    adapter.setSyntheticEntries({MakeLogEntry(1, diagnostics::LogSeverity::Info, "entry 0"),
                                 MakeLogEntry(2, diagnostics::LogSeverity::Info, "entry 1"),
                                 MakeLogEntry(3, diagnostics::LogSeverity::Error, "entry 2")});

    EXPECT_EQ(adapter.totalCount(), 3);
    EXPECT_EQ(adapter.visibleCount(), 3);
    EXPECT_TRUE(adapter.statusText().contains(QStringLiteral("Showing 3 of 3 entries")));

    adapter.setSeverityFilter(2); // Issues
    EXPECT_EQ(adapter.visibleCount(), 1);
    EXPECT_TRUE(adapter.statusText().contains(QStringLiteral("Showing 1 of 3 entries")));
    EXPECT_TRUE(adapter.statusText().contains(QStringLiteral("Issues")));
}

TEST(LogsAdapterTest, CopyAndExportGatesFollowTheCounts) {
    EnsureApplication();
    LogsAdapter adapter;
    adapter.setSyntheticEntries({});
    EXPECT_FALSE(adapter.canCopy());
    EXPECT_FALSE(adapter.canExport());

    adapter.setSyntheticEntries({MakeLogEntry(1, diagnostics::LogSeverity::Info, "started")});
    EXPECT_TRUE(adapter.canCopy());
    EXPECT_TRUE(adapter.canExport());
}

TEST(LogsAdapterTest, AutoScrollIsAPlainToggle) {
    EnsureApplication();
    LogsAdapter adapter;
    SignalCounter spy(&adapter, &LogsAdapter::autoScrollChanged);
    EXPECT_TRUE(adapter.autoScroll());
    adapter.setAutoScroll(false);
    adapter.setAutoScroll(false);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(adapter.autoScroll());
}

TEST(LogsAdapterTest, SupportBundleIsRoutedNotDuplicated) {
    EnsureApplication();
    LogsAdapter adapter;
    int requests = 0;
    QObject::connect(&adapter, &LogsAdapter::createSupportBundleRequested, &adapter,
                     [&requests](const QUrl&) { ++requests; });
    adapter.createSupportBundle(QUrl::fromLocalFile(QStringLiteral("C:/does-not-exist/bundle.zip")));
    EXPECT_EQ(requests, 1);
}

TEST(LogsAdapterTest, LogFolderPathFallsBackToTheCanonicalLocation) {
    EnsureApplication();
    LogsAdapter adapter;
    // AppLog::init() has not run in this test binary, so no real path exists yet.
    EXPECT_FALSE(adapter.logFolderPath().isEmpty());
}

// ── SupportBundleService mapping ────────────────────────────────────────────────

TEST(SupportBundleInputsTest, MapsContextAndAdaptersOntoPlainBundleData) {
    SupportBundleContext context;
    context.log_dir = QStringLiteral("C:/logs");
    context.launch_session_id = QStringLiteral("abc-123");
    context.created_at = QStringLiteral("2026-06-08T14:22:31");
    context.settings_summary = QStringLiteral("Container: MKV\n");

    capability::CapabilitySet caps;
    caps.gpu_adapter_name = "NVIDIA GeForce RTX 5070 Ti";

    capability::AdapterInfo adapter;
    adapter.name = "GeForce RTX 5070 Ti";
    adapter.vendor = capability::AdapterVendor::Nvidia;
    adapter.kind = capability::AdapterKind::Discrete;
    adapter.dedicated_video_memory_bytes = 16ULL * 1024 * 1024 * 1024;

    const diagnostics::BundleInputs inputs = BuildSupportBundleInputs(context, caps, {adapter});
    EXPECT_EQ(inputs.log_dir, QStringLiteral("C:/logs"));
    EXPECT_EQ(inputs.launch_session_id, QStringLiteral("abc-123"));
    EXPECT_EQ(inputs.settings_summary, QStringLiteral("Container: MKV\n"));
    EXPECT_EQ(inputs.capability.gpu_adapter_name, QStringLiteral("NVIDIA GeForce RTX 5070 Ti"));
    ASSERT_EQ(inputs.adapters.size(), 1U);
    EXPECT_EQ(inputs.adapters[0].vendor, QStringLiteral("NVIDIA"));
    EXPECT_EQ(inputs.adapters[0].kind, QStringLiteral("discrete"));
    EXPECT_FALSE(inputs.app_version.isEmpty());
}

TEST(SupportBundleServiceTest, RejectsAWriteWithNoDestination) {
    EnsureApplication();
    SupportBundleService service;
    std::vector<bool> results;
    QObject::connect(&service, &SupportBundleService::finished, &service,
                     [&results](bool ok, const QString&) { results.push_back(ok); });
    service.createAsync(QString(), {}, {});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results[0]);
    EXPECT_FALSE(service.busy());
}

TEST(SupportBundleServiceTest, RejectsAWriteWithNoLogDirectory) {
    EnsureApplication();
    SupportBundleService service;
    std::vector<bool> results;
    QObject::connect(&service, &SupportBundleService::finished, &service,
                     [&results](bool ok, const QString&) { results.push_back(ok); });
    service.createAsync(QStringLiteral("C:/does-not-exist/bundle.zip"), {}, {});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results[0]);
}

// ---------------------------------------------------------------------------
// Issue model update contract (QCR-405)
// ---------------------------------------------------------------------------
//
// While a recording runs, applyLiveDiagnostics() re-runs the recommendation
// engine at 2 Hz and hands the resulting cards to this model. A reset tells QML
// "different rows now", so QML destroys every delegate and builds new ones —
// twice a second, taking the expanded Evidence disclosure with it. The cards
// are almost always identical, and when they are not they are usually the same
// issues carrying new measurements. Only a change to the issue SET is
// structural.

namespace {

diagnostics::IssueCard MakeCard(const char* id, const char* title, const char* measured = "") {
    diagnostics::IssueCard card;
    card.id = id;
    card.tone = diagnostics::IssueTone::Notice;
    card.title = title;
    card.summary = "summary";
    card.measured = measured;
    return card;
}

// Counts both halves of a reset plus every dataChanged, so a test can say which
// of the three paths the model took.
class ModelChangeCounter {
  public:
    explicit ModelChangeCounter(QAbstractItemModel* model) {
        QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, model, [this]() { ++resets_; });
        QObject::connect(model, &QAbstractItemModel::dataChanged, model,
                         [this](const QModelIndex& first, const QModelIndex&, const QList<int>&) {
                             ++data_changes_;
                             changed_rows_.push_back(first.row());
                         });
    }

    [[nodiscard]] int resets() const noexcept {
        return resets_;
    }
    [[nodiscard]] int dataChanges() const noexcept {
        return data_changes_;
    }
    [[nodiscard]] const std::vector<int>& changedRows() const noexcept {
        return changed_rows_;
    }

  private:
    int resets_ = 0;
    int data_changes_ = 0;
    std::vector<int> changed_rows_;
};

} // namespace

TEST(DiagnosticIssueModelTest, IdenticalCardsChangeNothingAtAll) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("ENC-01", "Encoder falling behind", "62 fps")});

    ModelChangeCounter counter(&model);
    model.setCards({MakeCard("ENC-01", "Encoder falling behind", "62 fps")});
    model.setCards({MakeCard("ENC-01", "Encoder falling behind", "62 fps")});

    EXPECT_EQ(counter.resets(), 0) << "the 2 Hz live refresh delivers this same list over and over";
    EXPECT_EQ(counter.dataChanges(), 0);
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(DiagnosticIssueModelTest, SameIssuesWithNewValuesAreARowUpdate) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards(
        {MakeCard("ENC-01", "Encoder falling behind", "62 fps"), MakeCard("DSK-02", "Disk is slow", "48 MB/s")});

    ModelChangeCounter counter(&model);
    model.setCards(
        {MakeCard("ENC-01", "Encoder falling behind", "62 fps"), MakeCard("DSK-02", "Disk is slow", "31 MB/s")});

    EXPECT_EQ(counter.resets(), 0) << "a delegate's expanded Evidence must survive a measurement update";
    ASSERT_EQ(counter.dataChanges(), 1);
    ASSERT_EQ(counter.changedRows().size(), 1U);
    EXPECT_EQ(counter.changedRows()[0], 1) << "only the row whose value moved";
    EXPECT_EQ(model.data(model.index(1), DiagnosticIssueModel::MeasuredRole).toString(), QStringLiteral("31 MB/s"));
}

TEST(DiagnosticIssueModelTest, AnAddedIssueIsStructural) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("ENC-01", "Encoder falling behind")});

    ModelChangeCounter counter(&model);
    model.setCards({MakeCard("ENC-01", "Encoder falling behind"), MakeCard("DSK-02", "Disk is slow")});

    EXPECT_EQ(counter.resets(), 1);
    EXPECT_EQ(model.rowCount(), 2);
}

TEST(DiagnosticIssueModelTest, AResolvedIssueIsStructural) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("ENC-01", "Encoder falling behind"), MakeCard("DSK-02", "Disk is slow")});

    ModelChangeCounter counter(&model);
    model.setCards({MakeCard("ENC-01", "Encoder falling behind")});

    EXPECT_EQ(counter.resets(), 1);
    EXPECT_EQ(model.rowCount(), 1);
}

// Worst-first ordering can genuinely swap two cards. Row 0 becoming a different
// issue is not an update of row 0.
TEST(DiagnosticIssueModelTest, AReorderedIssueSetIsStructural) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("ENC-01", "Encoder falling behind"), MakeCard("DSK-02", "Disk is slow")});

    ModelChangeCounter counter(&model);
    model.setCards({MakeCard("DSK-02", "Disk is slow"), MakeCard("ENC-01", "Encoder falling behind")});

    EXPECT_EQ(counter.resets(), 1);
    EXPECT_EQ(model.data(model.index(0), DiagnosticIssueModel::IssueIdRole).toString(), QStringLiteral("DSK-02"));
}

// Synthesised cards (profile invalidity, hotkey conflicts) carry no id, so the
// title is what tells two of them apart.
TEST(DiagnosticIssueModelTest, TwoIdlessCardsAreToldApartByTitle) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("", "The saved preset is no longer valid")});

    ModelChangeCounter counter(&model);
    model.setCards({MakeCard("", "Two hotkeys are bound to the same combination")});

    EXPECT_EQ(counter.resets(), 1);
    EXPECT_EQ(counter.dataChanges(), 0);
}

TEST(DiagnosticIssueModelTest, ClearingEverySurfacedIssueIsStructural) {
    EnsureApplication();
    DiagnosticIssueModel model;
    model.setCards({MakeCard("ENC-01", "Encoder falling behind")});

    ModelChangeCounter counter(&model);
    model.setCards({});

    EXPECT_EQ(counter.resets(), 1);
    EXPECT_EQ(model.rowCount(), 0);
}
