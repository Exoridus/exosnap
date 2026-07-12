#pragma once
#include <QWidget>

#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <recorder_core/pipeline_diagnostics.h>
#include <recorder_core/pipeline_health.h>
#include <recorder_core/recorder_session.h>

#include "../diagnostics/CapabilitySummary.h"
#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/DpcLatencyProvider.h"
#include "../diagnostics/PresentProvider.h"
#include "../diagnostics/RecommendationEngine.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

class QLabel;
class QProgressBar;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QScrollArea;
class QFrame;

namespace exosnap {

namespace ui::widgets {
class PipelineFlow;
class SectionRuleHeader;
class LivePipelinePanel;
class ExoToggle;
class TipChip;
class ElevationLock;
} // namespace ui::widgets

struct OutputSettingsModel;
struct VideoSettingsModel;

// Diagnostics surface (suite-diag2.jsx end-state). A Simple default view — compact
// verdict + four readiness tiles + any Tier-1 blocker / Tier-2 measured problem +
// one bundled Tier-3 tip chip — with an Expert toggle that reveals the full flat
// taxonomy beneath the SAME verdict + tiles: ② Pre-flight → ③ Live → ④ Post-flight,
// plus the elevation unlock. Capability facts moved to the Device tab, so the old
// capability matrix is gone (see openDevicePageRequested).
class DiagnosticsPage : public QWidget {
    Q_OBJECT
  public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

    void setDiagnosticData(const capability::CapabilitySet& caps, const OutputSettingsModel& output,
                           const VideoSettingsModel& video, const capability::AudioUiState& audio,
                           const std::string& profile_name, const std::string& hotkeys_summary,
                           const std::string& settings_path, bool hotkeys_ok);

    // The currently selected capture target, used to resolve the target display's
    // HDR status for the HDR10 + H.264 pre-flight blocker (rec.hdr.h264). Set by
    // the owner whenever the selection changes; nullopt = no selection / SDR.
    void setSelectedCaptureTarget(const std::optional<recorder_core::CaptureTarget>& target);

    // Whether a concretely-saved capture target could not be resolved to any
    // connected display (stable-display-identity). Feeds the calm "saved display
    // not found" notice. label describes the saved (missing) display.
    void setSavedDisplayUnresolved(bool unresolved, const std::string& label);

    // Honest exclusive-fullscreen facts for the selected window capture target,
    // supplied by the owner from WindowEvidenceProbe (S2a). facts == nullopt for a
    // monitor target / no selection: the exclusive-window card stays silent. The
    // engine combines these with its present sample into the severity ladder.
    void setCaptureWindowEvidence(const std::optional<diagnostics::WindowTargetFacts>& facts,
                                  const diagnostics::WindowHubEvidence& hub);

    // Live recording-pipeline telemetry, delivered on the UI thread (~5 Hz while
    // recording, plus one final frozen snapshot). Safe to call when idle.
    void applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    // ADR 0033: inject the present/tearing diagnostics provider (borrowed, nullable).
    void setPresentProvider(diagnostics::IPresentProvider* provider) noexcept;

    // ADR 0033: inject the kernel DPC/ISR latency provider (borrowed, nullable).
    void setDpcProvider(diagnostics::DpcLatencyProvider* provider) noexcept;

    // Consumes the single global Expert mode (AppSettingsStore::expert_mode_enabled).
    // Mirrors ConfigPage::setExpertModeEnabled — the toolbar toggle and this setter
    // read/write the SAME persisted value (MainWindow keeps both pages in sync). No
    // second state. No-ops when unchanged, so cross-page sync can't loop.
    void setExpertModeEnabled(bool enabled);
    [[nodiscard]] bool isExpertModeEnabled() const noexcept;

    // SETTINGS-HONESTY-R1: gates Phase ④'s "Open last report" link. MainWindow calls
    // this whenever a recording completes/fails or the page is (re)built, mirroring
    // RecordPage::hasCompletedRecording(). No-op-safe before the button is built.
    void setHasLastRecording(bool has_last_recording);

  signals:
    void navigateToLogsRequested();
    // Second entry point (beside the Logs page) to the one support-bundle action.
    void createSupportBundleRequested();
    // v0.8.0-D: FixAction routing — MainWindow wires these in a later wave.
    void applyFixActionRequested(const QString& fix_id, const QString& changes_summary);
    void openAssistedFixRequested(const QString& fix_id);

    // Emitted when the toolbar Expert toggle flips. MainWindow persists this into
    // AppSettingsStore::expert_mode_enabled and mirrors it onto ConfigPage (single
    // source of truth). Guarded by the no-op check in setExpertModeEnabled.
    void expertModeChanged(bool enabled);

    // Hardware capability facts moved to the Device page. The Expert environment
    // row emits this; MainWindow routes it to kDevicePageIndex.
    void openDevicePageRequested();

    // SETTINGS-HONESTY-R1: Phase ④'s "Open last report" link. The real post-flight
    // report lives on the Edit overlay's Review step (EditExportPage); this button
    // never duplicates it, only routes there. The button itself is disabled unless
    // a completed recording exists to open (see setHasLastRecording); MainWindow's
    // handler re-checks the same gate before routing.
    void openLastReportRequested();

    // Review F4: emitted on every showEvent so MainWindow re-pushes the current
    // last-recording gate (setHasLastRecording). Covers the timing residue where
    // last_succeeded flips after the most recent chrome-state event -- the page
    // stays ignorant of recording state; it only asks to be refreshed.
    void lastRecordingGateRefreshRequested();

