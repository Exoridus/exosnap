#pragma once

#include "DiagnosticIssueModel.h"
#include "PipelineStageModel.h"
#include "SessionLedgerModel.h"

#include "diagnostics/DiagnosticsController.h"
#include "diagnostics/DpcLatencyProvider.h"
#include "services/SupportBundleService.h"

#include <capability/capability_set.h>

#include <QAbstractListModel>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace exosnap::quick {

// Narrow QML boundary for the Diagnostics area.
//
// All policy — the verdict rail, the tier→card/tip split, the readiness tiles, the
// self-test rollup, the pipeline cards and their frame-drop delta — lives in
// diagnostics::DiagnosticsController, which knows nothing about Qt Quick. This
// adapter owns one controller, converts its plain structs into QML-shaped values,
// and keeps every blocking probe off the GUI thread.
//
// Probe lifecycle (PERF): construction does NO I/O. The first probe starts on
// ensureChecked(), which the page calls the first time it becomes visible, and runs
// on a worker thread. `checking` is true for its duration so the surface can say so
// instead of showing a stale verdict as if it were fresh.
//
// The adapter deliberately does NOT own settings. Applying a fix, navigating and
// retargeting capture are emitted as intents; the composition root performs them.
class DiagnosticsAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DiagnosticsAdapter is provided by the application")

    Q_PROPERTY(QString verdictState READ verdictState NOTIFY verdictChanged FINAL)
    Q_PROPERTY(QString verdictHeadline READ verdictHeadline NOTIFY verdictChanged FINAL)
    Q_PROPERTY(QString verdictSubline READ verdictSubline NOTIFY verdictChanged FINAL)
    Q_PROPERTY(int blockerCount READ blockerCount NOTIFY verdictChanged FINAL)
    Q_PROPERTY(int noticeCount READ noticeCount NOTIFY verdictChanged FINAL)
    Q_PROPERTY(QString lastCheckText READ lastCheckText NOTIFY lastCheckChanged FINAL)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged FINAL)
    Q_PROPERTY(bool dataReady READ dataReady NOTIFY verdictChanged FINAL)

    Q_PROPERTY(bool hasLastRecording READ hasLastRecording NOTIFY hasLastRecordingChanged FINAL)
    Q_PROPERTY(bool elevated READ elevated NOTIFY environmentChanged FINAL)
    // True while the last live snapshot is in the recording or paused lifecycle.
    // The page is ordered by this: readiness answers "may I start", which stops
    // being the question the moment something is running.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged FINAL)

    // Declared as the Qt base type: qmltyperegistrar records the concrete subclass
    // under its namespaced C++ name while moc writes the property type unqualified,
    // so a concrete spelling here is unresolvable for qmllint.
    Q_PROPERTY(QAbstractListModel* issues READ issues CONSTANT FINAL)
    // The problems measured during this recording session, in first-seen order.
    // Same base-type spelling, same reason.
    Q_PROPERTY(QAbstractListModel* ledger READ ledger CONSTANT FINAL)
    Q_PROPERTY(int ledgerCount READ ledgerCount NOTIFY ledgerChanged FINAL)

    Q_PROPERTY(QVariantList tiles READ tiles NOTIFY tilesChanged FINAL)
    // The five live tiles, and whether there is a live pipeline to show them
    // for. Separate from `tiles`: those are pre-flight readiness facts and stay
    // meaningful while idle, these exist only while something is recording.
    Q_PROPERTY(QVariantList liveTiles READ liveTiles NOTIFY liveTilesChanged FINAL)
    Q_PROPERTY(QVariantList tips READ tips NOTIFY issuesChanged FINAL)
    Q_PROPERTY(bool hasIssues READ hasIssues NOTIFY issuesChanged FINAL)
    Q_PROPERTY(QVariantList environmentRows READ environmentRows NOTIFY environmentChanged FINAL)
    Q_PROPERTY(QVariantList configRows READ configRows NOTIFY environmentChanged FINAL)
    Q_PROPERTY(QVariantList selfTestRows READ selfTestRows NOTIFY selfTestChanged FINAL)
    Q_PROPERTY(QString selfTestStatus READ selfTestStatus NOTIFY selfTestChanged FINAL)
    // Same reason as `issues` above for the base-type spelling.
    Q_PROPERTY(QAbstractListModel* pipelineStages READ pipelineStages CONSTANT FINAL)
    Q_PROPERTY(bool pipelineLive READ pipelineLive NOTIFY pipelineChanged FINAL)

    // The finished recording, shaped for the Last session card. Empty until a
    // result has arrived; kept until the next recording starts.
    Q_PROPERTY(QVariantMap lastSession READ lastSession NOTIFY lastSessionChanged FINAL)
    Q_PROPERTY(bool hasLastSession READ hasLastSession NOTIFY lastSessionChanged FINAL)

    // The in-depth diagnostics switch. One product setting (the present/DPC
    // opt-in) behind two controls; the adapter never writes it, it reports the
    // user's intent as inDepthToggled and the composition root owns the setting.
    Q_PROPERTY(bool inDepthEnabled READ inDepthEnabled WRITE setInDepthEnabledFromUi NOTIFY inDepthChanged FINAL)
    Q_PROPERTY(QString inDepthStateText READ inDepthStateText NOTIFY inDepthChanged FINAL)
    Q_PROPERTY(bool inDepthAvailable READ inDepthAvailable NOTIFY inDepthChanged FINAL)

    Q_PROPERTY(bool bundleBusy READ bundleBusy NOTIFY bundleBusyChanged FINAL)
    Q_PROPERTY(QString defaultBundleFileName READ defaultBundleFileName NOTIFY lastCheckChanged FINAL)

  public:
    explicit DiagnosticsAdapter(QObject* parent = nullptr);
    ~DiagnosticsAdapter() override;

    [[nodiscard]] QString verdictState() const;
    [[nodiscard]] QString verdictHeadline() const;
    [[nodiscard]] QString verdictSubline() const;
    [[nodiscard]] int blockerCount() const noexcept;
    [[nodiscard]] int noticeCount() const noexcept;
    [[nodiscard]] QString lastCheckText() const;
    [[nodiscard]] bool checking() const noexcept;
    [[nodiscard]] bool dataReady() const noexcept;
    [[nodiscard]] bool hasLastRecording() const noexcept;
    [[nodiscard]] bool elevated() const noexcept;
    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] QAbstractListModel* issues() noexcept;
    [[nodiscard]] QAbstractListModel* ledger() noexcept;
    [[nodiscard]] int ledgerCount() const noexcept;
    [[nodiscard]] const QVariantList& tiles() const noexcept;
    [[nodiscard]] const QVariantList& liveTiles() const noexcept;
    [[nodiscard]] const QVariantList& tips() const noexcept;
    [[nodiscard]] bool hasIssues() const noexcept;
    [[nodiscard]] const QVariantList& environmentRows() const noexcept;
    [[nodiscard]] const QVariantList& configRows() const noexcept;
    [[nodiscard]] const QVariantList& selfTestRows() const noexcept;
    [[nodiscard]] const QString& selfTestStatus() const noexcept;
    [[nodiscard]] QAbstractListModel* pipelineStages() noexcept;
    [[nodiscard]] bool pipelineLive() const noexcept;
    [[nodiscard]] const QVariantMap& lastSession() const noexcept;
    [[nodiscard]] bool hasLastSession() const noexcept;
    [[nodiscard]] bool inDepthEnabled() const noexcept;
    // The QML write path. Changes nothing here: the setting is owned by the
    // composition root, which pushes the result back through setInDepthEnabled().
    void setInDepthEnabledFromUi(bool enabled);
    [[nodiscard]] QString inDepthStateText() const;
    [[nodiscard]] bool inDepthAvailable() const noexcept;
    [[nodiscard]] bool bundleBusy() const noexcept;
    [[nodiscard]] QString defaultBundleFileName() const;

    // Starts the first probe if none has run and none is in flight.
    Q_INVOKABLE void ensureChecked();
    // Explicit "Run Check". No-op while a probe is in flight.
    Q_INVOKABLE void runCheck();
    // Auto fix: never applies directly. Emits fixConfirmRequested so the frontend can
    // show the mandatory confirm; acceptFix() is what actually applies.
    Q_INVOKABLE void applyFix(const QString& fix_id);
    Q_INVOKABLE void acceptFix(const QString& fix_id);
    Q_INVOKABLE void openAssistedFix(const QString& fix_id);
    Q_INVOKABLE void createSupportBundle(const QUrl& destination);
    Q_INVOKABLE void openLogs();
    // The Logs page, filtered to one diagnostic id.
    Q_INVOKABLE void showInLog(const QString& entry_id);
    // The Edit surface on the last recording, positioned at `position_ms`.
    Q_INVOKABLE void openEditAt(qint64 position_ms);
    Q_INVOKABLE void openLastSessionFolder();

    // ── Host-side input (C++ only) ─────────────────────────────────────────────
    void setDiagnosticConfig(diagnostics::DiagnosticsController::Config config);
    void setCapabilitySet(const capability::CapabilitySet& caps);
    void setSelectedCaptureTarget(std::optional<exosnap::engine::CaptureTarget> target);
    void setCaptureTargetAdapter(diagnostics::RecommendationEngine::CaptureTargetAdapterFacts facts);
    void setPresentAttributionPid(unsigned long pid);
    void setSavedDisplayUnresolved(bool unresolved, const std::string& label);
    void setCaptureWindowEvidence(std::optional<diagnostics::WindowTargetFacts> facts,
                                  const diagnostics::WindowHubEvidence& hub);
    void setCaptureTargetHdrActive(bool active);
    // Re-reads the primary screen's size and compositor rate. The refresh-rate
    // mismatch check compares the configured fps against this number, so a
    // mode-set that is never picked up leaves the page recommending against a
    // rate the display now supports.
    void refreshDisplayFacts();
    void setElevated(bool elevated);
    void setHasLastRecording(bool has_last_recording);
    // ADR 0033 DPC/ISR latency. Borrowed, never owned, and PULLED on every evaluation
    // rather than pushed: the reading is only ever as current as the last read, so
    // sampling where the recommendation engine runs is what keeps a peak that stopped
    // being measured from standing on the page. nullptr means no producer is installed,
    // which reports exactly as an unavailable one does -- nothing.
    void setDpcLatencyProvider(diagnostics::IDpcLatencyProvider* provider);
    void setPresentSample(std::optional<diagnostics::PresentSample> sample);
    // The present/DPC opt-in as the settings store holds it. Does not emit
    // inDepthToggled -- that signal is the UI asking for a change, this is the
    // answer arriving.
    void setInDepthEnabled(bool enabled);
    void applyLiveDiagnostics(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot);
    // The frozen ledger of the recording that just ended, for the session report.
    [[nodiscard]] std::vector<diagnostics::LedgerEntry> frozenLedger() const;
    // Builds and publishes the Last session card from a finished recording. The
    // ledger has already been frozen by the terminal live snapshot, which the
    // coordinator delivers before the result.
    void setLastSession(const exosnap::UiRecordingResult& result);
    // Emitted by the composition root once an assisted fix has resolved to a
    // Settings section, so the page can route the navigation.
    void requestSettingsNavigation();

    // Read-only view of the policy owner, for the observability surface. The
    // adapter converts the controller's structs into QML-shaped values; the
    // structured control-channel payload reads the SAME controller instead of a
    // re-conversion of the QML shapes, so a tier or a fix action cannot be lost
    // in translation on the way out.
    [[nodiscard]] const diagnostics::DiagnosticsController& controller() const noexcept;

    // Test seam: applies a probe result without touching the filesystem.
    void applyProbeResultForTest(diagnostics::DiagnosticsController::ProbeResult probe);
    [[nodiscard]] diagnostics::DiagnosticsController& controllerForTest() noexcept;
    void refreshForTest();

  signals:
    void verdictChanged();
    void lastCheckChanged();
    void checkingChanged();
    void hasLastRecordingChanged();
    void recordingChanged();
    void ledgerChanged();
    // The recording ended and the ledger is closed. Whoever writes the session
    // report reads frozenLedger() from THIS signal and keeps its own copy: the
    // ledger lives on this thread and is gathered for the report elsewhere.
    void sessionLedgerFrozen();
    void lastSessionChanged();
    void inDepthChanged();
    // The user moved the in-depth switch. The composition root owns the setting
    // and the elevation gate behind it.
    void inDepthToggled(bool enabled);
    void tilesChanged();
    void liveTilesChanged();
    void issuesChanged();
    void environmentChanged();
    void selfTestChanged();
    void pipelineChanged();
    void bundleBusyChanged();

    // The mandatory confirm before an Auto fix is applied.
    void fixConfirmRequested(const QString& fixId, const QString& label, const QString& changesSummary);
    // Accepted Auto fix: the composition root owns the settings and performs it.
    void applyFixAccepted(const QString& fixId);
    void assistedFixRequested(const QString& fixId);
    void navigateToLogsRequested();
    void navigateToSettingsRequested();
    void showInLogRequested(const QString& entryId);
    void openEditAtRequested(qint64 positionMs);
    void openLastSessionFolderRequested();
    void bundleFinished(bool ok, const QString& message);

  private:
    void startProbe(bool run_self_test);
    void applyProbe(diagnostics::DiagnosticsController::ProbeResult probe, bool from_manual_check);
    void refreshSnapshot();
    void refreshPipeline();
    // Rebuilds the five live tiles from the last snapshot and publishes only when
    // they actually differ. The stream arrives at ~5 Hz and almost always
    // rebuilds identical -- a notify per sample would repaint five tiles twice a
    // second for nothing.
    void refreshLiveTiles();
    void refreshLedger();
    void refreshLastSessionMap();
    void refreshSelfTest();
    void setChecking(bool checking);
    void updateLiveProbeTimer();
    // Appends one sample per series from the snapshot the page just received.
    // Not throttled: the sparkline plots the last 60 snapshots, so skipping one
    // would stretch its 12 s window without saying so.
    void appendSeriesSamples(const exosnap::engine::RecordingDiagnosticsSnapshot& snapshot);
    void resetSeries();
    [[nodiscard]] QVariantList seriesFor(const std::string& key) const;
    [[nodiscard]] std::optional<diagnostics::DpcLatencyReading> readDpcLatency() const;

    diagnostics::DiagnosticsController controller_;
    capability::CapabilitySet caps_;
    // Borrowed from the composition root, which must outlive this adapter (see
    // QuickApplication's member declaration order).
    diagnostics::IDpcLatencyProvider* dpc_provider_ = nullptr;
    DiagnosticIssueModel issue_model_;
    PipelineStageModel pipeline_stage_model_;
    SessionLedgerModel ledger_model_;
    std::unique_ptr<SupportBundleService> bundle_service_;

    QDateTime last_check_at_;
    QString self_test_status_;
    QVariantList tiles_;
    QVariantList live_tiles_;
    // The plain structs the QVariantList above was built from, kept so the
    // "did anything change" comparison is over typed values rather than over
    // QVariantMaps.
    std::vector<diagnostics::LiveTile> live_tile_values_;
    QVariantList tips_;
    QVariantList environment_rows_;
    QVariantList config_rows_;
    QVariantList self_test_rows_;
    QVariantMap last_session_;

    // Sparkline history, one bounded deque per live-tile key. Adapter-side by
    // design: the engine publishes instants, and a trend is a property of the
    // surface that watched them go by.
    std::unordered_map<std::string, std::deque<double>> series_;
    uint64_t series_generation_ = 0;
    // Bumped on every append. The tiles can be byte-identical while the trend
    // under them has moved, so the publish gate compares this too.
    uint64_t series_revision_ = 0;
    uint64_t published_series_revision_ = 0;
    // There is no session-wide encoder p99 in the snapshot -- the engine's
    // percentiles come from a rolling window of about two seconds -- so the worst
    // p99 of this session is accumulated here rather than claimed from a window
    // figure that is not one.
    double encoder_session_worst_p99_ms_ = 0.0;
    // Wall clock of session second zero, pinned when a new generation arrives, so
    // the ledger can state times the user can match against their own day.
    QDateTime session_start_;

    diagnostics::VerdictState verdict_state_ = diagnostics::VerdictState::Neutral;
    QString verdict_headline_;
    QString verdict_subline_;
    int blocker_count_ = 0;
    int notice_count_ = 0;

    diagnostics::RefreshThrottle live_throttle_;
    // While recording, the writability + free-space facts are refreshed on this slow
    // cadence instead of the 2 Hz synchronous probe the Widgets page performed on the
    // GUI thread. Deliberately coarse: the fact it measures changes on a human scale.
    QTimer live_probe_timer_;

    std::optional<diagnostics::PresentSample> present_sample_;
    std::optional<diagnostics::DpcLatencyReading> dpc_reading_;

    bool checking_ = false;
    bool probe_in_flight_ = false;
    bool probed_ = false;
    bool in_depth_enabled_ = false;
    bool recording_ = false;
    bool pipeline_live_ = false;
    bool bundle_busy_ = false;

    // Declared last so it is destroyed FIRST: its destructor waits for an
    // in-flight probe, which touches the members above and, through DXGI/COM,
    // process-wide state. A detached QThread here instead crashed at teardown
    // roughly 2.5% of runs — after every test had passed, because the probe
    // outlived the process. The QPointer on the result callback cannot help:
    // it guards the callback, never the worker body. Same idiom as
    // EditSessionAdapter::keyframe_pool_.
    QThreadPool probe_pool_;
};

} // namespace exosnap::quick
