#include "DiagnosticsPage.h"

#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/DiagnosticsPresentation.h"
#include "../diagnostics/RecommendationEngine.h"
#include "../diagnostics/SelfTestRunner.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/widgets/LivePipelinePanel.h"
#include "../ui/widgets/PipelineFlow.h"
#include "../ui/widgets/PipelineStepCard.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include <capability/audio_ui_state.h>
#include <capability/resolver.h>
#include <capability/support_level.h>
#include <capability/user_config.h>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
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

// Issue-card tone maps onto the QSS issueTone / statTone tints.
QString severityTone(diagnostics::DiagnosticSeverity sev) {
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

QString collapseGlyph(bool open) {
    return open ? QString::fromUtf8("\xe2\x96\xbe  ")  // ▾
                : QString::fromUtf8("\xe2\x96\xb8  "); // ▸
}

QFrame* makeHorizontalRule(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::HLine);
    f->setProperty("frameRole", "sectionRuleLine");
    return f;
}

QLabel* makeTableHeader(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("labelRole", "tableHeader");
    return l;
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

    // ── A: Readiness / action header ──────────────────────────────────────────
    // A troubleshooting summary: a plain-language verdict plus the primary actions,
    // tinted by overall state, with three count tiles below.
    readiness_panel_ = makePanel(content);
    readiness_panel_->setProperty("panelRole", "readinessBanner");
    auto* rl = new QVBoxLayout(readiness_panel_);
    rl->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    rl->setSpacing(M::kSpaceSm);

    auto* head_row = new QHBoxLayout();
    head_row->setSpacing(M::kSpaceMd);

    auto* head_text = new QVBoxLayout();
    head_text->setSpacing(M::kSpaceXs);
    status_pill_ = new QLabel(QStringLiteral("NOT CHECKED"), readiness_panel_);
    status_pill_->setProperty("labelRole", "profileStatusBadge");
    status_pill_->setAlignment(Qt::AlignCenter);
    auto* pill_row = new QHBoxLayout();
    pill_row->setContentsMargins(0, 0, 0, 0);
    pill_row->addWidget(status_pill_);
    pill_row->addStretch();
    head_text->addLayout(pill_row);

    last_check_label_ = new QLabel(QStringLiteral("Last check: \xe2\x80\x94"), readiness_panel_);
    last_check_label_->setProperty("labelRole", "subtle");
    head_text->addWidget(last_check_label_);
    head_row->addLayout(head_text, 1);

    auto* btn_row = new QHBoxLayout();
    btn_row->setSpacing(M::kSpaceSm);
    run_check_btn_ = new QPushButton(QStringLiteral("Run Check"), readiness_panel_);
    run_check_btn_->setProperty("role", "primary");
    export_report_btn_ = new QPushButton(QStringLiteral("Export Report"), readiness_panel_);
    export_report_btn_->setProperty("role", "ghost");
    export_report_btn_->setEnabled(false);
    export_report_btn_->setToolTip(QStringLiteral("Diagnostic report export is planned for a future build."));
    btn_row->addWidget(run_check_btn_);
    btn_row->addWidget(export_report_btn_);
    head_row->addLayout(btn_row, 0);
    rl->addLayout(head_row);

    summary_label_ = new QLabel(QStringLiteral("Run a check to see whether this machine is set up to record well."),
                                readiness_panel_);
    summary_label_->setProperty("labelRole", "body");
    summary_label_->setWordWrap(true);
    rl->addWidget(summary_label_);

    // Count tiles
    auto* tiles_row = new QHBoxLayout();
    tiles_row->setSpacing(M::kSpaceMd);

    const auto makeStatTile = [&](const char* tone, const QString& tile_label, QFrame*& out_tile, QLabel*& out_num) {
        out_tile = new QFrame(readiness_panel_);
        out_tile->setProperty("panelRole", "statTile");
        out_tile->setProperty("statTone", tone);
        auto* tl = new QVBoxLayout(out_tile);
        tl->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
        tl->setSpacing(2);
        out_num = new QLabel(QStringLiteral("0"), out_tile);
        out_num->setProperty("labelRole", "statTileNum");
        out_num->setProperty("statTone", tone);
        auto* lbl = new QLabel(tile_label, out_tile);
        lbl->setProperty("labelRole", "statTileLabel");
        tl->addWidget(out_num);
        tl->addWidget(lbl);
        tiles_row->addWidget(out_tile, 1);
    };

    makeStatTile("blocker", QStringLiteral("Blockers"), blocker_tile_, blocker_count_);
    makeStatTile("notice", QStringLiteral("Notices"), notice_tile_, notice_count_);
    makeStatTile("pass", QStringLiteral("Passes"), pass_tile_, pass_count_);
    rl->addSpacing(M::kSpaceXs);
    rl->addLayout(tiles_row);
    layout->addWidget(readiness_panel_);

    // ── B: Capture pipeline (the page's visual center) ─────────────────────────
    // Per-stage readiness from real capability checks. The static flow shows which
    // stages are available; live per-frame telemetry is rendered in the LIVE PIPELINE
    // section below. No stage card shows fabricated latency / queue / throughput.
    auto* pipeline_header = new ui::widgets::SectionRuleHeader(QStringLiteral("CAPTURE PIPELINE"), content);
    pipeline_header->setMeta(QStringLiteral("Static checks"));
    layout->addWidget(pipeline_header);
    layout->addWidget(makeSubLabel(
        QStringLiteral("Per-stage availability for the active recording configuration. Live per-frame latency, queue "
                       "depth, drops and throughput are shown in the Live pipeline section below while recording."),
        content));

    pipeline_flow_ = new ui::widgets::PipelineFlow(content);
    layout->addWidget(pipeline_flow_);

    auto* pipeline_caption =
        new QLabel(QStringLiteral("Stage status reflects static availability checks. Live timing is below."), content);
    pipeline_caption->setProperty("labelRole", "pipelineCaption");
    pipeline_caption->setWordWrap(true);
    layout->addWidget(pipeline_caption);

    // ── B2: Live pipeline telemetry (real runtime metrics while recording) ──────
    auto* live_header = new ui::widgets::SectionRuleHeader(QStringLiteral("LIVE PIPELINE"), content);
    live_header->setMeta(QStringLiteral("Live telemetry"));
    layout->addWidget(live_header);
    layout->addWidget(makeSubLabel(
        QStringLiteral("Real low-overhead runtime metrics for the active recording, updated ~5×/second. Metrics that "
                       "cannot be measured are shown as Unavailable, never as zero."),
        content));
    live_pipeline_panel_ = new ui::widgets::LivePipelinePanel(content);
    layout->addWidget(live_pipeline_panel_);

    // ── C: Recommendations (actionable cards based on real detected issues) ─────
    auto* issues_header = new ui::widgets::SectionRuleHeader(QStringLiteral("RECOMMENDATIONS"), content);
    layout->addWidget(issues_header);
    layout->addWidget(makeSubLabel(
        QStringLiteral("Highest-priority blockers and notices for the current recording configuration."), content));

    issues_parent_ = new QWidget(content);
    overview_issues_layout_ = new QVBoxLayout(issues_parent_);
    overview_issues_layout_->setContentsMargins(0, 0, 0, 0);
    overview_issues_layout_->setSpacing(M::kSpaceSm);
    overview_issues_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate issue details."), issues_parent_));
    layout->addWidget(issues_parent_);

    // ── D: Capability matrix (real probes, visible but secondary) ──────────────
    capabilities_header_ = new ui::widgets::SectionRuleHeader(QStringLiteral("CAPABILITY MATRIX"), content);
    capabilities_header_->setMeta(QStringLiteral("Real probes"));
    layout->addWidget(capabilities_header_);
    layout->addWidget(makeSubLabel(
        QStringLiteral("Encoders, muxers and audio paths probed on this machine. Unavailable items are simply not "
                       "selectable — they never block a recording."),
        content));

    auto* cap_panel = makePanel(content);
    auto* cap_panel_layout = new QVBoxLayout(cap_panel);
    cap_panel_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    cap_panel_layout->setSpacing(0);
    capabilities_content_ = new QWidget(cap_panel);
    capabilities_layout_ = new QVBoxLayout(capabilities_content_);
    capabilities_layout_->setContentsMargins(0, 0, 0, 0);
    capabilities_layout_->setSpacing(0);
    capabilities_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate this list."), capabilities_content_));
    cap_panel_layout->addWidget(capabilities_content_);
    layout->addWidget(cap_panel);

    // ── E: Active configuration (collapsed reference) ──────────────────────────
    config_content_ = makeCollapsibleSection(QStringLiteral("Active configuration"),
                                             QStringLiteral("Recording settings as currently configured in the app."),
                                             content, config_toggle_);
    config_layout_ = static_cast<QVBoxLayout*>(config_content_->layout());
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate this list."), config_content_));
    layout->addWidget(config_toggle_->parentWidget());

    // ── D: Self-Test ──────────────────────────────────────────────────────────
    auto* selftest_header = new ui::widgets::SectionRuleHeader(QStringLiteral("SELF-TEST"), content);
    layout->addWidget(selftest_header);
    layout->addWidget(makeSubLabel(
        QStringLiteral("Validates core recording pipeline components without starting a full recording."), content));

    selftest_content_ = new QWidget(content);
    selftest_layout_ = new QVBoxLayout(selftest_content_);
    selftest_layout_->setContentsMargins(0, 0, 0, 0);
    selftest_layout_->setSpacing(M::kSpaceSm);

    auto* selftest_action_row = new QHBoxLayout();
    selftest_action_row->setSpacing(M::kSpaceMd);
    selftest_status_label_ = new QLabel(QStringLiteral("Status: Not run"), selftest_content_);
    selftest_status_label_->setProperty("labelRole", "body");
    selftest_run_btn_ = new QPushButton(QStringLiteral("Run Self-Test"), selftest_content_);
    selftest_run_btn_->setProperty("role", "ghost");
    selftest_run_btn_->setMaximumWidth(200);
    selftest_action_row->addWidget(selftest_status_label_, 1);
    selftest_action_row->addWidget(selftest_run_btn_, 0);
    selftest_layout_->addLayout(selftest_action_row); // item 0

    selftest_layout_->addWidget( // item 1
        makeSubLabel(QStringLiteral("Run a system check or click Run Self-Test."), selftest_content_));

    layout->addWidget(selftest_content_);

    // ── F: Logs redirect ───────────────────────────────────────────────────────
    auto* logs_card = makePanel(content);
    logs_card->setProperty("panelRole", "note");
    auto* ll = new QHBoxLayout(logs_card);
    ll->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    ll->setSpacing(M::kSpaceMd);
    auto* logs_text = new QVBoxLayout();
    logs_text->setSpacing(2);
    auto* logs_title = new QLabel(QStringLiteral("Application Logs"), logs_card);
    logs_title->setProperty("labelRole", "cardTitle");
    logs_text->addWidget(logs_title);
    logs_text->addWidget(
        makeSubLabel(QStringLiteral("Need the raw event stream behind these checks? Open the Logs page."), logs_card));
    ll->addLayout(logs_text, 1);
    auto* go_logs_btn = new QPushButton(QStringLiteral("Open Logs Page"), logs_card);
    go_logs_btn->setProperty("role", "ghost");
    ll->addWidget(go_logs_btn, 0, Qt::AlignVCenter);
    layout->addWidget(logs_card);

    layout->addStretch();

    content->setMaximumWidth(1320);
    {
        auto* centering_host = new QWidget();
        auto* ch = new QHBoxLayout(centering_host);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->addStretch(1);
        ch->addWidget(content, 0);
        ch->addStretch(1);
        scroll->setWidget(centering_host);
    }
    root->addWidget(scroll);

    connect(run_check_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
    connect(export_report_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onExportReport);
    connect(selftest_run_btn_, &QPushButton::clicked, this, &DiagnosticsPage::onRunCheck);
    connect(go_logs_btn, &QPushButton::clicked, this, &DiagnosticsPage::navigateToLogsRequested);

    // Seed the pipeline with honest "run a check" placeholders (no fake metrics).
    refreshPipeline();
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
    refreshPipeline();
}

void DiagnosticsPage::applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    if (live_pipeline_panel_ == nullptr) {
        return;
    }
    live_pipeline_panel_->applySnapshot(snapshot);
}

