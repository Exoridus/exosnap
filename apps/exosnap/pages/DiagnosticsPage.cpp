#include "DiagnosticsPage.h"

#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/RecommendationEngine.h"
#include "../diagnostics/SelfTestRunner.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"

#include <capability/audio_ui_state.h>
#include <capability/user_config.h>

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap {

namespace {

constexpr int kLogLevelBasic = 0;
constexpr int kLogLevelVerbose = 1;
constexpr int kLogLevelTrace = 2;

QString severityClass(diagnostics::DiagnosticSeverity sev) {
    switch (sev) {
    case diagnostics::DiagnosticSeverity::Pass:
        return QStringLiteral("pass");
    case diagnostics::DiagnosticSeverity::Notice:
        return QStringLiteral("notice");
    case diagnostics::DiagnosticSeverity::Blocker:
        return QStringLiteral("blocker");
    }
    return QStringLiteral("pass");
}

QString severityIcon(diagnostics::DiagnosticSeverity sev) {
    switch (sev) {
    case diagnostics::DiagnosticSeverity::Pass:
        return QString::fromUtf8("\xe2\x9c\x93"); // checkmark
    case diagnostics::DiagnosticSeverity::Notice:
        return QString::fromUtf8("\xe2\x9a\xa0"); // warning
    case diagnostics::DiagnosticSeverity::Blocker:
        return QString::fromUtf8("\xe2\x9c\x97"); // cross
    }
    return QStringLiteral("?");
}

QFrame* makeHorizontalRule(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::HLine);
    f->setObjectName(QStringLiteral("diagnosticRule"));
    return f;
}

} // namespace

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);

    auto* overview = new QWidget();
    auto* capabilities = new QWidget();
    auto* configuration = new QWidget();
    auto* recommendations = new QWidget();
    auto* performance = new QWidget();
    auto* logs = new QWidget();
    auto* selftest = new QWidget();

    buildOverviewTab(overview);
    buildCapabilitiesTab(capabilities);
    buildConfigurationTab(configuration);
    buildRecommendationsTab(recommendations);
    buildPerformanceTab(performance);
    buildLogsTab(logs);
    buildSelfTestTab(selftest);

    tabs_->addTab(overview, QStringLiteral("Overview"));
    tabs_->addTab(capabilities, QStringLiteral("Capabilities"));
    tabs_->addTab(configuration, QStringLiteral("Configuration"));
    tabs_->addTab(recommendations, QStringLiteral("Recommendations"));
    tabs_->addTab(performance, QStringLiteral("Performance"));
    tabs_->addTab(logs, QStringLiteral("Logs"));
    tabs_->addTab(selftest, QStringLiteral("Self-Test"));

    root->addWidget(tabs_);
}

void DiagnosticsPage::setDiagnosticData(const capability::CapabilitySet& caps, const OutputSettingsModel& output,
                                        const VideoSettingsModel& video, const capability::AudioUiState& audio,
                                        const std::string& profile_name, const std::string& hotkeys_summary,
                                        const std::string& settings_path, bool hotkeys_ok) {
    caps_ = caps;
    profile_name_ = profile_name;
    hotkeys_summary_ = hotkeys_summary;
    settings_path_ = settings_path;
    hotkeys_ok_ = hotkeys_ok;

    cap_summary_ = diagnostics::CapabilitySummary::FromCapabilitySet(caps_);
    config_summary_ = diagnostics::ConfigSummary::FromCurrentSettings(
        output, video, audio, std::filesystem::path(settings_path_), profile_name_, hotkeys_summary_);
    data_ready_ = true;

    refreshOverview();
    refreshRecommendations();
    refreshSelfTest();

    // Populate capabilities table
    if (capabilities_layout_ && capabilities_content_) {
        // Clear existing
        QLayoutItem* child;
        while ((child = capabilities_layout_->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        for (const auto& entry : cap_summary_.entries) {
            capabilities_layout_->addWidget(makeInfoRow(QString::fromStdString(entry.label),
                                                        QString::fromStdString(entry.value),
                                                        QString::fromStdString(entry.status), capabilities_content_));
        }
        capabilities_layout_->addStretch();
    }

    // Populate configuration table
    if (config_layout_ && config_content_) {
        QLayoutItem* child;
        while ((child = config_layout_->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }
        for (const auto& entry : config_summary_.entries) {
            config_layout_->addWidget(makeInfoRow(QString::fromStdString(entry.label),
                                                  QString::fromStdString(entry.value), QString(), config_content_));
        }
        config_layout_->addStretch();
    }

    // Update log path
    if (log_path_label_) {
        QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        log_path_label_->setText(data_dir + QStringLiteral("/logs/exosnap.log"));
    }
}

// --- Helpers ---

QLabel* DiagnosticsPage::makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "subtitle");
    l->setWordWrap(true);
    return l;
}

QLabel* DiagnosticsPage::makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "section");
    return l;
}