  private slots:
    void onRunCheck();
    void onExportReport();

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void refreshOverview();
    void refreshSelfTest();
    void refreshConfiguration();
    void refreshPipeline();
    void updatePipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);
    void renderPipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    // Splits engine results into Tier-1 blockers / Tier-2 measured problems (issue
    // cards) and Tier-3 optimisations (bundled into the tip chip).
    void refreshTopIssues(const diagnostics::DiagnosticChecklist& recommendations);

    void refreshReadinessTiles(int blockers, int notices, int cap_passes);
    void setReadinessState(const QString& state);
    void applyExpertVisibility();

    QLabel* makeSubLabel(const QString& text, QWidget* parent);
    QFrame* makePanel(QWidget* parent);
    QWidget* makeInfoRow(const QString& label, const QString& value, const QString& status, QWidget* parent,
                         bool first_row);
    QWidget* makeCollapsibleSection(const QString& title, const QString& subtitle, QWidget* parent,
                                    QToolButton*& out_toggle);

    // Builds one wide readiness tile (title · value · sub, optional trailing check).
    QFrame* makeReadinessTile(const QString& object_name, const QString& title, QLabel*& out_value, QLabel*& out_sub,
                              QLabel*& out_icon);

    // ── Toolbar (Expert toggle mirrors Settings) ───────────────────────────────
    ui::widgets::ExoToggle* expert_toggle_ = nullptr;
    QLabel* expert_mode_label_ = nullptr;
    QLabel* mode_caption_ = nullptr;

    // ── Verdict banner (kept: readinessBanner + status pill + last-check) ───────
    QFrame* readiness_panel_ = nullptr;
    QLabel* readiness_icon_ = nullptr;
    QLabel* status_pill_ = nullptr;
    // Verdict hero (canon suite-diag2.jsx CompactVerdict).
    QLabel* verdict_glyph_ = nullptr;
    QLabel* verdict_headline_ = nullptr;
    QProgressBar* disk_bar_ = nullptr;
    QLabel* last_check_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* run_check_btn_ = nullptr;
    QPushButton* export_report_btn_ = nullptr;

    // ── Four readiness tiles (Readiness · Encoder · Disk · Display) ─────────────
    QFrame* readiness_tile_ = nullptr;
    QLabel* readiness_tile_value_ = nullptr;
    QLabel* readiness_tile_sub_ = nullptr;
    QLabel* readiness_tile_icon_ = nullptr;
    QLabel* encoder_tile_value_ = nullptr;
    QLabel* encoder_tile_sub_ = nullptr;
    QLabel* disk_tile_value_ = nullptr;
    QLabel* disk_tile_sub_ = nullptr;
    QLabel* display_tile_value_ = nullptr;
    QLabel* display_tile_sub_ = nullptr;

    // ── Worst-first cards (shared: visible in both Simple + Expert) ─────────────
    QVBoxLayout* overview_issues_layout_ = nullptr;
    QWidget* issues_parent_ = nullptr;
    ui::widgets::TipChip* tip_chip_ = nullptr;

    // ── Expert-only container (phases + elevation) ─────────────────────────────
    QWidget* expert_container_ = nullptr;

    // Capture pipeline (Phase ③ health cards) — always constructed so live wiring +
    // tests work regardless of view; hidden in Simple.
    ui::widgets::PipelineFlow* pipeline_flow_ = nullptr;
    ui::widgets::LivePipelinePanel* live_pipeline_panel_ = nullptr;

    // Active configuration (collapsible reference, Expert).
    QVBoxLayout* config_layout_ = nullptr;
    QWidget* config_content_ = nullptr;
    QToolButton* config_toggle_ = nullptr;

    // Self-test (Expert · Pre-flight).
    QVBoxLayout* selftest_layout_ = nullptr;
    QWidget* selftest_content_ = nullptr;
    QPushButton* selftest_run_btn_ = nullptr;
    QLabel* selftest_status_label_ = nullptr;

    ui::widgets::ElevationLock* elevation_lock_ = nullptr;

    // SETTINGS-HONESTY-R1: Phase ④ "Open last report" link + its gate state.
    QPushButton* open_last_report_btn_ = nullptr;
    bool has_last_recording_ = false;

    // ── Injected data ──────────────────────────────────────────────────────────
    capability::CapabilitySet caps_;
    diagnostics::CapabilitySummary cap_summary_;
    diagnostics::ConfigSummary config_summary_;
    std::string profile_name_;
    std::string hotkeys_summary_;
    std::string settings_path_;
    bool hotkeys_ok_ = false;
    bool data_ready_ = false;
    bool expert_mode_enabled_ = false;
    std::optional<recorder_core::CaptureTarget> selected_capture_target_;
    std::optional<diagnostics::WindowTargetFacts> capture_window_facts_;
    diagnostics::WindowHubEvidence capture_window_hub_;
    bool saved_display_unresolved_ = false;
    std::string saved_display_label_;
    capability::UserRecorderConfig active_user_config_{};
    capability::ResolveResult profile_validation_;

    std::filesystem::path output_folder_;
    std::optional<uint64_t> output_drive_free_bytes_; // nullopt = volume not queryable
    std::string output_filesystem_name_;

    recorder_core::RecordingDiagnosticsSnapshot last_live_snapshot_{};

    std::chrono::steady_clock::time_point last_cards_applied_{};
    uint64_t cards_last_generation_ = 0;
    uint64_t cards_last_problem_drops_ = 0;

    diagnostics::IPresentProvider* present_provider_ = nullptr;
    diagnostics::DpcLatencyProvider* dpc_provider_ = nullptr;
};

} // namespace exosnap
