#include "DiagnosticsAdapter.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/DiagnosticsProbe.h"
#include "diagnostics/FixActionDispatcher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <QVariantMap>

#include <cmath>
#include <utility>

namespace exosnap::quick {
namespace {

QString Text(const std::string& value) {
    return QString::fromStdString(value);
}

QString Key(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QVariantMap TileToMap(const diagnostics::ReadinessTile& tile) {
    QVariantMap map;
    map.insert(QStringLiteral("key"), Text(tile.key));
    map.insert(QStringLiteral("title"), Text(tile.title));
    map.insert(QStringLiteral("value"), Text(tile.value));
    map.insert(QStringLiteral("sub"), Text(tile.sub));
    map.insert(QStringLiteral("tone"), Key(diagnostics::TileToneKey(tile.tone)));
    map.insert(QStringLiteral("hasUsageBar"), tile.has_usage_bar);
    map.insert(QStringLiteral("usagePercent"), tile.usage_percent);
    map.insert(QStringLiteral("showOkGlyph"), tile.show_ok_glyph);
    return map;
}

QVariantMap TipToMap(const diagnostics::TipEntry& tip) {
    QVariantMap map;
    map.insert(QStringLiteral("id"), Text(tip.id));
    map.insert(QStringLiteral("summary"), Text(tip.summary));
    map.insert(QStringLiteral("hasFix"), tip.has_fix);
    map.insert(QStringLiteral("fixId"), Text(tip.fix_id));
    map.insert(QStringLiteral("fixLabel"), Text(tip.fix_label));
    map.insert(QStringLiteral("changes"), Text(tip.changes));
    map.insert(QStringLiteral("fixSafety"), static_cast<int>(tip.fix_safety));
    return map;
}

QVariantMap RowToMap(const diagnostics::KeyValueRow& row) {
    QVariantMap map;
    map.insert(QStringLiteral("label"), Text(row.label));
    map.insert(QStringLiteral("value"), Text(row.value));
    return map;
}

QVariantMap SelfTestRowToMap(const diagnostics::SelfTestRow& row) {
    QVariantMap map;
    map.insert(QStringLiteral("title"), Text(row.title));
    map.insert(QStringLiteral("statusText"), Text(row.status_text));
    map.insert(QStringLiteral("detail"), Text(row.detail));
    map.insert(QStringLiteral("tone"), Key(diagnostics::IssueToneKey(row.tone)));
    map.insert(QStringLiteral("notRun"), row.not_run);
    return map;
}

QVariantMap StageToMap(const diagnostics::PipelineStage& stage) {
    QVariantMap map;
    map.insert(QStringLiteral("key"), Text(stage.key));
    map.insert(QStringLiteral("title"), Text(stage.title));
    map.insert(QStringLiteral("lane"), Text(stage.lane));
    map.insert(QStringLiteral("value"), Text(stage.value));
    map.insert(QStringLiteral("tip"), Text(stage.tip));
    map.insert(QStringLiteral("status"), Key(diagnostics::StageStatusKey(stage.status)));
    return map;
}

diagnostics::DiagnosticsController::DisplayFacts PrimaryDisplayFacts() {
    diagnostics::DiagnosticsController::DisplayFacts facts;
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        facts.width = screen->size().width();
        facts.height = screen->size().height();
        facts.refresh_hz = static_cast<int>(std::lround(screen->refreshRate()));
    }
    return facts;
}

// While recording, the writability + free-space facts are refreshed on this cadence.
constexpr int kLiveProbeIntervalMs = 10000;

} // namespace

DiagnosticsAdapter::DiagnosticsAdapter(QObject* parent)
    : QObject(parent), bundle_service_(std::make_unique<SupportBundleService>()) {
    controller_.SetDisplayFacts(PrimaryDisplayFacts());

    last_check_text_ = QStringLiteral("Last check: \xe2\x80\x94");
    self_test_status_ = QStringLiteral("Status: Not run");

    live_probe_timer_.setInterval(kLiveProbeIntervalMs);
    connect(&live_probe_timer_, &QTimer::timeout, this, [this]() {
        if (!probe_in_flight_)
            startProbe(false);
    });

    connect(bundle_service_.get(), &SupportBundleService::busyChanged, this, [this]() {
        const bool busy = bundle_service_->busy();
        if (bundle_busy_ == busy)
            return;
        bundle_busy_ = busy;
        emit bundleBusyChanged();
    });
    connect(bundle_service_.get(), &SupportBundleService::finished, this, [this](bool ok, const QString& message) {
        emit bundleFinished(ok, message);
        diagnostics::AppLog::info(QStringLiteral("diagnostics"),
                                  ok ? QStringLiteral("Support bundle written")
                                     : QStringLiteral("Support bundle failed: %1").arg(message));
    });

    // One probe at a time: probeInFlight already rejects overlapping requests,
    // and a second concurrent DXGI/COM enumeration buys nothing.
    probe_pool_.setMaxThreadCount(1);

    refreshSnapshot();
    refreshPipeline();
}

// Defaulted, but not trivial: destroying probe_pool_ waits for an in-flight
// probe. See the member's declaration for why that wait is required.
DiagnosticsAdapter::~DiagnosticsAdapter() = default;

// ── Reads ───────────────────────────────────────────────────────────────────────

QString DiagnosticsAdapter::verdictState() const {
    return checking_ ? QStringLiteral("checking") : Key(diagnostics::VerdictStateKey(verdict_state_));
}

QString DiagnosticsAdapter::verdictHeadline() const {
    return checking_ ? QStringLiteral("Checking\xe2\x80\xa6") : verdict_headline_;
}

QString DiagnosticsAdapter::verdictSubline() const {
    return checking_ ? QStringLiteral("Check in progress.") : verdict_subline_;
}

int DiagnosticsAdapter::blockerCount() const noexcept {
    return blocker_count_;
}

int DiagnosticsAdapter::noticeCount() const noexcept {
    return notice_count_;
}

const QString& DiagnosticsAdapter::lastCheckText() const noexcept {
    return last_check_text_;
}

bool DiagnosticsAdapter::checking() const noexcept {
    return checking_;
}

bool DiagnosticsAdapter::dataReady() const noexcept {
    return controller_.dataReady();
}

bool DiagnosticsAdapter::expertMode() const noexcept {
    return expert_mode_;
}

void DiagnosticsAdapter::setExpertMode(bool enabled) {
    if (expert_mode_ == enabled)
        return;
    expert_mode_ = enabled;
    emit expertModeChanged(enabled);
}

bool DiagnosticsAdapter::hasLastRecording() const noexcept {
    return controller_.hasLastRecording();
}

bool DiagnosticsAdapter::elevated() const noexcept {
    return controller_.elevated();
}

QAbstractListModel* DiagnosticsAdapter::issues() noexcept {
    return &issue_model_;
}

const QVariantList& DiagnosticsAdapter::tiles() const noexcept {
    return tiles_;
}

const QVariantList& DiagnosticsAdapter::tips() const noexcept {
    return tips_;
}

bool DiagnosticsAdapter::hasIssues() const noexcept {
    return issue_model_.rowCount() > 0;
}

const QVariantList& DiagnosticsAdapter::environmentRows() const noexcept {
    return environment_rows_;
}

const QVariantList& DiagnosticsAdapter::configRows() const noexcept {
    return config_rows_;
}

const QVariantList& DiagnosticsAdapter::selfTestRows() const noexcept {
    return self_test_rows_;
}

const QString& DiagnosticsAdapter::selfTestStatus() const noexcept {
    return self_test_status_;
}

const QVariantList& DiagnosticsAdapter::pipelineStages() const noexcept {
    return pipeline_stages_;
}

bool DiagnosticsAdapter::pipelineLive() const noexcept {
    return pipeline_live_;
}

bool DiagnosticsAdapter::bundleBusy() const noexcept {
    return bundle_busy_;
}

QString DiagnosticsAdapter::defaultBundleFileName() const {
    return QStringLiteral("exosnap-support-%1.zip")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
}

// ── Commands ────────────────────────────────────────────────────────────────────

void DiagnosticsAdapter::ensureChecked() {
    if (probed_ || probe_in_flight_)
        return;
    startProbe(true);
}

void DiagnosticsAdapter::runCheck() {
    if (probe_in_flight_)
        return;
    startProbe(true);
}

void DiagnosticsAdapter::applyFix(const QString& fix_id) {
    if (fix_id.isEmpty())
        return;
    // Label + changes summary come from the card that raised the fix, so the
    // confirm always states what the user is agreeing to.
    QString label = fix_id;
    QString changes;
    for (const auto& card : issue_model_.cards()) {
        if (Text(card.fix_id) == fix_id) {
            label = Text(card.fix_label);
            changes = Text(card.fix_changes_summary);
            break;
        }
    }
    if (changes.isEmpty()) {
        for (const auto& tip : tips_) {
            const QVariantMap map = tip.toMap();
            if (map.value(QStringLiteral("fixId")).toString() == fix_id) {
                label = map.value(QStringLiteral("fixLabel")).toString();
                changes = map.value(QStringLiteral("changes")).toString();
                break;
            }
        }
    }
    emit fixConfirmRequested(fix_id, label, changes);
}

void DiagnosticsAdapter::acceptFix(const QString& fix_id) {
    if (fix_id.isEmpty())
        return;
    emit applyFixAccepted(fix_id);
}

void DiagnosticsAdapter::openAssistedFix(const QString& fix_id) {
    if (fix_id.isEmpty())
        return;
    emit assistedFixRequested(fix_id);
}

void DiagnosticsAdapter::createSupportBundle(const QUrl& destination) {
    const QString path = destination.isLocalFile() ? destination.toLocalFile() : destination.toString();

    SupportBundleContext context;
    context.log_dir = QFileInfo(diagnostics::AppLog::logFilePath()).absolutePath();
    context.launch_session_id = diagnostics::AppLog::sessionId();
    context.created_at = QDateTime::currentDateTime().toString(Qt::ISODate);
    QString settings_text;
    for (const auto& row : controller_.configRows())
        settings_text += QStringLiteral("%1: %2\n").arg(Text(row.label), Text(row.value));
    context.settings_summary = settings_text;

    bundle_service_->createAsync(path, std::move(context), caps_);
}

void DiagnosticsAdapter::openLogs() {
    emit navigateToLogsRequested();
}

void DiagnosticsAdapter::openDevice() {
    emit navigateToDeviceRequested();
}

void DiagnosticsAdapter::openLastReport() {
    if (!controller_.hasLastRecording())
        return;
    emit openLastReportRequested();
}

// ── Host-side input ─────────────────────────────────────────────────────────────

void DiagnosticsAdapter::setDiagnosticConfig(diagnostics::DiagnosticsController::Config config) {
    controller_.SetConfig(std::move(config));
    refreshSnapshot();
    refreshPipeline();
    emit environmentChanged();
    // The config carries a new output folder, so the cached probe facts no longer
    // describe the right volume; re-probe unless one is already running.
    if (!probe_in_flight_)
        startProbe(!probed_);
}

void DiagnosticsAdapter::setCapabilitySet(const capability::CapabilitySet& caps) {
    caps_ = caps;
}

void DiagnosticsAdapter::setSelectedCaptureTarget(std::optional<recorder_core::CaptureTarget> target) {
    controller_.SetSelectedCaptureTarget(std::move(target));
    refreshSnapshot();
}

void DiagnosticsAdapter::setSavedDisplayUnresolved(bool unresolved, const std::string& label) {
    controller_.SetSavedDisplayUnresolved(unresolved, label);
    refreshSnapshot();
}

void DiagnosticsAdapter::setCaptureWindowEvidence(std::optional<diagnostics::WindowTargetFacts> facts,
                                                  const diagnostics::WindowHubEvidence& hub) {
    controller_.SetCaptureWindowEvidence(std::move(facts), hub);
    refreshSnapshot();
}

void DiagnosticsAdapter::setCaptureTargetHdrActive(bool active) {
    controller_.SetCaptureTargetHdrActive(active);
    refreshSnapshot();
}

void DiagnosticsAdapter::setElevated(bool elevated) {
    controller_.SetElevated(elevated);
    emit environmentChanged();
}

void DiagnosticsAdapter::setHasLastRecording(bool has_last_recording) {
    if (controller_.hasLastRecording() == has_last_recording)
        return;
    controller_.SetHasLastRecording(has_last_recording);
    emit hasLastRecordingChanged();
    // Only the Last-session tile changes, but the tile list is a handful of maps —
    // rebuilding it is cheaper than threading a per-tile update through QML.
    refreshSnapshot();
}

void DiagnosticsAdapter::setDpcLatency(diagnostics::DpcLatencyReading reading) {
    controller_.SetDpcLatency(std::move(reading));
}

void DiagnosticsAdapter::setPresentSample(std::optional<diagnostics::PresentSample> sample) {
    controller_.SetPresentSample(std::move(sample));
}

void DiagnosticsAdapter::applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    controller_.SetLiveSnapshot(snapshot);

