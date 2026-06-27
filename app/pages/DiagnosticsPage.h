#pragma once
#include <QWidget>

#include <capability/capability_set.h>
#include <capability/resolver.h>
#include <recorder_core/pipeline_diagnostics.h>

#include "../diagnostics/CapabilitySummary.h"
#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/RecommendationEngine.h"

#include <cstdint>
#include <filesystem>

class QLabel;
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
} // namespace ui::widgets

struct OutputSettingsModel;
struct VideoSettingsModel;

class DiagnosticsPage : public QWidget {
    Q_OBJECT
  public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);

    void setDiagnosticData(const capability::CapabilitySet& caps, const OutputSettingsModel& output,
                           const VideoSettingsModel& video, const capability::AudioUiState& audio,
                           const std::string& profile_name, const std::string& hotkeys_summary,
                           const std::string& settings_path, bool hotkeys_ok);

    // Live recording-pipeline telemetry, delivered on the UI thread (~5 Hz while
    // recording, plus one final frozen snapshot). Safe to call when idle.
    void applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

  signals:
    void navigateToLogsRequested();
    // v0.8.0-D: FixAction routing — MainWindow wires these in a later wave.
    void applyFixActionRequested(const QString& fix_id, const QString& changes_summary);
    void openAssistedFixRequested(const QString& fix_id);

  private slots:
    void onRunCheck();
    void onExportReport();

  private:
    void refreshOverview();
    void refreshSelfTest();
    void refreshCapabilities();
    void refreshConfiguration();
    void refreshPipeline();
    void refreshTopIssues(const diagnostics::DiagnosticChecklist& recommendations, int total_notices,
                          int total_blockers);
    void setReadinessState(const QString& state);

    QLabel* makeSubLabel(const QString& text, QWidget* parent);
    QFrame* makePanel(QWidget* parent);
    QWidget* makeInfoRow(const QString& label, const QString& value, const QString& status, QWidget* parent,
                         bool first_row);

    // Builds a collapsible "Technical details" section (disclosure head + hidden body).
    // Returns the body widget the caller fills; the body starts collapsed.
    QWidget* makeCollapsibleSection(const QString& title, const QString& subtitle, QWidget* parent,
                                    QToolButton*& out_toggle);

    // Readiness / status
    QFrame* readiness_panel_ = nullptr;
    QLabel* readiness_icon_ = nullptr;
    QLabel* status_pill_ = nullptr;
    QLabel* last_check_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* run_check_btn_ = nullptr;
    QPushButton* export_report_btn_ = nullptr;
    QFrame* blocker_tile_ = nullptr;
    QFrame* notice_tile_ = nullptr;
    QFrame* pass_tile_ = nullptr;
    QLabel* blocker_count_ = nullptr;
    QLabel* notice_count_ = nullptr;
    QLabel* pass_count_ = nullptr;

    // Capture pipeline (the page's visual center)
    ui::widgets::PipelineFlow* pipeline_flow_ = nullptr;

    // Live pipeline telemetry panel (fed by applyLiveDiagnostics).
    ui::widgets::LivePipelinePanel* live_pipeline_panel_ = nullptr;

    // Top Issues / recommendations
    QVBoxLayout* overview_issues_layout_ = nullptr;
    QWidget* issues_parent_ = nullptr;

    // Capability matrix (visible, secondary real-probe table)
    QVBoxLayout* capabilities_layout_ = nullptr;
    QWidget* capabilities_content_ = nullptr;
    ui::widgets::SectionRuleHeader* capabilities_header_ = nullptr;

    // Configuration (collapsible body)
    QVBoxLayout* config_layout_ = nullptr;
    QWidget* config_content_ = nullptr;
    QToolButton* config_toggle_ = nullptr;

    // Self-test
    QVBoxLayout* selftest_layout_ = nullptr;
    QWidget* selftest_content_ = nullptr;
    QPushButton* selftest_run_btn_ = nullptr;
    QLabel* selftest_status_label_ = nullptr;

    // Injected data
    capability::CapabilitySet caps_;
    diagnostics::CapabilitySummary cap_summary_;
    diagnostics::ConfigSummary config_summary_;
    diagnostics::ConfigSummary config_raw_;
    std::string profile_name_;
    std::string hotkeys_summary_;
    std::string settings_path_;
    bool hotkeys_ok_ = false;
    bool data_ready_ = false;
    capability::UserRecorderConfig active_user_config_{};
    capability::ResolveResult profile_validation_;

    // Disk-space guard data (LOW-DISK-GUARD-R1)
    std::filesystem::path output_folder_;
    uint64_t output_drive_free_bytes_ = 0;

    // Filesystem-check data (FILESYSTEM-CHECKS-R1)
    std::string output_filesystem_name_; // e.g. "FAT32", "NTFS"; empty = not queried

    // Last live pipeline snapshot (fed by applyLiveDiagnostics). Consumed by refreshOverview's
    // RecommendationEngine for the VRR/CFR judder correlation (v0.8.0 / ADR 0033). Only valid
    // snapshots are forwarded to the engine.
    recorder_core::RecordingDiagnosticsSnapshot last_live_snapshot_{};
};

} // namespace exosnap
