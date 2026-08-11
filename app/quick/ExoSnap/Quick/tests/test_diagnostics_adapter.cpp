// The QML boundary: probe results in, already-resolved presentation values out.
// The adapter never performs I/O here — probe results are injected — so these tests
// exercise the boundary shape, not the filesystem.

#include "DiagnosticIssueModel.h"
#include "DiagnosticsAdapter.h"
#include "LogsAdapter.h"

#include "services/SupportBundleService.h"

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
    EXPECT_EQ(adapter.lastCheckText(), QString::fromUtf8("Last check: \xe2\x80\x94"));
    // Six core tiles even before any data lands; the Last-session tile is gated.
    EXPECT_EQ(adapter.tiles().size(), 6);
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
    EXPECT_TRUE(adapter.lastCheckText().startsWith(QStringLiteral("Last check: ")));
    EXPECT_FALSE(adapter.lastCheckText().endsWith(QString::fromUtf8("\xe2\x80\x94")));
}

TEST(DiagnosticsAdapterTest, LastSessionTileAppearsOnlyAfterARecording) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());
    EXPECT_TRUE(TileWithKey(adapter.tiles(), QStringLiteral("session")).isEmpty());

    SignalCounter spy(&adapter, &DiagnosticsAdapter::hasLastRecordingChanged);
    adapter.setHasLastRecording(true);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(adapter.hasLastRecording());
    EXPECT_FALSE(TileWithKey(adapter.tiles(), QStringLiteral("session")).isEmpty());

    // Idempotent: a repeated push must not churn the tiles.
    adapter.setHasLastRecording(true);
    EXPECT_EQ(spy.count(), 1);
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
    SignalCounter device(&adapter, &DiagnosticsAdapter::navigateToDeviceRequested);
    SignalCounter report(&adapter, &DiagnosticsAdapter::openLastReportRequested);

    adapter.openLogs();
    adapter.openDevice();
    adapter.openLastReport(); // gated: no completed recording yet
    EXPECT_EQ(logs.count(), 1);
    EXPECT_EQ(device.count(), 1);
    EXPECT_EQ(report.count(), 0);

    adapter.setHasLastRecording(true);
    adapter.openLastReport();
    EXPECT_EQ(report.count(), 1);
}

TEST(DiagnosticsAdapterTest, ExpertModeIsAToggleThatOnlyNotifiesOnChange) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    SignalCounter spy(&adapter, &DiagnosticsAdapter::expertModeChanged);
    EXPECT_FALSE(adapter.expertMode());
    adapter.setExpertMode(true);
    adapter.setExpertMode(true);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(adapter.expertMode());
}

TEST(DiagnosticsAdapterTest, IdlePipelineShowsTheStaticReadinessStages) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    EXPECT_FALSE(adapter.pipelineLive());
    ASSERT_EQ(adapter.pipelineStages().size(), 6);
    EXPECT_EQ(adapter.pipelineStages().at(0).toMap().value(QStringLiteral("status")).toString(),
              QStringLiteral("planned"));
}

TEST(DiagnosticsAdapterTest, LiveSnapshotSwitchesThePipelineToMeasuredStages) {
    EnsureApplication();
    DiagnosticsAdapter adapter;
    adapter.setDiagnosticConfig(MakeConfig());

    recorder_core::RecordingDiagnosticsSnapshot snapshot;
    snapshot.valid = true;
    snapshot.lifecycle = recorder_core::DiagnosticsLifecycle::Recording;
    snapshot.session_generation = 1;
    snapshot.capture.target_fps = 60.0;
    snapshot.capture.actual_fps = 59.4;
    adapter.applyLiveDiagnostics(snapshot);

    EXPECT_TRUE(adapter.pipelineLive());
    EXPECT_EQ(adapter.pipelineStages().at(0).toMap().value(QStringLiteral("value")).toString(),
              QStringLiteral("59.4 / 60.0 fps"));
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