    if (!controller_.liveRecording()) {
        live_throttle_.Reset();
        refreshPipeline();
        updateLiveProbeTimer();
        return;
    }

    // The snapshot arrives at ~5 Hz. Re-running the recommendation engine and
    // rebuilding the cards that often is wasted work, so both are throttled to 2 Hz —
    // fast enough that live Tier-2 problems still surface WHILE recording.
    if (!live_throttle_.Allow(diagnostics::RefreshThrottle::Clock::now()))
        return;

    refreshPipeline();
    if (controller_.dataReady())
        refreshSnapshot();
    updateLiveProbeTimer();
}

void DiagnosticsAdapter::requestSettingsNavigation() {
    emit navigateToSettingsRequested();
}

void DiagnosticsAdapter::applyProbeResultForTest(diagnostics::DiagnosticsController::ProbeResult probe) {
    applyProbe(std::move(probe), true);
}

diagnostics::DiagnosticsController& DiagnosticsAdapter::controllerForTest() noexcept {
    return controller_;
}

void DiagnosticsAdapter::refreshForTest() {
    refreshSnapshot();
    refreshPipeline();
}

// ── Internals ───────────────────────────────────────────────────────────────────

void DiagnosticsAdapter::startProbe(bool run_self_test) {
    if (probe_in_flight_)
        return;
    probe_in_flight_ = true;
    setChecking(true);
    last_check_text_ = QStringLiteral("Last check: running\xe2\x80\xa6");
    emit lastCheckChanged();

    diagnostics::DiagnosticsProbeRequest request;
    request.output_folder = std::filesystem::path(controller_.outputFolder());
    request.run_self_test = run_self_test;

    // Volume query, output-path write probe and the self-test (DXGI factory +
    // LoadLibraryW + temp file + COM audio enumeration) all run here, off the GUI
    // thread. Results are marshalled back through the application object, so the
    // QPointer check happens on the thread that would destroy this adapter.
    QPointer<DiagnosticsAdapter> guard(this);
    probe_pool_.start([guard, request, run_self_test]() {
        diagnostics::DiagnosticsController::ProbeResult probe = diagnostics::RunDiagnosticsProbe(request);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, probe = std::move(probe), run_self_test]() mutable {
                if (guard)
                    guard->applyProbe(std::move(probe), run_self_test);
            },
            Qt::QueuedConnection);
    });
}