// --- Helpers ---

QLabel* DiagnosticsPage::makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "subtitle");
    l->setWordWrap(true);
    return l;
}

QFrame* DiagnosticsPage::makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

QWidget* DiagnosticsPage::makeCollapsibleSection(const QString& title, const QString& subtitle, QWidget* parent,
                                                 QToolButton*& out_toggle) {
    auto* wrap = new QWidget(parent);
    auto* wl = new QVBoxLayout(wrap);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(M::kSpaceXs);

    auto* toggle = new QToolButton(wrap);
    toggle->setProperty("role", "collapseHead");
    toggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toggle->setCheckable(true);
    toggle->setChecked(false);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toggle->setText(collapseGlyph(false) + title);
    wl->addWidget(toggle);

    auto* body = new QWidget(wrap);
    body->setVisible(false);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(M::kSpaceXs, M::kSpaceSm, M::kSpaceXs, M::kSpaceSm);
    body_layout->setSpacing(M::kSpaceXs);

    // #14: subtitle goes inside the collapsible body, not outside it.
    if (!subtitle.trimmed().isEmpty()) {
        auto* sub = new QLabel(subtitle, body);
        sub->setProperty("labelRole", "collapseSub");
        sub->setWordWrap(true);
        body_layout->addWidget(sub);
    }

    wl->addWidget(body);

    connect(toggle, &QToolButton::toggled, this, [toggle, body](bool on) {
        body->setVisible(on);
        // Preserve the label (incl. any count suffix); only flip the 3-char glyph prefix.
        toggle->setText(collapseGlyph(on) + toggle->text().mid(3));
    });

    out_toggle = toggle;
    return body;
}