QFrame* DiagnosticsPage::makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

QWidget* DiagnosticsPage::makeInfoRow(const QString& label, const QString& value, const QString& status,
                                      QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceSm, ui::theme::ExoSnapMetrics::kSpaceXs,
                                   ui::theme::ExoSnapMetrics::kSpaceSm, ui::theme::ExoSnapMetrics::kSpaceXs);
    row_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    auto* name_label = new QLabel(label, row);
    name_label->setProperty("labelRole", "body");
    name_label->setMinimumWidth(180);
    row_layout->addWidget(name_label);

    auto* value_label = new QLabel(value, row);
    value_label->setProperty("labelRole", "mono");
    value_label->setWordWrap(true);
    row_layout->addWidget(value_label, 1);

    if (!status.isEmpty()) {
        auto* status_label = new QLabel(status, row);
        QString status_lower = status.toLower();
        if (status_lower == QStringLiteral("available") || status_lower == QStringLiteral("pass") ||
            status_lower == QStringLiteral("info")) {
            status_label->setProperty("labelRole", "statusGood");
        } else if (status_lower == QStringLiteral("unavailable")) {
            status_label->setProperty("labelRole", "statusBad");
        } else {
            status_label->setProperty("labelRole", "subtle");
        }
        row_layout->addWidget(status_label);
    }

    return row;
}

// --- Overview Tab ---

void DiagnosticsPage::buildOverviewTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    layout->addWidget(makeSectionLabel(QStringLiteral("Readiness / Capability Overview"), content));

    auto* overview_panel = makePanel(content);
    auto* overview_layout = new QVBoxLayout(overview_panel);
    overview_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                        ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    overview_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    overview_layout->addWidget(makeSubLabel(
        QStringLiteral("Diagnostics are designed to block invalid recording states early."), overview_panel));

    status_label_ = new QLabel(QStringLiteral("Overall status: Not checked"), overview_panel);
    status_label_->setProperty("labelRole", "body");
    overview_layout->addWidget(status_label_);

    last_check_label_ = new QLabel(QStringLiteral("Last check: —"), overview_panel);
    last_check_label_->setProperty("labelRole", "subtle");
    overview_layout->addWidget(last_check_label_);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    run_check_btn_ = new QPushButton(QStringLiteral("Run System & Pipeline Check"), overview_panel);
    run_check_btn_->setProperty("role", "primary");
    export_report_btn_ = new QPushButton(QStringLiteral("Export Diagnostic Report"), overview_panel);
    export_report_btn_->setProperty("role", "ghost");
    export_report_btn_->setEnabled(false);

    btn_row->addWidget(run_check_btn_);
    btn_row->addWidget(export_report_btn_);
    btn_row->addStretch();
    overview_layout->addLayout(btn_row);

    summary_label_ = new QLabel(QStringLiteral("Run a check to see results."), overview_panel);
    summary_label_->setProperty("labelRole", "muted");
    overview_layout->addWidget(summary_label_);

    // Counts row
    auto* counts_row = new QHBoxLayout();
    counts_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXl);

    auto* blocker_panel = new QFrame(overview_panel);
    blocker_panel->setProperty("panelRole", "compactRow");
    auto* blocker_layout = new QHBoxLayout(blocker_panel);
    blocker_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                       ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);
    blocker_count_ = new QLabel(QStringLiteral("0"), blocker_panel);
    blocker_count_->setProperty("labelRole", "countBlocker");
    auto* blocker_label = new QLabel(QStringLiteral("Blockers"), blocker_panel);
    blocker_label->setProperty("labelRole", "body");
    blocker_layout->addWidget(blocker_count_);
    blocker_layout->addWidget(blocker_label);
    counts_row->addWidget(blocker_panel);

    auto* notice_panel = new QFrame(overview_panel);
    notice_panel->setProperty("panelRole", "compactRow");
    auto* notice_inner = new QHBoxLayout(notice_panel);
    notice_inner->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                     ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);
    notice_count_ = new QLabel(QStringLiteral("0"), notice_panel);
    notice_count_->setProperty("labelRole", "countNotice");
    auto* notice_label = new QLabel(QStringLiteral("Notices"), notice_panel);
    notice_label->setProperty("labelRole", "body");
    notice_inner->addWidget(notice_count_);
    notice_inner->addWidget(notice_label);
    counts_row->addWidget(notice_panel);

    auto* pass_panel = new QFrame(overview_panel);
    pass_panel->setProperty("panelRole", "compactRow");
    auto* pass_inner = new QHBoxLayout(pass_panel);
    pass_inner->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                   ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);
    pass_count_ = new QLabel(QStringLiteral("0"), pass_panel);
    pass_count_->setProperty("labelRole", "countPass");
    auto* pass_label = new QLabel(QStringLiteral("Passes"), pass_panel);
    pass_label->setProperty("labelRole", "body");
    pass_inner->addWidget(pass_count_);
    pass_inner->addWidget(pass_label);
    counts_row->addWidget(pass_panel);

    counts_row->addStretch();
    overview_layout->addLayout(counts_row);

    layout->addWidget(overview_panel);
    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(run_check_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
    connect(export_report_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onExportReport);
}