void DiagnosticsAdapter::applyProbe(diagnostics::DiagnosticsController::ProbeResult probe, bool from_manual_check) {
    probe_in_flight_ = false;
    probed_ = true;
    controller_.SetProbeResult(std::move(probe));
    setChecking(false);

    last_check_text_ = QStringLiteral("Last check: %1")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd MMM yyyy, hh:mm")));
    emit lastCheckChanged();

    if (from_manual_check)
        refreshSelfTest();
    refreshSnapshot();
    refreshPipeline();
    updateLiveProbeTimer();
}

void DiagnosticsAdapter::refreshSnapshot() {
    const diagnostics::DiagnosticsSnapshot snapshot = controller_.Evaluate();

    verdict_state_ = snapshot.verdict.state;
    verdict_headline_ = Text(snapshot.verdict.headline);
    verdict_subline_ = Text(snapshot.verdict.subline);
    blocker_count_ = snapshot.verdict.blockers;
    notice_count_ = snapshot.verdict.notices;
    emit verdictChanged();

    tiles_.clear();
    tiles_.reserve(static_cast<qsizetype>(snapshot.tiles.size()));
    for (const auto& tile : snapshot.tiles)
        tiles_.append(TileToMap(tile));
    emit tilesChanged();

    issue_model_.setCards(snapshot.cards);
    tips_.clear();
    tips_.reserve(static_cast<qsizetype>(snapshot.tips.size()));
    for (const auto& tip : snapshot.tips)
        tips_.append(TipToMap(tip));
    emit issuesChanged();

    environment_rows_.clear();
    for (const auto& row : snapshot.environment_rows)
        environment_rows_.append(RowToMap(row));
    config_rows_.clear();
    for (const auto& row : controller_.configRows())
        config_rows_.append(RowToMap(row));
    emit environmentChanged();
}

