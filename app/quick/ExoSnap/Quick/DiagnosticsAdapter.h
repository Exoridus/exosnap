#pragma once

#include "DiagnosticIssueModel.h"

#include "diagnostics/DiagnosticsController.h"
#include "services/SupportBundleService.h"

#include <capability/capability_set.h>

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

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

    Q_PROPERTY(bool expertMode READ expertMode WRITE setExpertMode NOTIFY expertModeChanged FINAL)
    Q_PROPERTY(bool hasLastRecording READ hasLastRecording NOTIFY hasLastRecordingChanged FINAL)
    Q_PROPERTY(bool elevated READ elevated NOTIFY environmentChanged FINAL)

    // Declared as the Qt base type: qmltyperegistrar records the concrete subclass
    // under its namespaced C++ name while moc writes the property type unqualified,
    // so a concrete spelling here is unresolvable for qmllint.
    Q_PROPERTY(QAbstractListModel* issues READ issues CONSTANT FINAL)

    Q_PROPERTY(QVariantList tiles READ tiles NOTIFY tilesChanged FINAL)
    Q_PROPERTY(QVariantList tips READ tips NOTIFY issuesChanged FINAL)
    Q_PROPERTY(bool hasIssues READ hasIssues NOTIFY issuesChanged FINAL)
    Q_PROPERTY(QVariantList environmentRows READ environmentRows NOTIFY environmentChanged FINAL)
    Q_PROPERTY(QVariantList configRows READ configRows NOTIFY environmentChanged FINAL)
    Q_PROPERTY(QVariantList selfTestRows READ selfTestRows NOTIFY selfTestChanged FINAL)
    Q_PROPERTY(QString selfTestStatus READ selfTestStatus NOTIFY selfTestChanged FINAL)
    Q_PROPERTY(QVariantList pipelineStages READ pipelineStages NOTIFY pipelineChanged FINAL)
    Q_PROPERTY(bool pipelineLive READ pipelineLive NOTIFY pipelineChanged FINAL)

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
    [[nodiscard]] const QString& lastCheckText() const noexcept;
    [[nodiscard]] bool checking() const noexcept;
    [[nodiscard]] bool dataReady() const noexcept;
    [[nodiscard]] bool expertMode() const noexcept;
    void setExpertMode(bool enabled);
    [[nodiscard]] bool hasLastRecording() const noexcept;
    [[nodiscard]] bool elevated() const noexcept;
    [[nodiscard]] QAbstractListModel* issues() noexcept;
    [[nodiscard]] const QVariantList& tiles() const noexcept;
    [[nodiscard]] const QVariantList& tips() const noexcept;
    [[nodiscard]] bool hasIssues() const noexcept;
    [[nodiscard]] const QVariantList& environmentRows() const noexcept;
    [[nodiscard]] const QVariantList& configRows() const noexcept;
    [[nodiscard]] const QVariantList& selfTestRows() const noexcept;
    [[nodiscard]] const QString& selfTestStatus() const noexcept;
    [[nodiscard]] const QVariantList& pipelineStages() const noexcept;
    [[nodiscard]] bool pipelineLive() const noexcept;
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
    Q_INVOKABLE void openDevice();
    Q_INVOKABLE void openLastReport();

    // ── Host-side input (C++ only) ─────────────────────────────────────────────
    void setDiagnosticConfig(diagnostics::DiagnosticsController::Config config);
    void setCapabilitySet(const capability::CapabilitySet& caps);
    void setSelectedCaptureTarget(std::optional<recorder_core::CaptureTarget> target);
    void setSavedDisplayUnresolved(bool unresolved, const std::string& label);
    void setCaptureWindowEvidence(std::optional<diagnostics::WindowTargetFacts> facts,
                                  const diagnostics::WindowHubEvidence& hub);
    void setCaptureTargetHdrActive(bool active);
    void setElevated(bool elevated);
    void setHasLastRecording(bool has_last_recording);
    void setDpcLatency(diagnostics::DpcLatencyReading reading);
    void setPresentSample(std::optional<diagnostics::PresentSample> sample);
    void applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);
    // Emitted by the composition root once an assisted fix has resolved to a
    // Settings section, so the page can route the navigation.
    void requestSettingsNavigation();

    // Test seam: applies a probe result without touching the filesystem.
    void applyProbeResultForTest(diagnostics::DiagnosticsController::ProbeResult probe);
    [[nodiscard]] diagnostics::DiagnosticsController& controllerForTest() noexcept;
    void refreshForTest();

  signals:
    void verdictChanged();
    void lastCheckChanged();
    void checkingChanged();
    void expertModeChanged(bool enabled);
    void hasLastRecordingChanged();
    void tilesChanged();
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
    void navigateToDeviceRequested();
    void navigateToSettingsRequested();
    void openLastReportRequested();
    void bundleFinished(bool ok, const QString& message);

  private:
    void startProbe(bool run_self_test);
    void applyProbe(diagnostics::DiagnosticsController::ProbeResult probe, bool from_manual_check);
    void refreshSnapshot();
    void refreshPipeline();
    void refreshSelfTest();
    void setChecking(bool checking);
    void updateLiveProbeTimer();

    diagnostics::DiagnosticsController controller_;
    capability::CapabilitySet caps_;
    DiagnosticIssueModel issue_model_;
    std::unique_ptr<SupportBundleService> bundle_service_;

    QString last_check_text_;
    QString self_test_status_;
    QVariantList tiles_;
    QVariantList tips_;
    QVariantList environment_rows_;
    QVariantList config_rows_;
    QVariantList self_test_rows_;
    QVariantList pipeline_stages_;

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

    bool checking_ = false;
    bool probe_in_flight_ = false;
    bool probed_ = false;
    bool expert_mode_ = false;
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