void DiagnosticsPage::onRunCheck() {
    status_label_->setText(QStringLiteral("Overall status: Checking..."));
    last_check_label_->setText(QStringLiteral("Last check: running..."));
    summary_label_->setText(QStringLiteral("Check in progress."));

    if (!data_ready_) {
        status_label_->setText(QStringLiteral("Overall status: No data available"));
        last_check_label_->setText(QStringLiteral("Last check: —"));
        summary_label_->setText(QStringLiteral("Diagnostic data has not been loaded. Open the Record page first."));
        return;
    }

    refreshOverview();
    refreshRecommendations();
    refreshSelfTest();
}

void DiagnosticsPage::onExportReport() {
    // Disabled until full report export is wired.
}

// --- Capabilities Tab ---

void DiagnosticsPage::buildCapabilitiesTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    capabilities_content_ = new QWidget();
    capabilities_layout_ = new QVBoxLayout(capabilities_content_);
    capabilities_layout_->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                                             ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    capabilities_layout_->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);

    capabilities_layout_->addWidget(
        makeSectionLabel(QStringLiteral("Hardware & Software Capabilities"), capabilities_content_));
    capabilities_layout_->addWidget(makeSubLabel(
        QStringLiteral("Detected capabilities from the current system. Values in red/grey are not available."),
        capabilities_content_));
    capabilities_layout_->addSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    // Header row
    auto* header_row = new QWidget(capabilities_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceSm, 0, ui::theme::ExoSnapMetrics::kSpaceSm, 0);
    auto* h1 = new QLabel(QStringLiteral("Feature"), header_row);
    h1->setProperty("labelRole", "section");
    h1->setMinimumWidth(180);
    auto* h2 = new QLabel(QStringLiteral("Detected Value"), header_row);
    h2->setProperty("labelRole", "section");
    auto* h3 = new QLabel(QStringLiteral("Status"), header_row);
    h3->setProperty("labelRole", "section");
    header_layout->addWidget(h1);
    header_layout->addWidget(h2, 1);
    header_layout->addWidget(h3);
    capabilities_layout_->addWidget(header_row);
    capabilities_layout_->addWidget(makeHorizontalRule(capabilities_content_));

    capabilities_layout_->addWidget(makeSubLabel(
        QStringLiteral("Run a system check from the Overview tab to populate this list."), capabilities_content_));
    capabilities_layout_->addStretch();

    scroll->setWidget(capabilities_content_);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

// --- Configuration Tab ---

