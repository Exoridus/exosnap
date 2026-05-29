#pragma once
#include <QWidget>

#include <capability/capability_set.h>
#include <capability/resolver.h>

#include "../diagnostics/CapabilitySummary.h"
#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/RecommendationEngine.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class QScrollArea;
class QFrame;

namespace exosnap {

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

  signals:
    void navigateToLogsRequested();

  private slots:
    void onRunCheck();
    void onExportReport();

  private:
    void refreshOverview();
    void refreshSelfTest();
    void refreshCapabilities();
    void refreshConfiguration();
    void refreshTopIssues(const diagnostics::DiagnosticChecklist& recommendations, int total_notices,
                          int total_blockers);

    QLabel* makeSubLabel(const QString& text, QWidget* parent);
    QLabel* makeSectionLabel(const QString& text, QWidget* parent);
    QFrame* makePanel(QWidget* parent);
    QWidget* makeInfoRow(const QString& label, const QString& value, const QString& status, QWidget* parent);

    // Readiness / status
    QLabel* status_label_ = nullptr;
    QLabel* last_check_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* run_check_btn_ = nullptr;
    QPushButton* export_report_btn_ = nullptr;
    QLabel* blocker_count_ = nullptr;
    QLabel* notice_count_ = nullptr;
    QLabel* pass_count_ = nullptr;

    // Top Issues
    QVBoxLayout* overview_issues_layout_ = nullptr;
    QWidget* issues_parent_ = nullptr;

    // Capabilities
    QVBoxLayout* capabilities_layout_ = nullptr;
    QWidget* capabilities_content_ = nullptr;

    // Configuration
    QVBoxLayout* config_layout_ = nullptr;
    QWidget* config_content_ = nullptr;

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
};

} // namespace exosnap
