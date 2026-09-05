#include "DiagnosticsAdapter.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/DiagnosticsProbe.h"
#include "diagnostics/FixActionDispatcher.h"
#include "models/CaptureTargetPresentation.h"
#include "viewmodels/RecordViewModel.h"

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

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace exosnap::quick {
namespace {

QString Text(const std::string& value) {
    return QString::fromStdString(value);
}

QString Key(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// The separator the diagnostics surface composes its one-line facts with, in
// the same spelling the policy layer already uses.
constexpr const char* kMiddot = "\xc2\xb7";

// Up to two decimals, without the trailing zeros a fixed precision leaves
// behind: budgets read "16.67 ms" and "8 ms", never "8.00 ms".
QString Trimmed(double value) {
    QString text = QString::number(value, 'f', 2);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0')))
            text.chop(1);
        if (text.endsWith(QLatin1Char('.')))
            text.chop(1);
    }
    return text;
}

QString Measured(double value, const std::string& unit) {
    if (unit.empty())
        return Trimmed(value);
    return Trimmed(value) + QLatin1Char(' ') + QString::fromStdString(unit);
}

QString DurationText(double seconds) {
    if (seconds < 0.0)
        seconds = 0.0;
    if (seconds < 60.0)
        return Trimmed(seconds) + QStringLiteral(" s");
    const int total = static_cast<int>(seconds + 0.5);
    return QStringLiteral("%1 min %2 s").arg(total / 60).arg(total % 60);
}

// Position inside the recording, as the transport and the Edit surface show it.
QString ClipTime(double seconds) {
    const int total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QLatin1Char('0')).arg(total % 60, 2, 10, QLatin1Char('0'));
}

QVariantList ChipsToList(const std::vector<diagnostics::CodecChip>& chips) {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(chips.size()));
    for (const diagnostics::CodecChip& chip : chips) {
        QVariantMap map;
        map.insert(QStringLiteral("text"), QString::fromStdString(chip.label));
        map.insert(QStringLiteral("state"), chip.selected    ? QStringLiteral("selected")
                                            : chip.available ? QStringLiteral("available")
                                                             : QStringLiteral("unavailable"));
        list.append(map);
    }
    return list;
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
    map.insert(QStringLiteral("headBadge"), Text(tile.head_badge));
    map.insert(QStringLiteral("chips"), ChipsToList(tile.chips));
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

// One ledger entry, reduced to the bare fragments the card composes its line
// from. `now_s` is the session clock of the evaluation this row is drawn for.
SessionLedgerRow LedgerRow(const diagnostics::LedgerEntry& entry, double now_s, bool live,
                           const QDateTime& session_start) {
    const auto clock = [&session_start](double offset_s) {
        return session_start.isValid()
                   ? session_start.addMSecs(static_cast<qint64>(offset_s * 1000.0)).toString(QStringLiteral("hh:mm:ss"))
                   : QString();
    };

    SessionLedgerRow row;
    row.entryId = Text(entry.id);
    row.title = Text(entry.title);
    row.summary = Text(entry.summary);
    row.logExcerpt = Text(entry.log_excerpt);
    row.active = entry.active;
    row.count = static_cast<int>(entry.count);
    row.firstSeenText = clock(entry.first_seen_s);
    // While the recording runs, "40 s ago" is the fact the reader can act on;
    // afterwards the session is a closed interval and a time of day places the
    // problem inside it.
    row.lastSeenText =
        live ? DurationText(now_s - entry.last_seen_s) + QStringLiteral(" ago") : clock(entry.last_seen_s);
    row.worstText = entry.worst.has_value() ? Measured(*entry.worst, entry.unit) : Text(entry.worst_text);
    row.budgetText = entry.budget.has_value() ? Measured(*entry.budget, entry.unit) : QStringLiteral("no budget");
    // An open occurrence has no length yet, so a live entry is timed from the
    // start of the stretch it is still in rather than from a total that excludes it.
    double active_s = entry.total_active_s;
    if (entry.active && !entry.occurrences.empty())
        active_s = now_s - entry.occurrences.back().start_s;
    row.totalActiveText = DurationText(active_s);

    for (const diagnostics::LedgerOccurrence& occurrence : entry.occurrences) {
        QVariantMap map;
        map.insert(QStringLiteral("startMs"), static_cast<int>(occurrence.start_s * 1000.0));
        map.insert(QStringLiteral("endMs"), static_cast<int>(occurrence.end_s * 1000.0));
        map.insert(QStringLiteral("worstText"), Measured(occurrence.worst, entry.unit));
        map.insert(QStringLiteral("text"), ClipTime(occurrence.start_s));
        row.occurrences.append(map);
    }
    return row;
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

// Sparkline depth: the last 60 snapshots, which is 12 s at the 5 Hz diagnostics
// cadence. Long enough to show a trend, short enough that it is still "now".
constexpr std::size_t kSeriesLength = 60;

} // namespace

DiagnosticsAdapter::DiagnosticsAdapter(QObject* parent)
    : QObject(parent), bundle_service_(std::make_unique<SupportBundleService>()) {
    controller_.SetDisplayFacts(PrimaryDisplayFacts());

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

QString DiagnosticsAdapter::lastCheckText() const {
    // While recording the band reports the session, so its stamp says since when
    // and at what rate rather than when the readiness probe last ran.
    if (recording_) {
        return session_start_.isValid() ? QStringLiteral("Recording since %1 \xc2\xb7 live 5x/s")
                                              .arg(session_start_.toString(QStringLiteral("hh:mm")))
                                        : QStringLiteral("Recording \xc2\xb7 live 5x/s");
    }
    if (probe_in_flight_)
        return QStringLiteral("Checking\xe2\x80\xa6");
    if (!last_check_at_.isValid())
        return QStringLiteral("Not checked yet");
    // The page has no Run check button any more, so the stamp is where the
    // recheck policy is stated: nobody has to ask whether this is stale.
    return QStringLiteral("Checked %1 \xc2\xb7 rechecks every 10 s and on every settings change")
        .arg(last_check_at_.toString(QStringLiteral("hh:mm")));
}

bool DiagnosticsAdapter::checking() const noexcept {
    return checking_;
}

bool DiagnosticsAdapter::dataReady() const noexcept {
    return controller_.dataReady();
}

bool DiagnosticsAdapter::hasLastRecording() const noexcept {
    return controller_.hasLastRecording();
}

bool DiagnosticsAdapter::elevated() const noexcept {
    return controller_.elevated();
}

bool DiagnosticsAdapter::recording() const noexcept {
    return recording_;
}

QAbstractListModel* DiagnosticsAdapter::issues() noexcept {
    return &issue_model_;
}

QAbstractListModel* DiagnosticsAdapter::ledger() noexcept {
    return &ledger_model_;
}

int DiagnosticsAdapter::ledgerCount() const noexcept {
    return ledger_model_.rowCount();
}

const QVariantMap& DiagnosticsAdapter::lastSession() const noexcept {
    return last_session_;
}

bool DiagnosticsAdapter::hasLastSession() const noexcept {
    return controller_.lastSession().valid;
}

bool DiagnosticsAdapter::inDepthEnabled() const noexcept {
    return in_depth_enabled_;
}

void DiagnosticsAdapter::setInDepthEnabledFromUi(bool enabled) {
    if (in_depth_enabled_ == enabled || !inDepthAvailable())
        return;
    emit inDepthToggled(enabled);
}

QString DiagnosticsAdapter::inDepthStateText() const {
    // The opt-in persists across launches; elevation does not. With the setting on
    // in a standard process no ETW session exists and no in-depth tile has a
    // reading, so the sub-text names the gate rather than claiming the two traces
    // that are not running.
    if (in_depth_enabled_) {
        return controller_.elevated() ? QStringLiteral("On \xc2\xb7 elevated \xc2\xb7 PresentMon + DPC/ISR trace")
                                      : QStringLiteral("On \xc2\xb7 not measuring \xc2\xb7 needs an admin relaunch");
    }
    if (recording_)
        return QStringLiteral("Off \xc2\xb7 cannot change while recording");
    return QStringLiteral("Off \xc2\xb7 needs an admin relaunch");
}

bool DiagnosticsAdapter::inDepthAvailable() const noexcept {
    return !recording_;
}

const QVariantList& DiagnosticsAdapter::tiles() const noexcept {
    return tiles_;
}

const QVariantList& DiagnosticsAdapter::liveTiles() const noexcept {
    return live_tiles_;
}

void DiagnosticsAdapter::refreshLiveTiles() {
    const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot = controller_.liveSnapshot();
    diagnostics::LiveTileInputs inputs{snapshot, controller_.ledger()};
    // Elevation is half the gate: with the opt-in on but the process standard,
    // no trace is running and there is nothing for the extra tiles to report.
    inputs.in_depth = in_depth_enabled_ && controller_.elevated();
    inputs.present = present_sample_;
    inputs.dpc = dpc_reading_;
    inputs.gpu_exec_p99_ms = snapshot.compositor.gpu_exec_p99_ms;

    std::vector<diagnostics::LiveTile> next = diagnostics::BuildLiveTiles(inputs);
    // The tiles can be identical while the sparklines have moved on, so the
    // series revision is part of "did anything change" -- otherwise the trend
    // freezes on a run where the rounded headline happens to hold still.
    if (next == live_tile_values_ && series_revision_ == published_series_revision_)
        return;

    live_tile_values_ = std::move(next);
    published_series_revision_ = series_revision_;
    live_tiles_.clear();
    live_tiles_.reserve(static_cast<qsizetype>(live_tile_values_.size()));
    for (const diagnostics::LiveTile& tile : live_tile_values_) {
        const QVariantList series = seriesFor(tile.key);
        QString session_detail = QString::fromStdString(tile.session_detail);
        if (tile.key == "encoder" && encoder_session_worst_p99_ms_ > 0.0) {
            session_detail =
                QStringLiteral("session worst p99 ") + Trimmed(encoder_session_worst_p99_ms_) + QStringLiteral(" ms");
        }
        // With a trend on screen the headline is "now" and the sparkline is the
        // last twelve seconds, so the third line is the whole run. Without one
        // there is nothing for a session figure to sit under, and the detail
        // keeps saying why a measurement is missing.
        QString detail = QString::fromStdString(tile.detail);
        if (!series.isEmpty() && !session_detail.isEmpty())
            detail = session_detail;

        QVariantMap entry;
        entry.insert(QStringLiteral("key"), QString::fromStdString(tile.key));
        entry.insert(QStringLiteral("title"), QString::fromStdString(tile.title));
        entry.insert(QStringLiteral("value"), QString::fromStdString(tile.value));
        entry.insert(QStringLiteral("sub"), QString::fromStdString(tile.sub));
        entry.insert(QStringLiteral("subTinted"), QString::fromStdString(tile.sub_tinted));
        entry.insert(QStringLiteral("detail"), detail);
        entry.insert(QStringLiteral("sessionDetail"), session_detail);
        entry.insert(QStringLiteral("tone"),
                     QString::fromUtf8(diagnostics::TileToneKey(tile.tone).data(),
                                       static_cast<qsizetype>(diagnostics::TileToneKey(tile.tone).size())));
        entry.insert(QStringLiteral("valueTone"), Key(diagnostics::ValueToneKey(tile.value_tone)));
        entry.insert(QStringLiteral("subTone"), Key(diagnostics::ValueToneKey(tile.sub_tone)));
        entry.insert(QStringLiteral("series"), series);
        // NaN is the sparkline's own "no budget line", so an absent budget is
        // passed through as one rather than as a zero it would try to draw.
        entry.insert(QStringLiteral("budget"), tile.budget.value_or(std::numeric_limits<double>::quiet_NaN()));
        live_tiles_.append(entry);
    }
    emit liveTilesChanged();
}

void DiagnosticsAdapter::appendSeriesSamples(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    const auto push = [this](const char* key, double value) {
        std::deque<double>& samples = series_[key];
        samples.push_back(value);
        while (samples.size() > kSeriesLength)
            samples.pop_front();
    };

    push("framePacing", snapshot.capture.actual_fps);
    if (snapshot.video_encoder.frames_encoded > 0) {
        push("encoder", snapshot.video_encoder.p99_ms);
        encoder_session_worst_p99_ms_ = std::max(encoder_session_worst_p99_ms_, snapshot.video_encoder.p99_ms);
    }
    // Only a measured drift is plotted: a zero here would draw perfect sync on a
    // recording whose drift nobody measured.
    if (snapshot.av_drift_availability == exosnap::engine::MetricAvailability::Available)
        push("audioSync", snapshot.av_drift_ms);
    push("storage", snapshot.disk.throughput_mib_s);
    ++series_revision_;
}

void DiagnosticsAdapter::resetSeries() {
    series_.clear();
    ++series_revision_;
}

QVariantList DiagnosticsAdapter::seriesFor(const std::string& key) const {
    const auto it = series_.find(key);
    if (it == series_.end())
        return {};
    QVariantList values;
    values.reserve(static_cast<qsizetype>(it->second.size()));
    for (const double sample : it->second)
        values.append(sample);
    return values;
}

std::optional<diagnostics::DpcLatencyReading> DiagnosticsAdapter::readDpcLatency() const {
    if (dpc_provider_ == nullptr)
        return std::nullopt;
    const diagnostics::DpcLatencyReading reading = dpc_provider_->Read();
    return reading.available ? std::optional<diagnostics::DpcLatencyReading>(reading) : std::nullopt;
}

void DiagnosticsAdapter::refreshLedger() {
    const double now_s = controller_.liveSnapshot().elapsed_seconds;
    const bool live = controller_.liveRecording();

    std::vector<SessionLedgerRow> rows;
    rows.reserve(controller_.ledger().entries().size());
    for (const diagnostics::LedgerEntry& entry : controller_.ledger().entries())
        rows.push_back(LedgerRow(entry, now_s, live, session_start_));

    const int previous_rows = ledger_model_.rowCount();
    ledger_model_.setRows(std::move(rows));
    if (previous_rows != ledger_model_.rowCount() || live)
        emit ledgerChanged();
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

QAbstractListModel* DiagnosticsAdapter::pipelineStages() noexcept {
    return &pipeline_stage_model_;
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

void DiagnosticsAdapter::showInLog(const QString& entry_id) {
    if (entry_id.isEmpty())
        return;
    emit showInLogRequested(entry_id);
}

void DiagnosticsAdapter::openEditAt(qint64 position_ms) {
    emit openEditAtRequested(position_ms < 0 ? 0 : position_ms);
}

void DiagnosticsAdapter::openLastSessionFolder() {
    if (!controller_.lastSession().valid)
        return;
    emit openLastSessionFolderRequested();
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

void DiagnosticsAdapter::setSelectedCaptureTarget(std::optional<exosnap::engine::CaptureTarget> target) {
    // Naming the target is presentation, and this is where the product's own
    // label lives; the controller would otherwise report a device path.
    std::string label;
    if (target.has_value()) {
        label = ResolveCaptureTargetPresentation(*target, target->kind == exosnap::engine::CaptureTarget::Kind::Window
                                                              ? CaptureTargetPresentationKind::Window
                                                              : CaptureTargetPresentationKind::Display)
                    .label;
    }
    controller_.SetSelectedCaptureTarget(std::move(target), std::move(label));
    refreshSnapshot();
}

void DiagnosticsAdapter::setCaptureTargetAdapter(diagnostics::RecommendationEngine::CaptureTargetAdapterFacts facts) {
    controller_.SetCaptureTargetAdapter(std::move(facts));
}

void DiagnosticsAdapter::setPresentAttributionPid(unsigned long pid) {
    controller_.SetPresentAttributionPid(pid);
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

void DiagnosticsAdapter::refreshDisplayFacts() {
    controller_.SetDisplayFacts(PrimaryDisplayFacts());
    refreshSnapshot();
}

void DiagnosticsAdapter::setElevated(bool elevated) {
    controller_.SetElevated(elevated);
    emit environmentChanged();
    emit inDepthChanged();
}

void DiagnosticsAdapter::setInDepthEnabled(bool enabled) {
    if (in_depth_enabled_ == enabled)
        return;
    in_depth_enabled_ = enabled;
    emit inDepthChanged();
    refreshLiveTiles();
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

void DiagnosticsAdapter::setDpcLatencyProvider(diagnostics::IDpcLatencyProvider* provider) {
    dpc_provider_ = provider;
    refreshSnapshot();
}

void DiagnosticsAdapter::setPresentSample(std::optional<diagnostics::PresentSample> sample) {
    present_sample_ = sample;
    controller_.SetPresentSample(std::move(sample));
}

std::vector<diagnostics::LedgerEntry> DiagnosticsAdapter::frozenLedger() const {
    return controller_.frozenLedger();
}

void DiagnosticsAdapter::setLastSession(const exosnap::UiRecordingResult& result) {
    diagnostics::LastSession session =
        diagnostics::BuildLastSession(result, controller_.liveSnapshot(), controller_.frozenLedger());
    // The controller has no clock; the wall times come from the session start
    // this adapter pinned when the recording's first snapshot arrived.
    if (session_start_.isValid()) {
        session.started_at_text = session_start_.toString(QStringLiteral("hh:mm")).toStdString();
        session.ended_at_text = session_start_.addMSecs(static_cast<qint64>(session.duration_s * 1000.0))
                                    .toString(QStringLiteral("hh:mm"))
                                    .toStdString();
    }
    controller_.SetLastSession(std::move(session));
    refreshLastSessionMap();
    emit lastSessionChanged();
}

void DiagnosticsAdapter::applyLiveDiagnostics(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot) {
    controller_.SetLiveSnapshot(snapshot);
    const bool live = controller_.liveRecording();

    if (live) {
        if (snapshot.session_generation != series_generation_) {
            series_generation_ = snapshot.session_generation;
            resetSeries();
            encoder_session_worst_p99_ms_ = 0.0;
            // Second zero of this session, so a ledger entry can be stamped with
            // a time of day rather than an offset only this page understands.
            session_start_ =
                QDateTime::currentDateTime().addMSecs(-static_cast<qint64>(snapshot.elapsed_seconds * 1000.0));
        }
        appendSeriesSamples(snapshot);
    }

    const bool left_recording = recording_ && !live;
    if (recording_ != live) {
        recording_ = live;
        emit recordingChanged();
        emit inDepthChanged();
        // The band's stamp reports the session while one runs and the readiness
        // recheck policy otherwise, so it changes meaning on this edge.
        emit lastCheckChanged();
    }

    if (!live) {
        live_throttle_.Reset();
        refreshPipeline();
        // Not throttled on this edge, and a full snapshot rather than only the
        // ledger: the verdict band reports the SESSION while one runs, and
        // nothing else recomputes it from ComputeVerdict() afterwards. Without
        // this the band keeps saying "Recording" above the Last session card
        // until the user changes a setting.
        refreshSnapshot();
        // The tiles must go away on this very edge rather than at the next
        // allowed tick, or the page keeps showing a live summary of a recording
        // that has stopped.
        refreshLiveTiles();
        updateLiveProbeTimer();
        // The controller froze the ledger on this same edge, so the closed copy
        // can go to whoever writes the session report. Pushed rather than pulled:
        // the report is written on the recording thread, which must not reach
        // into state this thread owns.
        if (left_recording)
            emit sessionLedgerFrozen();
        return;
    }

    // The snapshot arrives at ~5 Hz. Re-running the recommendation engine and
    // rebuilding the cards that often is wasted work, so both are throttled to 2 Hz —
    // fast enough that live Tier-2 problems still surface WHILE recording.
    if (!live_throttle_.Allow(diagnostics::RefreshThrottle::Clock::now()))
        return;

    refreshPipeline();
    refreshLiveTiles();
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

const diagnostics::DiagnosticsController& DiagnosticsAdapter::controller() const noexcept {
    return controller_;
}

diagnostics::DiagnosticsController& DiagnosticsAdapter::controllerForTest() noexcept {
    return controller_;
}

void DiagnosticsAdapter::refreshForTest() {
    refreshSnapshot();
    refreshPipeline();
    refreshLiveTiles();
}

// ── Internals ───────────────────────────────────────────────────────────────────

void DiagnosticsAdapter::startProbe(bool run_self_test) {
    if (probe_in_flight_)
        return;
    probe_in_flight_ = true;
    setChecking(true);
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

    last_check_at_ = QDateTime::currentDateTime();
    emit lastCheckChanged();

    if (from_manual_check)
        refreshSelfTest();
    refreshSnapshot();
    refreshPipeline();
    updateLiveProbeTimer();
}

void DiagnosticsAdapter::refreshSnapshot() {
    // ADR 0033. Read the DPC/ISR producer HERE, where the recommendation engine is about
    // to run, and hand the controller nothing at all unless the kernel trace is actually
    // measuring. An unavailable reading is not a zero one: with no measurement there is
    // no recommendation to make, and the Diagnostics page says nothing about DPC rather
    // than reporting a peak nobody is updating any more. Read() takes a short lock and
    // never blocks on the kernel, so this stays a GUI-thread call.
    dpc_reading_ = readDpcLatency();
    controller_.SetDpcLatency(dpc_reading_);

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

    // While recording, a Tier-2 measured problem is reported by the session
    // ledger, which says when it fired and how often. Repeating it as a card
    // above would show the same finding twice with less of the story.
    std::vector<diagnostics::IssueCard> cards = snapshot.cards;
    if (controller_.liveRecording()) {
        std::unordered_set<std::string> measured;
        for (const diagnostics::DiagnosticResult& result : controller_.lastChecklist().results) {
            if (result.tier == diagnostics::DiagnosticTier::MeasuredProblem)
                measured.insert(result.id);
        }
        std::erase_if(cards, [&measured](const diagnostics::IssueCard& card) {
            return !card.id.empty() && measured.count(card.id) > 0;
        });
    }
    issue_model_.setCards(std::move(cards));
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

    // The ledger is observed inside Evaluate(), so the model follows the pass
    // that produced it rather than a second, differently timed read.
    refreshLedger();
}

void DiagnosticsAdapter::refreshPipeline() {
    const bool live = controller_.liveRecording();
    const bool live_changed = pipeline_live_ != live;
    pipeline_live_ = live;
    // The model decides for itself whether anything moved; `pipelineChanged` now
    // carries only `pipelineLive`, so it is emitted only when that boolean does.
    pipeline_stage_model_.setStages(controller_.BuildPipelineStages());
    if (live_changed)
        emit pipelineChanged();
}

void DiagnosticsAdapter::refreshLastSessionMap() {
    const diagnostics::LastSession& session = controller_.lastSession();
    last_session_.clear();
    if (!session.valid)
        return;

    const QString saved = QStringLiteral("Recording saved");
    last_session_.insert(QStringLiteral("headerText"),
                         session.problems > 0 ? QStringLiteral("%1 %2 %3 problem%4 observed")
                                                    .arg(saved, QString::fromUtf8(kMiddot))
                                                    .arg(session.problems)
                                                    .arg(session.problems == 1 ? QString() : QStringLiteral("s"))
                                              : saved);
    last_session_.insert(QStringLiteral("fileName"), Text(session.file_name));
    last_session_.insert(QStringLiteral("durationMs"), static_cast<int>(session.duration_s * 1000.0));
    last_session_.insert(QStringLiteral("mediaDurationMs"), static_cast<int>(session.media_duration_s * 1000.0));
    last_session_.insert(QStringLiteral("startedAtText"), Text(session.started_at_text));
    last_session_.insert(QStringLiteral("endedAtText"), Text(session.ended_at_text));
    last_session_.insert(QStringLiteral("durationText"), DurationText(session.duration_s));
    last_session_.insert(QStringLiteral("problems"), session.problems);

    QVariantList facts;
    for (const diagnostics::LastSessionFact& fact : session.facts) {
        QVariantMap map;
        map.insert(QStringLiteral("key"), Text(fact.key));
        map.insert(QStringLiteral("label"), Text(fact.label));
        map.insert(QStringLiteral("value"), Text(fact.value));
        map.insert(QStringLiteral("sub"), Text(fact.sub));
        map.insert(QStringLiteral("valueTone"), Key(diagnostics::ValueToneKey(fact.tone)));
        facts.append(map);
    }
    last_session_.insert(QStringLiteral("facts"), facts);

    // Units live on the ledger entry, not on the mark, so the mark's worst value
    // is spelled with the unit of the check that produced it.
    const auto unit_of = [&session](const std::string& id) {
        for (const diagnostics::LedgerEntry& entry : session.ledger) {
            if (entry.id == id)
                return entry.unit;
        }
        return std::string{};
    };

    QVariantList marks;
    for (const diagnostics::TimelineMark& mark : session.marks) {
        QVariantMap map;
        map.insert(QStringLiteral("startMs"), static_cast<int>(mark.start_s * 1000.0));
        map.insert(QStringLiteral("endMs"), static_cast<int>(mark.end_s * 1000.0));
        map.insert(QStringLiteral("durationMs"), static_cast<int>((mark.end_s - mark.start_s) * 1000.0));
        map.insert(QStringLiteral("id"), Text(mark.id));
        map.insert(QStringLiteral("title"), Text(mark.title));
        map.insert(QStringLiteral("worstText"), Measured(mark.worst, unit_of(mark.id)));
        map.insert(QStringLiteral("tone"), Text(mark.tone));
        marks.append(map);
    }
    last_session_.insert(QStringLiteral("marks"), marks);

    // Worst first (rule 3): the card opens the entry with the largest overshoot
    // and leaves the rest as rows. An entry without a measured value sorts last.
    std::vector<const diagnostics::LedgerEntry*> ordered;
    ordered.reserve(session.ledger.size());
    for (const diagnostics::LedgerEntry& entry : session.ledger)
        ordered.push_back(&entry);
    // The key is the OVERSHOOT, which only an entry with a budget has. A count in
    // its own unit ("12 mode flips") is not comparable to a ratio, so entries
    // without a budget rank after every entry with one instead of winning on the
    // magnitude of a number nothing was measured against.
    const auto overshoot = [](const diagnostics::LedgerEntry* entry) {
        return entry->budget.value_or(0.0) > 0.0 && entry->worst.has_value()
                   ? std::optional<double>(*entry->worst / *entry->budget)
                   : std::nullopt;
    };
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&overshoot](const diagnostics::LedgerEntry* a, const diagnostics::LedgerEntry* b) {
                         const std::optional<double> left = overshoot(a);
                         const std::optional<double> right = overshoot(b);
                         if (left.has_value() != right.has_value())
                             return left.has_value();
                         return left.has_value() && *left > *right;
                     });

    QVariantList entries;
    for (const diagnostics::LedgerEntry* entry : ordered) {
        const SessionLedgerRow row = LedgerRow(*entry, session.duration_s, /*live=*/false, session_start_);
        QVariantMap map;
        map.insert(QStringLiteral("entryId"), row.entryId);
        map.insert(QStringLiteral("title"), row.title);
        map.insert(QStringLiteral("summary"), row.summary);
        map.insert(QStringLiteral("active"), row.active);
        map.insert(QStringLiteral("count"), row.count);
        map.insert(QStringLiteral("firstSeenText"), row.firstSeenText);
        map.insert(QStringLiteral("lastSeenText"), row.lastSeenText);
        map.insert(QStringLiteral("worstText"), row.worstText);
        map.insert(QStringLiteral("budgetText"), row.budgetText);
        map.insert(QStringLiteral("totalActiveText"), row.totalActiveText);
        map.insert(QStringLiteral("logExcerpt"), row.logExcerpt);
        map.insert(QStringLiteral("occurrences"), row.occurrences);
        entries.append(map);
    }
    last_session_.insert(QStringLiteral("ledgerEntries"), entries);
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