void DiagnosticsPage::buildConfigurationTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    config_content_ = new QWidget();
    config_layout_ = new QVBoxLayout(config_content_);
    config_layout_->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                                       ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    config_layout_->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);

    config_layout_->addWidget(makeSectionLabel(QStringLiteral("Current Configuration"), config_content_));
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Active recording settings as currently configured in the app."), config_content_));
    config_layout_->addSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    // Header
    auto* header_row = new QWidget(config_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceSm, 0, ui::theme::ExoSnapMetrics::kSpaceSm, 0);
    auto* h1 = new QLabel(QStringLiteral("Setting"), header_row);
    h1->setProperty("labelRole", "section");
    h1->setMinimumWidth(180);
    auto* h2 = new QLabel(QStringLiteral("Value"), header_row);
    h2->setProperty("labelRole", "section");
    header_layout->addWidget(h1);
    header_layout->addWidget(h2, 1);
    config_layout_->addWidget(header_row);
    config_layout_->addWidget(makeHorizontalRule(config_content_));

    config_layout_->addWidget(makeSubLabel(
        QStringLiteral("Run a system check from the Overview tab to populate this list."), config_content_));
    config_layout_->addStretch();

    scroll->setWidget(config_content_);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

// --- Recommendations Tab ---

void DiagnosticsPage::buildRecommendationsTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    recommendations_content_ = new QWidget();
    recommendations_layout_ = new QVBoxLayout(recommendations_content_);
    recommendations_layout_->setContentsMargins(
        ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
        ui::theme::ExoSnapMetrics::kSpaceXl);
    recommendations_layout_->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    recommendations_layout_->addWidget(makeSectionLabel(QStringLiteral("Recommendations"), recommendations_content_));
    recommendations_layout_->addWidget(makeSubLabel(
        QStringLiteral("Rule-based warnings about your current configuration."), recommendations_content_));

    recommendations_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check from the Overview tab to generate recommendations."),
                     recommendations_content_));
    recommendations_layout_->addStretch();

    scroll->setWidget(recommendations_content_);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

void DiagnosticsPage::refreshRecommendations() {
    if (!recommendations_layout_ || !recommendations_content_ || !data_ready_)
        return;

    // Clear existing
    QLayoutItem* child;
    while ((child = recommendations_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    recommendations_layout_->addWidget(makeSectionLabel(QStringLiteral("Recommendations"), recommendations_content_));
    recommendations_layout_->addWidget(makeSubLabel(
        QStringLiteral("Rule-based warnings about your current configuration."), recommendations_content_));
    recommendations_layout_->addWidget(makeHorizontalRule(recommendations_content_));

    capability::UserRecorderConfig user_config;
    user_config.container = config_summary_.entries.size() > 2 ? [&]() {
        auto c = config_summary_.entries[2].value;
        if (c == "MKV")
            return capability::Container::Matroska;
        if (c == "MP4")
            return capability::Container::Mp4;
        return capability::Container::WebM;
    }()
                                                               : capability::Container::Matroska;
    user_config.frame_rate_num = 60;
    user_config.frame_rate_den = 1;

    // Parse codec from config summary if available
    diagnostics::RecommendationEngine engine(caps_, user_config, /*refresh=*/0, /*drive free=*/0, true);
    auto checklist = engine.Generate();

    if (checklist.results.empty()) {
        recommendations_layout_->addWidget(makeSubLabel(
            QStringLiteral("No recommendations. Your configuration looks good."), recommendations_content_));
    } else {
        for (const auto& result : checklist.results) {
            auto* card = makePanel(recommendations_content_);
            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                            ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
            card_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceXs);

            auto* title_row = new QHBoxLayout();
            auto* sev_icon = new QLabel(severityIcon(result.severity), card);
            sev_icon->setProperty("labelRole", severityClass(result.severity));
            auto* title_label = new QLabel(QString::fromStdString(result.title), card);
            title_label->setProperty("labelRole", "body");
            title_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
            title_row->addWidget(sev_icon);
            title_row->addWidget(title_label, 1);
            card_layout->addLayout(title_row);

            auto* summary_lbl = new QLabel(QString::fromStdString(result.summary), card);
            summary_lbl->setProperty("labelRole", "subtle");
            summary_lbl->setWordWrap(true);
            card_layout->addWidget(summary_lbl);

            if (!result.recommendation.empty()) {
                auto* rec_lbl = new QLabel(QString::fromStdString("Recommendation: " + result.recommendation), card);
                rec_lbl->setProperty("labelRole", "muted");
                rec_lbl->setWordWrap(true);
                card_layout->addWidget(rec_lbl);
            }

            recommendations_layout_->addWidget(card);
        }
    }

    recommendations_layout_->addStretch();
}

// --- Performance Tab ---

void DiagnosticsPage::buildPerformanceTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    layout->addWidget(makeSectionLabel(QStringLiteral("Performance / Trace"), content));
    auto* panel = makePanel(content);
    auto* panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                     ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    panel_layout->addWidget(
        makeSubLabel(QStringLiteral("Performance waterfall and tracing is not yet available in this build."), panel));

    auto* placeholder =
        new QLabel(QStringLiteral("[waterfall] Frame timing waterfall\n"
                                  "[timeline] Encoder timeline\n"
                                  "[metrics] Real-time performance counters\n\n"
                                  "These views will be populated when the full trace infrastructure is integrated."),
                   panel);
    placeholder->setProperty("labelRole", "mono");
    placeholder->setProperty("panelRole", "placeholder");
    panel_layout->addWidget(placeholder);

    layout->addWidget(panel);
    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);
}

