#include "DiagnosticsPage.h"

#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/DiagnosticsPresentation.h"
#include "../diagnostics/RecommendationEngine.h"
#include "../diagnostics/SelfTestRunner.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include <capability/audio_ui_state.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>

namespace exosnap {

using M = ui::theme::ExoSnapMetrics;

namespace {

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

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceLg);

    // ── A: Readiness panel ────────────────────────────────────────────────────
    auto* readiness_panel = makePanel(content);
    auto* rl = new QVBoxLayout(readiness_panel);
    rl->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    rl->setSpacing(M::kSpaceSm);

    rl->addWidget(makeSubLabel(QStringLiteral("Diagnostics are designed to block invalid recording states early."),
                               readiness_panel));

    status_label_ = new QLabel(QStringLiteral("Overall status: Not checked"), readiness_panel);
    status_label_->setProperty("labelRole", "body");
    rl->addWidget(status_label_);

    last_check_label_ = new QLabel(QStringLiteral("Last check: —"), readiness_panel);
    last_check_label_->setProperty("labelRole", "subtle");
    rl->addWidget(last_check_label_);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(M::kSpaceSm);
    run_check_btn_ = new QPushButton(QStringLiteral("Run System && Pipeline Check"), readiness_panel);
    run_check_btn_->setProperty("role", "primary");
    export_report_btn_ = new QPushButton(QStringLiteral("Export Diagnostic Report"), readiness_panel);
    export_report_btn_->setProperty("role", "ghost");
    export_report_btn_->setEnabled(false);
    btn_row->addWidget(run_check_btn_);
    btn_row->addWidget(export_report_btn_);
    btn_row->addStretch();
    rl->addLayout(btn_row);

    summary_label_ = new QLabel(QStringLiteral("Run a check to see results."), readiness_panel);
    summary_label_->setProperty("labelRole", "muted");
    rl->addWidget(summary_label_);

    // Counts row
    auto* counts_row = new QHBoxLayout();
    counts_row->setSpacing(M::kSpaceXl);

    auto* blocker_panel = new QFrame(readiness_panel);
    blocker_panel->setProperty("panelRole", "compactRow");
    auto* bl = new QHBoxLayout(blocker_panel);
    bl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    blocker_count_ = new QLabel(QStringLiteral("0"), blocker_panel);
    blocker_count_->setProperty("labelRole", "countBlocker");
    auto* blocker_lbl = new QLabel(QStringLiteral("Blockers"), blocker_panel);
    blocker_lbl->setProperty("labelRole", "body");
    bl->addWidget(blocker_count_);
    bl->addWidget(blocker_lbl);
    counts_row->addWidget(blocker_panel);

    auto* notice_panel = new QFrame(readiness_panel);
    notice_panel->setProperty("panelRole", "compactRow");
    auto* nl = new QHBoxLayout(notice_panel);
    nl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    notice_count_ = new QLabel(QStringLiteral("0"), notice_panel);
    notice_count_->setProperty("labelRole", "countNotice");
    auto* notice_lbl = new QLabel(QStringLiteral("Notices"), notice_panel);
    notice_lbl->setProperty("labelRole", "body");
    nl->addWidget(notice_count_);
    nl->addWidget(notice_lbl);
    counts_row->addWidget(notice_panel);

    auto* pass_panel = new QFrame(readiness_panel);
    pass_panel->setProperty("panelRole", "compactRow");
    auto* pl = new QHBoxLayout(pass_panel);
    pl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    pass_count_ = new QLabel(QStringLiteral("0"), pass_panel);
    pass_count_->setProperty("labelRole", "countPass");
    auto* pass_lbl = new QLabel(QStringLiteral("Passes"), pass_panel);
    pass_lbl->setProperty("labelRole", "body");
    pl->addWidget(pass_count_);
    pl->addWidget(pass_lbl);
    counts_row->addWidget(pass_panel);

    counts_row->addStretch();
    rl->addLayout(counts_row);
    layout->addWidget(readiness_panel);

