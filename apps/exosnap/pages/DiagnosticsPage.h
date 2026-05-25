#pragma once
#include <QWidget>

#include <capability/capability_set.h>

#include "../diagnostics/CapabilitySummary.h"
#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/RecommendationEngine.h"

class QLabel;
class QPushButton;
class QTabWidget;
class QVBoxLayout;
class QScrollArea;
class QComboBox;
class QSpinBox;
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

  private slots:
    void onRunCheck();
    void onExportReport();
    void onOpenLogFolder();
    void onCopyLatestLogs();
    void onExportDiagnosticsBundle();
    void onLogLevelChanged(int index);

  private:
    void buildOverviewTab(QWidget* container);
    void buildCapabilitiesTab(QWidget* container);
    void buildConfigurationTab(QWidget* container);
    void buildRecommendationsTab(QWidget* container);
    void buildPerformanceTab(QWidget* container);
    void buildLogsTab(QWidget* container);
    void buildSelfTestTab(QWidget* container);

    void refreshOverview();
    void refreshRecommendations();
    void refreshSelfTest();

    QLabel* makeSubLabel(const QString& text, QWidget* parent);
    QLabel* makeSectionLabel(const QString& text, QWidget* parent);
    QFrame* makePanel(QWidget* parent);
    QWidget* makeInfoRow(const QString& label, const QString& value, const QString& status, QWidget* parent);

    QTabWidget* tabs_ = nullptr;

    // Overview
    QLabel* status_label_ = nullptr;
    QLabel* last_check_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* run_check_btn_ = nullptr;
    QPushButton* export_report_btn_ = nullptr;
    QLabel* blocker_count_ = nullptr;
    QLabel* notice_count_ = nullptr;
    QLabel* pass_count_ = nullptr;

    // Capabilities
    QVBoxLayout* capabilities_layout_ = nullptr;
    QWidget* capabilities_content_ = nullptr;

    // Configuration
    QVBoxLayout* config_layout_ = nullptr;
    QWidget* config_content_ = nullptr;

    // Recommendations
    QVBoxLayout* recommendations_layout_ = nullptr;
    QWidget* recommendations_content_ = nullptr;

    // Logs
    QLabel* log_path_label_ = nullptr;
    QComboBox* log_level_combo_ = nullptr;
    QSpinBox* max_file_size_spin_ = nullptr;
    QSpinBox* max_file_count_spin_ = nullptr;

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
};

} // namespace exosnap