// --- Logs Tab ---

void DiagnosticsPage::buildLogsTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    // Log path
    layout->addWidget(makeSectionLabel(QStringLiteral("Log File Location"), content));
    auto* path_panel = makePanel(content);
    auto* path_layout = new QVBoxLayout(path_panel);
    path_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                    ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    path_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    log_path_label_ = new QLabel(QStringLiteral("Not initialized"), path_panel);
    log_path_label_->setProperty("labelRole", "mono");
    log_path_label_->setWordWrap(true);
    log_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_layout->addWidget(log_path_label_);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* open_btn = new QPushButton(QStringLiteral("Open Log Folder"), path_panel);
    open_btn->setProperty("role", "ghost");
    auto* copy_btn = new QPushButton(QStringLiteral("Copy Latest Logs"), path_panel);
    copy_btn->setProperty("role", "ghost");
    auto* bundle_btn = new QPushButton(QStringLiteral("Export Diagnostics Bundle"), path_panel);
    bundle_btn->setProperty("role", "ghost");

    btn_row->addWidget(open_btn);
    btn_row->addWidget(copy_btn);
    btn_row->addWidget(bundle_btn);
    btn_row->addStretch();
    path_layout->addLayout(btn_row);

    layout->addWidget(path_panel);

    // Log level
    layout->addWidget(makeSectionLabel(QStringLiteral("Log Level"), content));
    auto* level_panel = makePanel(content);
    auto* level_layout = new QVBoxLayout(level_panel);
    level_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                     ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    level_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    log_level_combo_ = new QComboBox(level_panel);
    log_level_combo_->addItem(QStringLiteral("Basic"));
    log_level_combo_->addItem(QStringLiteral("Verbose"));
    log_level_combo_->addItem(QStringLiteral("Trace"));
    log_level_combo_->setCurrentIndex(kLogLevelBasic);
    log_level_combo_->setMinimumWidth(200);
    level_layout->addWidget(log_level_combo_);
    layout->addWidget(level_panel);

    // Rotation settings
    layout->addWidget(makeSectionLabel(QStringLiteral("Log Rotation"), content));
    auto* rot_panel = makePanel(content);
    auto* rot_layout = new QVBoxLayout(rot_panel);
    rot_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd,
                                   ui::theme::ExoSnapMetrics::kSpaceLg, ui::theme::ExoSnapMetrics::kSpaceMd);
    rot_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);

    auto* size_row = new QHBoxLayout();
    auto* size_label = new QLabel(QStringLiteral("Max file size (MB):"), rot_panel);
    size_label->setProperty("labelRole", "body");
    max_file_size_spin_ = new QSpinBox(rot_panel);
    max_file_size_spin_->setRange(1, 1000);
    max_file_size_spin_->setValue(10);
    max_file_size_spin_->setSuffix(QStringLiteral(" MB"));
    size_row->addWidget(size_label);
    size_row->addWidget(max_file_size_spin_);
    size_row->addStretch();
    rot_layout->addLayout(size_row);

    auto* count_row = new QHBoxLayout();
    auto* count_label = new QLabel(QStringLiteral("Max file count:"), rot_panel);
    count_label->setProperty("labelRole", "body");
    max_file_count_spin_ = new QSpinBox(rot_panel);
    max_file_count_spin_->setRange(1, 100);
    max_file_count_spin_->setValue(5);
    count_row->addWidget(count_label);
    count_row->addWidget(max_file_count_spin_);
    count_row->addStretch();
    rot_layout->addLayout(count_row);

    layout->addWidget(rot_panel);
    layout->addStretch();
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(open_btn, &QPushButton::clicked, this, &DiagnosticsPage::onOpenLogFolder);
    connect(copy_btn, &QPushButton::clicked, this, &DiagnosticsPage::onCopyLatestLogs);
    connect(bundle_btn, &QPushButton::clicked, this, &DiagnosticsPage::onExportDiagnosticsBundle);
    connect(log_level_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &DiagnosticsPage::onLogLevelChanged);
}