void DiagnosticsAdapter::refreshPipeline() {
    const std::vector<diagnostics::PipelineStage> stages = controller_.BuildPipelineStages();
    pipeline_live_ = controller_.liveRecording();
    pipeline_stages_.clear();
    pipeline_stages_.reserve(static_cast<qsizetype>(stages.size()));
    for (const auto& stage : stages)
        pipeline_stages_.append(StageToMap(stage));
    emit pipelineChanged();
}

void DiagnosticsAdapter::refreshSelfTest() {
    const diagnostics::SelfTestReport& report = controller_.selfTest();
    self_test_status_ = QStringLiteral("Status: %1").arg(Key(diagnostics::SelfTestStateLabel(report.state)));
    self_test_rows_.clear();
    self_test_rows_.reserve(static_cast<qsizetype>(report.rows.size()));
    for (const auto& row : report.rows)
        self_test_rows_.append(SelfTestRowToMap(row));
    emit selfTestChanged();
}

void DiagnosticsAdapter::setChecking(bool checking) {
    if (checking_ == checking)
        return;
    checking_ = checking;
    emit checkingChanged();
    emit verdictChanged();
}

void DiagnosticsAdapter::updateLiveProbeTimer() {
    const bool should_run = controller_.liveRecording();
    if (should_run && !live_probe_timer_.isActive())
        live_probe_timer_.start();
    else if (!should_run && live_probe_timer_.isActive())
        live_probe_timer_.stop();
}

} // namespace exosnap::quick