QWidget* DiagnosticsPage::makeInfoRow(const QString& label, const QString& value, const QString& status,
                                      QWidget* parent, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("diagTableRow"));
    row->setProperty("firstRow", first_row);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
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

void DiagnosticsPage::setReadinessState(const QString& state) {
    const auto repolish = [](QWidget* w) {
        if (!w)
            return;
        w->style()->unpolish(w);
        w->style()->polish(w);
    };
    const bool tinted =
        state == QStringLiteral("ready") || state == QStringLiteral("warn") || state == QStringLiteral("blocked");
    if (readiness_panel_) {
        readiness_panel_->setProperty("stateRole", tinted ? QVariant(state) : QVariant());
        repolish(readiness_panel_);
    }
    if (status_pill_) {
        status_pill_->setProperty("stateRole", tinted ? QVariant(state) : QVariant());
        repolish(status_pill_);
    }
}

void DiagnosticsPage::onRunCheck() {
    status_pill_->setText(QStringLiteral("CHECKING"));
    setReadinessState(QStringLiteral("checking"));
    last_check_label_->setText(QStringLiteral("Last check: running..."));
    summary_label_->setText(QStringLiteral("Check in progress."));

    if (!data_ready_) {
        status_pill_->setText(QStringLiteral("NO DATA"));
        setReadinessState(QStringLiteral("neutral"));
        last_check_label_->setText(QStringLiteral("Last check: \xe2\x80\x94"));
        summary_label_->setText(QStringLiteral("Diagnostic data has not been loaded. Open the Record page first."));
        return;
    }

    refreshOverview();
    refreshSelfTest();
    refreshPipeline();
}