// --- Self-Test Tab ---

void DiagnosticsPage::buildSelfTestTab(QWidget* container) {
    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    selftest_content_ = new QWidget();
    selftest_layout_ = new QVBoxLayout(selftest_content_);
    selftest_layout_->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                                         ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    selftest_layout_->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

    selftest_layout_->addWidget(makeSectionLabel(QStringLiteral("Self-Test"), selftest_content_));
    selftest_layout_->addWidget(
        makeSubLabel(QStringLiteral("Validates core recording pipeline components without starting a full recording."),
                     selftest_content_));

    selftest_status_label_ = new QLabel(QStringLiteral("Status: Not run"), selftest_content_);
    selftest_status_label_->setProperty("labelRole", "body");
    selftest_layout_->addWidget(selftest_status_label_);

    selftest_run_btn_ = new QPushButton(QStringLiteral("Run Self-Test"), selftest_content_);
    selftest_run_btn_->setProperty("role", "primary");
    selftest_run_btn_->setMaximumWidth(200);
    selftest_layout_->addWidget(selftest_run_btn_);

    selftest_layout_->addWidget(makeHorizontalRule(selftest_content_));
    selftest_layout_->addWidget(makeSubLabel(
        QStringLiteral("Run a system check from the Overview tab or click Run Self-Test."), selftest_content_));
    selftest_layout_->addStretch();

    scroll->setWidget(selftest_content_);

    auto* root = new QVBoxLayout(container);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(selftest_run_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
}

void DiagnosticsPage::refreshSelfTest() {
    if (!selftest_layout_ || !selftest_content_)
        return;

    // Clear existing content below the run button and rule
    while (selftest_layout_->count() > 6) {
        QLayoutItem* child = selftest_layout_->takeAt(selftest_layout_->count() - 1);
        if (child->widget())
            delete child->widget();
        delete child;
    }

    diagnostics::SelfTestRunner runner;
    auto checklist = runner.Run();

    if (checklist.worst_severity() == diagnostics::DiagnosticSeverity::Pass) {
        selftest_status_label_->setText(QStringLiteral("Status: PASS"));
    } else if (checklist.has_notice) {
        selftest_status_label_->setText(QStringLiteral("Status: WARN"));
    }

    for (const auto& result : checklist.results) {
        auto* row = makePanel(selftest_content_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm,
                                       ui::theme::ExoSnapMetrics::kSpaceMd, ui::theme::ExoSnapMetrics::kSpaceSm);

        auto* icon_lbl = new QLabel(severityIcon(result.severity), row);
        icon_lbl->setProperty("labelRole", severityClass(result.severity));
        row_layout->addWidget(icon_lbl);

        auto* name_lbl = new QLabel(QString::fromStdString(result.title), row);
        name_lbl->setProperty("labelRole", "body");
        name_lbl->setMinimumWidth(200);
        row_layout->addWidget(name_lbl);

        auto* status_lbl = new QLabel(QString::fromStdString(result.summary), row);
        status_lbl->setProperty("labelRole",
                                result.severity == diagnostics::DiagnosticSeverity::Pass ? "statusGood" : "statusBad");
        row_layout->addWidget(status_lbl);

        auto* detail_lbl = new QLabel(QString::fromStdString(result.detail), row);
        detail_lbl->setProperty("labelRole", "subtle");
        detail_lbl->setWordWrap(true);
        row_layout->addWidget(detail_lbl, 1);

        selftest_layout_->addWidget(row);
    }
    selftest_layout_->addStretch();
}

// --- Overview refresh ---

void DiagnosticsPage::refreshOverview() {
    if (!data_ready_)
        return;

    diagnostics::CapabilitySummary caps = diagnostics::CapabilitySummary::FromCapabilitySet(caps_);
    diagnostics::RecommendationEngine engine(caps_, capability::UserRecorderConfig{}, 0, 0, true);
    auto recs = engine.Generate();

    diagnostics::DiagnosticChecklist combined;
    for (auto& r : caps.entries) {
        diagnostics::DiagnosticResult dr;
        dr.id = "cap." + r.label;
        dr.group = diagnostics::DiagnosticGroup::CapabilityProbe;
        dr.severity = r.available ? diagnostics::DiagnosticSeverity::Pass : diagnostics::DiagnosticSeverity::Notice;
        dr.title = r.label;
        dr.summary = r.value;
        dr.current_value = r.status;
        dr.timestamp = QDateTime::currentSecsSinceEpoch();
        combined.results.push_back(std::move(dr));
    }
    for (auto& r : recs.results) {
        combined.results.push_back(r);
    }

    // Count
    int blockers = 0, notices = 0, passes = 0;
    for (const auto& r : combined.results) {
        switch (r.severity) {
        case diagnostics::DiagnosticSeverity::Blocker:
            ++blockers;
            break;
        case diagnostics::DiagnosticSeverity::Notice:
            ++notices;
            break;
        case diagnostics::DiagnosticSeverity::Pass:
            ++passes;
            break;
        }
    }

    if (blockers > 0) {
        status_label_->setText(QStringLiteral("Overall status: BLOCKED — %1 blocker(s) found").arg(blockers));
    } else if (notices > 0) {
        status_label_->setText(QStringLiteral("Overall status: Ready with %1 notice(s)").arg(notices));
    } else {
        status_label_->setText(QStringLiteral("Overall status: Ready"));
    }

    last_check_label_->setText(QStringLiteral("Last check: %1")
                                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd MMM yyyy, hh:mm"))));

    summary_label_->setText(QStringLiteral("Capabilities: %1 entries checked, %2 recommendations.")
                                .arg(caps.entries.size())
                                .arg(recs.results.size()));

    blocker_count_->setText(QString::number(blockers));
    notice_count_->setText(QString::number(notices));
    pass_count_->setText(QString::number(passes));

    export_report_btn_->setEnabled(true);
}

// --- Log slots ---

void DiagnosticsPage::onOpenLogFolder() {
    QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDesktopServices::openUrl(QUrl::fromLocalFile(data_dir + QStringLiteral("/logs")));
}

void DiagnosticsPage::onCopyLatestLogs() {
    QString data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString log_path = data_dir + QStringLiteral("/logs/exosnap.log");
    QFileInfo fi(log_path);
    if (!fi.exists()) {
        QMessageBox::information(this, QStringLiteral("Copy Logs"),
                                 QStringLiteral("No log file found at:\n") + log_path);
        return;
    }
    QString dest = QFileDialog::getSaveFileName(this, QStringLiteral("Copy Log File"),
                                                QStandardPaths::writableLocation(QStandardPaths::DesktopLocation) +
                                                    QStringLiteral("/exosnap.log"),
                                                QStringLiteral("Log files (*.log);;All files (*)"));
    if (dest.isEmpty())
        return;
    if (!QFile::copy(log_path, dest)) {
        QMessageBox::warning(this, QStringLiteral("Copy Logs"), QStringLiteral("Failed to copy log file to:\n") + dest);
    } else {
        QMessageBox::information(this, QStringLiteral("Copy Logs"), QStringLiteral("Log file copied to:\n") + dest);
    }
}

void DiagnosticsPage::onExportDiagnosticsBundle() {
    QMessageBox::information(this, QStringLiteral("Diagnostics Bundle"),
                             QStringLiteral("Diagnostics bundle export is not yet available in this build."));
}

void DiagnosticsPage::onLogLevelChanged(int index) {
    Q_UNUSED(index);
    // Scaffold: log level persistence will be wired when AppSettingsStore supports it.
}

// --- MainWindow wiring ---

} // namespace exosnap