    // ── B: Top Issues ─────────────────────────────────────────────────────────
    layout->addWidget(makeHorizontalRule(content));
    layout->addWidget(makeSectionLabel(QStringLiteral("Top Issues"), content));
    layout->addWidget(makeSubLabel(
        QStringLiteral("Highest-priority blockers and notices for the current recording configuration."), content));

    issues_parent_ = new QWidget(content);
    overview_issues_layout_ = new QVBoxLayout(issues_parent_);
    overview_issues_layout_->setContentsMargins(0, 0, 0, 0);
    overview_issues_layout_->setSpacing(M::kSpaceSm);
    overview_issues_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate issue details."), issues_parent_));
    layout->addWidget(issues_parent_);

    // ── C: Technical Details ──────────────────────────────────────────────────
    layout->addWidget(makeHorizontalRule(content));
    layout->addWidget(makeSectionLabel(QStringLiteral("Technical Details"), content));

    // C1: Capabilities
    capabilities_content_ = new QWidget(content);
    capabilities_layout_ = new QVBoxLayout(capabilities_content_);
    capabilities_layout_->setContentsMargins(0, 0, 0, 0);
    capabilities_layout_->setSpacing(M::kSpaceXs);
    capabilities_layout_->addWidget(
        makeSectionLabel(QStringLiteral("Hardware & Software Capabilities"), capabilities_content_));
    capabilities_layout_->addWidget(makeSubLabel(
        QStringLiteral("Detected capabilities from the current system. Values marked unavailable are not selectable."),
        capabilities_content_));
    capabilities_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate this list."), capabilities_content_));
    layout->addWidget(capabilities_content_);
    layout->addSpacing(M::kSpaceMd);

    // C2: Configuration
    config_content_ = new QWidget(content);
    config_layout_ = new QVBoxLayout(config_content_);
    config_layout_->setContentsMargins(0, 0, 0, 0);
    config_layout_->setSpacing(M::kSpaceXs);
    config_layout_->addWidget(makeSectionLabel(QStringLiteral("Active Configuration"), config_content_));
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Active recording settings as currently configured in the app."), config_content_));
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate this list."), config_content_));
    layout->addWidget(config_content_);

    // ── D: Self-Test ──────────────────────────────────────────────────────────
    layout->addWidget(makeHorizontalRule(content));
    layout->addWidget(makeSectionLabel(QStringLiteral("Self-Test"), content));
    layout->addWidget(makeSubLabel(
        QStringLiteral("Validates core recording pipeline components without starting a full recording."), content));

    selftest_content_ = new QWidget(content);
    selftest_layout_ = new QVBoxLayout(selftest_content_);
    selftest_layout_->setContentsMargins(0, 0, 0, 0);
    selftest_layout_->setSpacing(M::kSpaceLg);

    selftest_status_label_ = new QLabel(QStringLiteral("Status: Not run"), selftest_content_);
    selftest_status_label_->setProperty("labelRole", "body");
    selftest_layout_->addWidget(selftest_status_label_); // item 0

    selftest_run_btn_ = new QPushButton(QStringLiteral("Run Self-Test"), selftest_content_);
    selftest_run_btn_->setProperty("role", "primary");
    selftest_run_btn_->setMaximumWidth(200);
    selftest_layout_->addWidget(selftest_run_btn_); // item 1

    selftest_layout_->addWidget(makeHorizontalRule(selftest_content_)); // item 2
    selftest_layout_->addWidget(makeSubLabel(                           // item 3
        QStringLiteral("Run a system check or click Run Self-Test."), selftest_content_));

    layout->addWidget(selftest_content_);

    // ── E: Logs redirect ──────────────────────────────────────────────────────
    layout->addWidget(makeHorizontalRule(content));

    auto* logs_card = makePanel(content);
    auto* ll = new QVBoxLayout(logs_card);
    ll->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    ll->setSpacing(M::kSpaceMd);
    ll->addWidget(makeSectionLabel(QStringLiteral("Application Logs"), logs_card));
    ll->addWidget(makeSubLabel(QStringLiteral("Raw application logs are available on the Logs page."), logs_card));
    auto* go_logs_btn = new QPushButton(QStringLiteral("Open Logs Page"), logs_card);
    go_logs_btn->setProperty("role", "ghost");
    go_logs_btn->setMaximumWidth(200);
    ll->addWidget(go_logs_btn);
    layout->addWidget(logs_card);