void DiagnosticsPage::onExportReport() {
    // Disabled/planned: report export is not wired yet (button stays disabled).
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

    auto* header_row = new QWidget(capabilities_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(M::kSpaceSm, 0, M::kSpaceSm, 0);
    header_layout->setSpacing(M::kSpaceMd);
    auto* h1 = makeTableHeader(QStringLiteral("Feature"), header_row);
    h1->setMinimumWidth(180);
    auto* h2 = makeTableHeader(QStringLiteral("Detected Value"), header_row);
    auto* h3 = makeTableHeader(QStringLiteral("Status"), header_row);
    header_layout->addWidget(h1);
    header_layout->addWidget(h2, 1);
    header_layout->addWidget(h3);
    capabilities_layout_->addWidget(header_row);
    capabilities_layout_->addWidget(makeHorizontalRule(capabilities_content_));

    bool first = true;
    for (const auto& entry : cap_summary_.entries) {
        capabilities_layout_->addWidget(
            makeInfoRow(QString::fromStdString(entry.label), QString::fromStdString(entry.value),
                        QString::fromStdString(entry.status), capabilities_content_, first));
        first = false;
    }

    if (capabilities_header_)
        capabilities_header_->setMeta(QStringLiteral("%1 checks").arg(cap_summary_.entries.size()));
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

    auto* header_row = new QWidget(config_content_);
    auto* header_layout = new QHBoxLayout(header_row);
    header_layout->setContentsMargins(M::kSpaceSm, 0, M::kSpaceSm, 0);
    header_layout->setSpacing(M::kSpaceMd);
    auto* h1 = makeTableHeader(QStringLiteral("Setting"), header_row);
    h1->setMinimumWidth(180);
    auto* h2 = makeTableHeader(QStringLiteral("Value"), header_row);
    header_layout->addWidget(h1);
    header_layout->addWidget(h2, 1);
    config_layout_->addWidget(header_row);
    config_layout_->addWidget(makeHorizontalRule(config_content_));

    bool first = true;
    for (const auto& entry : config_summary_.entries) {
        config_layout_->addWidget(makeInfoRow(QString::fromStdString(entry.label), QString::fromStdString(entry.value),
                                              QString(), config_content_, first));
        first = false;
    }

    if (config_toggle_)
        config_toggle_->setText(collapseGlyph(config_toggle_->isChecked()) + QStringLiteral("Active configuration"));
}

// --- Self-Test refresh ---

void DiagnosticsPage::refreshSelfTest() {
    if (!selftest_layout_ || !selftest_content_)
        return;

    // Preserve the first two items (action row, hint); remove dynamic result rows.
    while (selftest_layout_->count() > 2) {
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

        auto* row = new QFrame(selftest_content_);
        row->setProperty("panelRole", "selfTestRow");
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
        row_layout->setSpacing(M::kSpaceMd);

        auto* icon_lbl =
            new QLabel(is_not_executed ? QStringLiteral("\xe2\x80\x94") : severityIcon(result.severity), row);
        icon_lbl->setProperty("labelRole", is_not_executed ? "subtle" : severityClass(result.severity));
        row_layout->addWidget(icon_lbl);

        auto* name_lbl = new QLabel(QString::fromStdString(result.title), row);
        name_lbl->setProperty("labelRole", "selfTestTitle");
        name_lbl->setMinimumWidth(180);
        row_layout->addWidget(name_lbl);

        auto* status_lbl =
            new QLabel(is_not_executed ? QStringLiteral("Not run") : QString::fromStdString(result.summary), row);
        status_lbl->setProperty("labelRole", result.severity == diagnostics::DiagnosticSeverity::Pass ? "statusGood"
                                             : is_not_executed                                        ? "subtle"
                                                                                                      : "statusBad");
        row_layout->addWidget(status_lbl);

        auto* detail_lbl = new QLabel(QString::fromStdString(result.detail), row);
        detail_lbl->setProperty("labelRole", "selfTestDetail");
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
        card->setProperty("panelRole", "issueCard");
        card->setProperty("issueTone", severityTone(severity));
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
        card_layout->setSpacing(M::kSpaceXs);

        auto* title_row = new QHBoxLayout();
        title_row->setSpacing(M::kSpaceSm);
        auto* icon_label = new QLabel(severityIcon(severity), card);
        icon_label->setProperty("labelRole", severityClass(severity));
        auto* title_label = new QLabel(title, card);
        title_label->setProperty("labelRole", "issueTitle");
        title_label->setWordWrap(true);
        title_row->addWidget(icon_label, 0, Qt::AlignTop);
        title_row->addWidget(title_label, 1);
        card_layout->addLayout(title_row);

        auto* summary_label = new QLabel(summary, card);
        summary_label->setProperty("labelRole", "issueDesc");
        summary_label->setWordWrap(true);
        card_layout->addWidget(summary_label);

        if (!detail.trimmed().isEmpty()) {
            auto* detail_label = new QLabel(detail, card);
            detail_label->setProperty("labelRole", "issueMeta");
            detail_label->setWordWrap(true);
            card_layout->addWidget(detail_label);
        }

        if (!action.trimmed().isEmpty()) {
            auto* action_label = new QLabel(QStringLiteral("Action: ") + action, card);
            action_label->setProperty("labelRole", "issueMeta");
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
                QStringLiteral("No blockers. %1 informational notice(s) are listed in the capability matrix below "
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
        // #10: proper pluralization
        const QString blocker_word = (blockers == 1) ? QStringLiteral("blocker") : QStringLiteral("blockers");
        status_pill_->setText(QStringLiteral("BLOCKED \xc2\xb7 %1").arg(blockers));
        setReadinessState(QStringLiteral("blocked"));
        summary_label_->setText(QStringLiteral("%1 %2 must be resolved before recording. See Top Issues below.")
                                    .arg(blockers)
                                    .arg(blocker_word));
    } else if (notices > 0) {
        // #10: proper pluralization
        const QString notice_word = (notices == 1) ? QStringLiteral("NOTICE") : QStringLiteral("NOTICES");
        const QString item_word = (notices == 1) ? QStringLiteral("item") : QStringLiteral("items");
        status_pill_->setText(QStringLiteral("READY \xc2\xb7 %1 %2").arg(notices).arg(notice_word));
        setReadinessState(QStringLiteral("warn"));
        summary_label_->setText(QStringLiteral("No blockers \xe2\x80\x94 you can record now. %1 %2 could be better.")
                                    .arg(notices)
                                    .arg(item_word));
    } else {
        status_pill_->setText(QStringLiteral("READY"));
        setReadinessState(QStringLiteral("ready"));
        summary_label_->setText(QStringLiteral("All checks passed. This machine is set up to record well."));
    }

    last_check_label_->setText(QStringLiteral("Last check: %1")
                                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd MMM yyyy, hh:mm"))));

    blocker_count_->setText(QString::number(blockers));
    notice_count_->setText(QString::number(notices));
    pass_count_->setText(QString::number(passes));

    // #04: Tint tiles conditionally — count==0 → neutral (no alarm colour at zero).
    const auto setTileActive = [](QFrame* tile, QLabel* num, const char* active_tone, bool active) {
        if (!tile || !num)
            return;
        const QString tone = active ? QString::fromLatin1(active_tone) : QStringLiteral("zero");
        tile->setProperty("statTone", tone);
        num->setProperty("statTone", tone);
        // Also update the sibling statTileLabel so it dims at zero.
        for (auto* lbl : tile->findChildren<QLabel*>()) {
            if (lbl->property("labelRole").toString() == QLatin1String("statTileLabel")) {
                lbl->setProperty("statTone", tone);
                lbl->style()->unpolish(lbl);
                lbl->style()->polish(lbl);
                lbl->update();
            }
        }
        tile->style()->unpolish(tile);
        tile->style()->polish(tile);
        num->style()->unpolish(num);
        num->style()->polish(num);
        tile->update();
        num->update();
    };
    setTileActive(blocker_tile_, blocker_count_, "blocker", blockers > 0);
    setTileActive(notice_tile_, notice_count_, "notice", notices > 0);
    // Pass tile: always neutral (green tint is fine at any count)
    setTileActive(pass_tile_, pass_count_, "pass", passes > 0);

    refreshTopIssues(recs, notices, blockers);
}

// --- Pipeline refresh ---
//
// Maps the canonical capture-pipeline steps onto real capability probes where
// one exists (Encoder/Muxer/Disk) and leaves probe-less internal stages
// (Source Capture/Frame Queue/Compositor) honestly Planned. No live per-frame
// timing exists yet, so no step ever shows fabricated latency/queue/throughput.
void DiagnosticsPage::refreshPipeline() {
    if (!pipeline_flow_)
        return;

    using Status = ui::widgets::PipelineStepCard::Status;

    // Internal stages without a runtime probe: honestly Planned, never faked.
    pipeline_flow_->setStepStatus(0, Status::Planned, QStringLiteral("Capture frame timing is not instrumented yet."));
    pipeline_flow_->setStepStatus(1, Status::Planned, QStringLiteral("Frame-queue depth is not instrumented yet."));
    pipeline_flow_->setStepStatus(2, Status::Planned, QStringLiteral("Compositor timing is not instrumented yet."));

    if (!data_ready_) {
        pipeline_flow_->setStepStatus(3, Status::Planned, QStringLiteral("Run a check to probe the encoder."));
        pipeline_flow_->setStepStatus(4, Status::Planned, QStringLiteral("Run a check to probe the muxer."));
        pipeline_flow_->setStepStatus(5, Status::Planned, QStringLiteral("Run a check to probe the output path."));
        return;
    }

    // Encoder — real video-codec selectability for the active configuration.
    const bool encoder_ok = capability::IsSelectable(caps_.QueryVideoCodec(active_user_config_.video_codec).level);
    pipeline_flow_->setStepStatus(
        3, encoder_ok ? Status::Ok : Status::Unavailable,
        encoder_ok ? QStringLiteral("Selected video encoder is available. Live encoder load is not measured.")
                   : QStringLiteral("Selected video codec is not available on this system."));

    // Muxer — real container selectability.
    const bool muxer_ok = capability::IsSelectable(caps_.QueryContainer(active_user_config_.container).level);
    pipeline_flow_->setStepStatus(
        4, muxer_ok ? Status::Ok : Status::Unavailable,
        muxer_ok ? QStringLiteral("Selected container muxer is available. Write throughput is not measured.")
                 : QStringLiteral("Selected container is not available on this system."));

    // Disk — real temp-directory writability probe (no live throughput).
    const bool disk_ok = diagnostics::SelfTestRunner::CheckOutputPathWritable(settings_path_).passed;
    pipeline_flow_->setStepStatus(5, disk_ok ? Status::Ok : Status::Unavailable,
                                  disk_ok
                                      ? QStringLiteral("Output path is writable. Live disk throughput is not measured.")
                                      : QStringLiteral("Output path is not writable."));
}

} // namespace exosnap