    layout->addStretch();

    constexpr int kMaxContentWidth = 1080;
    content->setMaximumWidth(kMaxContentWidth);
    auto* content_holder = new QWidget();
    auto* holder_layout = new QHBoxLayout(content_holder);
    holder_layout->setContentsMargins(0, 0, 0, 0);
    holder_layout->setSpacing(0);
    holder_layout->addStretch(1);
    holder_layout->addWidget(content);
    holder_layout->addStretch(1);
    scroll->setWidget(content_holder);
    root->addWidget(scroll);

    connect(run_check_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
    connect(export_report_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onExportReport);
    connect(selftest_run_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
    connect(go_logs_btn, &QPushButton::clicked, this, &DiagnosticsPage::navigateToLogsRequested);
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
    active_user_config_ = diagnostics::UserConfigFromSettings(output, video);
    capability::SettingsResolver resolver(caps_);
    profile_validation_ = resolver.ValidateConfig(active_user_config_);
    data_ready_ = true;

    refreshOverview();
    refreshSelfTest();
    refreshCapabilities();
    refreshConfiguration();
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
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceXs, M::kSpaceSm, M::kSpaceXs);
    row_layout->setSpacing(M::kSpaceMd);

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
    refreshSelfTest();
}

void DiagnosticsPage::onExportReport() {
    // Disabled until full report export is wired.
}

// --- Capabilities refresh ---

void DiagnosticsPage::refreshCapabilities() {
    if (!capabilities_layout_ || !capabilities_content_ || !data_ready_)
        return;

    QLayoutItem* child = nullptr;
    while ((child = capabilities_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    capabilities_layout_->addWidget(
        makeSectionLabel(QStringLiteral("Hardware & Software Capabilities"), capabilities_content_));
    capabilities_layout_->addWidget(makeSubLabel(
        QStringLiteral("Detected capabilities from the current system. Values marked unavailable are not selectable."),
        capabilities_content_));
    capabilities_layout_->addSpacing(M::kSpaceMd);

    auto* header_row = new QWidget(capabilities_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(M::kSpaceSm, 0, M::kSpaceSm, 0);
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

    for (const auto& entry : cap_summary_.entries) {
        capabilities_layout_->addWidget(makeInfoRow(QString::fromStdString(entry.label),
                                                    QString::fromStdString(entry.value),
                                                    QString::fromStdString(entry.status), capabilities_content_));
    }
}

// --- Configuration refresh ---

void DiagnosticsPage::refreshConfiguration() {
    if (!config_layout_ || !config_content_ || !data_ready_)
        return;

    QLayoutItem* child = nullptr;
    while ((child = config_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    config_layout_->addWidget(makeSectionLabel(QStringLiteral("Active Configuration"), config_content_));
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Active recording settings as currently configured in the app."), config_content_));
    config_layout_->addSpacing(M::kSpaceMd);

    auto* header_row = new QWidget(config_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(M::kSpaceSm, 0, M::kSpaceSm, 0);
    auto* h1 = new QLabel(QStringLiteral("Setting"), header_row);
    h1->setProperty("labelRole", "section");
    h1->setMinimumWidth(180);
    auto* h2 = new QLabel(QStringLiteral("Value"), header_row);
    h2->setProperty("labelRole", "section");
    header_layout->addWidget(h1);
    header_layout->addWidget(h2, 1);
    config_layout_->addWidget(header_row);
    config_layout_->addWidget(makeHorizontalRule(config_content_));

    for (const auto& entry : config_summary_.entries) {
        config_layout_->addWidget(makeInfoRow(QString::fromStdString(entry.label), QString::fromStdString(entry.value),
                                              QString(), config_content_));
    }
}

// --- Self-Test refresh ---

void DiagnosticsPage::refreshSelfTest() {
    if (!selftest_layout_ || !selftest_content_)
        return;

    // Preserve first 4 items (status label, run button, rule, hint); remove dynamic results.
    while (selftest_layout_->count() > 4) {
        QLayoutItem* child = selftest_layout_->takeAt(selftest_layout_->count() - 1);
        if (child->widget())
            delete child->widget();
        delete child;
    }

    diagnostics::SelfTestRunner runner;
    auto checklist = runner.Run();

    // Detect whether all non-passing results are scaffold probes not yet implemented.
    bool all_not_executed = true;
    for (const auto& r : checklist.results) {
        if (r.severity != diagnostics::DiagnosticSeverity::Pass &&
            r.detail.find("not executed in this build") == std::string::npos) {
            all_not_executed = false;
            break;
        }
    }

    if (checklist.worst_severity() == diagnostics::DiagnosticSeverity::Pass) {
        selftest_status_label_->setText(QStringLiteral("Status: PASS"));
    } else if (all_not_executed) {
        selftest_status_label_->setText(QStringLiteral("Status: Not run"));
    } else if (checklist.has_notice) {
        selftest_status_label_->setText(QStringLiteral("Status: WARN"));
    }

    for (const auto& result : checklist.results) {
        const bool is_not_executed = result.severity != diagnostics::DiagnosticSeverity::Pass &&
                                     result.detail.find("not executed in this build") != std::string::npos;

        auto* row = makePanel(selftest_content_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);

        auto* icon_lbl =
            new QLabel(is_not_executed ? QStringLiteral("\xe2\x80\x94") : severityIcon(result.severity), row);
        icon_lbl->setProperty("labelRole", is_not_executed ? "subtle" : severityClass(result.severity));
        row_layout->addWidget(icon_lbl);

        auto* name_lbl = new QLabel(QString::fromStdString(result.title), row);
        name_lbl->setProperty("labelRole", "body");
        name_lbl->setMinimumWidth(200);
        row_layout->addWidget(name_lbl);

        auto* status_lbl =
            new QLabel(is_not_executed ? QStringLiteral("Not run") : QString::fromStdString(result.summary), row);
        status_lbl->setProperty("labelRole", result.severity == diagnostics::DiagnosticSeverity::Pass ? "statusGood"
                                             : is_not_executed                                        ? "subtle"
                                                                                                      : "statusBad");
        row_layout->addWidget(status_lbl);

        auto* detail_lbl = new QLabel(QString::fromStdString(result.detail), row);
        detail_lbl->setProperty("labelRole", "subtle");
        detail_lbl->setWordWrap(true);
        row_layout->addWidget(detail_lbl, 1);

        selftest_layout_->addWidget(row);
    }
}

// --- Top Issues ---

void DiagnosticsPage::refreshTopIssues(const diagnostics::DiagnosticChecklist& recommendations, int total_notices,
                                       int total_blockers) {
    if (!overview_issues_layout_)
        return;

    QLayoutItem* child = nullptr;
    while ((child = overview_issues_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    int issue_count = 0;
    constexpr int kMaxIssues = 6;

    const auto add_issue_card = [&](diagnostics::DiagnosticSeverity severity, const QString& title,
                                    const QString& summary, const QString& action, const QString& detail) {
        if (issue_count >= kMaxIssues)
            return;

        auto* card = makePanel(issues_parent_);
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
        card_layout->setSpacing(M::kSpaceXs);

        auto* title_row = new QHBoxLayout();
        auto* icon_label = new QLabel(severityIcon(severity), card);
        icon_label->setProperty("labelRole", severityClass(severity));
        auto* title_label = new QLabel(title, card);
        title_label->setProperty("labelRole", "body");
        title_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
        title_row->addWidget(icon_label);
        title_row->addWidget(title_label, 1);
        card_layout->addLayout(title_row);

        auto* summary_label = new QLabel(summary, card);
        summary_label->setProperty("labelRole", "subtle");
        summary_label->setWordWrap(true);
        card_layout->addWidget(summary_label);

        if (!detail.trimmed().isEmpty()) {
            auto* detail_label = new QLabel(detail, card);
            detail_label->setProperty("labelRole", "subtle");
            detail_label->setWordWrap(true);
            card_layout->addWidget(detail_label);
        }

        if (!action.trimmed().isEmpty()) {
            auto* action_label = new QLabel(QStringLiteral("Action: ") + action, card);
            action_label->setProperty("labelRole", "muted");
            action_label->setWordWrap(true);
            card_layout->addWidget(action_label);
        }

        overview_issues_layout_->addWidget(card);
        ++issue_count;
    };

    if (!profile_validation_.succeeded) {
        for (const auto& invalid : profile_validation_.invalidity) {
            const QString field_display = QString::fromStdString(diagnostics::InvalidFieldDisplayName(invalid.field));
            const QString action_hint = QString::fromStdString(diagnostics::InvalidFieldActionHint(invalid.field));
            add_issue_card(diagnostics::DiagnosticSeverity::Blocker,
                           field_display + QStringLiteral(" is not supported"), QString::fromStdString(invalid.message),
                           action_hint, QString{});
        }
    }

    const bool has_profile_invalidity = !profile_validation_.invalidity.empty();
    const std::vector<diagnostics::DiagnosticResult> ordered_recommendations =
        diagnostics::BuildTopIssueRecommendations(recommendations, has_profile_invalidity);

    for (const auto& result : ordered_recommendations) {
        if (result.severity != diagnostics::DiagnosticSeverity::Blocker)
            continue;
        add_issue_card(result.severity, QString::fromStdString(result.title), QString::fromStdString(result.summary),
                       QString::fromStdString(result.recommendation), QString::fromStdString(result.detail));
    }

    for (const auto& warning : profile_validation_.warnings) {
        add_issue_card(diagnostics::DiagnosticSeverity::Notice, QStringLiteral("Configuration needs validation"),
                       QString::fromStdString(warning.message),
                       QStringLiteral("Run a short recording to validate quality on this machine."),
                       QStringLiteral("Code: %1").arg(QString::fromStdString(warning.code)));
    }

    if (!hotkeys_ok_ && hotkeys_summary_ != "None configured") {
        add_issue_card(diagnostics::DiagnosticSeverity::Notice, QStringLiteral("Global hotkeys are not active"),
                       QStringLiteral("Hotkeys are configured but not currently registered."),
                       QStringLiteral("Open the Hotkeys page and reapply the binding if shortcuts do not trigger."),
                       QStringLiteral("If the app just launched, this can clear once startup completes."));
    }

    for (const auto& result : ordered_recommendations) {
        if (result.severity == diagnostics::DiagnosticSeverity::Blocker)
            continue;
        add_issue_card(result.severity, QString::fromStdString(result.title), QString::fromStdString(result.summary),
                       QString::fromStdString(result.recommendation), QString::fromStdString(result.detail));
    }

    if (issue_count == 0) {
        if (total_blockers == 0 && total_notices > 0) {
            overview_issues_layout_->addWidget(makeSubLabel(
                QStringLiteral("No blockers. %1 informational notice(s) are listed in Technical Details below "
                               "and do not block the active recording configuration.")
                    .arg(total_notices),
                issues_parent_));
        } else {
            overview_issues_layout_->addWidget(
                makeSubLabel(QStringLiteral("No blockers or notices detected in the active recording configuration."),
                             issues_parent_));
        }
    }
}

// --- Overview refresh ---

void DiagnosticsPage::refreshOverview() {
    if (!data_ready_)
        return;

    diagnostics::RecommendationEngine engine(caps_, active_user_config_, 0, 0, profile_validation_.succeeded);
    auto recs = engine.Generate();

    diagnostics::DiagnosticChecklist combined;
    for (const auto& r : cap_summary_.entries) {
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

    QString summary = QStringLiteral("Capabilities: %1 entries checked, %2 recommendation(s).")
                          .arg(cap_summary_.entries.size())
                          .arg(recs.results.size());
    if (!profile_validation_.succeeded && !profile_validation_.invalidity.empty()) {
        summary += QStringLiteral(" Configuration compatibility blockers found.");
    }
    summary_label_->setText(summary);

    blocker_count_->setText(QString::number(blockers));
    notice_count_->setText(QString::number(notices));
    pass_count_->setText(QString::number(passes));
    refreshTopIssues(recs, notices, blockers);

    export_report_btn_->setEnabled(true);
}

} // namespace exosnap
