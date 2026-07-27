#include "DiagnosticsPage.h"

#include "../diagnostics/ConfigSummary.h"
#include "../diagnostics/DiagnosticsPresentation.h"
#include "../diagnostics/DiskSpaceProvider.h"
#include "../diagnostics/FilesystemProvider.h"
#include "../diagnostics/RecommendationEngine.h"
#include "../diagnostics/SelfTestRunner.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../services/TargetDisplayFacts.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/LucideIcon.h"
#include "../ui/widgets/ElevationLock.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/LivePipelinePanel.h"
#include "../ui/widgets/PipelineFlow.h"
#include "../ui/widgets/PipelineStepCard.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include "../ui/widgets/TipChip.h"
#include <capability/audio_ui_state.h>
#include <capability/resolver.h>
#include <capability/runtime_snapshot.h>
#include <capability/support_level.h>
#include <capability/user_config.h>

#include <windows.h>

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <QProgressBar>
#include <QStorageInfo>

#include <algorithm>
#include <cmath>

namespace exosnap {

using M = ui::theme::ExoSnapMetrics;
using Pal = ui::theme::ExoSnapPalette;

namespace {

// True when the capture target's hosting display currently has Windows HDR ON.
// Monitor targets use their HMONITOR; window targets (WGC) resolve their hosting
// monitor via MonitorFromWindow — so a window on an HDR display now gets the same
// HDR-blocker/expert gating as a monitor target (see FindTargetDisplayFacts).
static bool SelectedTargetHdrActive(const std::optional<recorder_core::CaptureTarget>& target,
                                    const capability::CapabilitySet& caps) {
    if (!target.has_value()) {
        return false;
    }
    const capability::DisplayHdrFacts* facts = FindTargetDisplayFacts(*target, caps.runtime.displays);
    return facts != nullptr && facts->hdr_active;
}

static recorder_core::PresentMode ToSnapshotMode(diagnostics::PresentMode m) noexcept {
    switch (m) {
    case diagnostics::PresentMode::Composed:
        return recorder_core::PresentMode::Composed;
    case diagnostics::PresentMode::IndependentFlip:
        return recorder_core::PresentMode::IndependentFlip;
    case diagnostics::PresentMode::ExclusiveFullscreen:
        return recorder_core::PresentMode::ExclusiveFullscreen;
    default:
        return recorder_core::PresentMode::Unknown;
    }
}

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
        return QString::fromUtf8("\xe2\x9c\x93");
    case diagnostics::DiagnosticSeverity::Notice:
        return QString::fromUtf8("\xe2\x9a\xa0");
    case diagnostics::DiagnosticSeverity::Blocker:
        return QString::fromUtf8("\xe2\x9c\x97");
    }
    return QStringLiteral("?");
}

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

// True for checks whose measurement requires elevated present-path telemetry
// (PresentMon / DPC-ISR ETW). Drives the "Elev" lock badge on the entry card so
// the user knows the diagnosis came from the elevated baseline (diag-model.jsx).
bool needsElevation(const std::string& id) {
    return id.rfind("rec.present.", 0) == 0 || id.rfind("rec.dpc.", 0) == 0;
}

int fixKind(const diagnostics::FixAction& fa) {
    switch (fa.safety) {
    case diagnostics::FixAction::Safety::Auto:
        return 0;
    case diagnostics::FixAction::Safety::Assisted:
        return 1;
    case diagnostics::FixAction::Safety::External:
        return 2;
    }
    return 1;
}

QString humanBytes(uint64_t bytes) {
    const double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1024.0)
        return QString::number(gb / 1024.0, 'f', 1) + QStringLiteral(" TB");
    if (gb >= 10.0)
        return QString::number(gb, 'f', 0) + QStringLiteral(" GB");
    return QString::number(gb, 'f', 1) + QStringLiteral(" GB");
}

} // namespace

DiagnosticsPage::DiagnosticsPage(QWidget* parent) : QWidget(parent) {
    const QString dash = QString::fromUtf8("\xe2\x80\x94");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Slim toolbar — Expert toggle mirrors Settings (suite-diag2.jsx:411) ──────
    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("diagToolbar"));
    auto* tl = new QHBoxLayout(toolbar);
    tl->setContentsMargins(M::kSpaceXl, M::kSpaceSm, M::kSpaceXl, M::kSpaceSm);
    tl->setSpacing(M::kSpaceMd);
    auto* toolbar_kicker = new QLabel(QStringLiteral("DIAGNOSTICS"), toolbar);
    toolbar_kicker->setProperty("labelRole", "toolbarKicker");
    tl->addWidget(toolbar_kicker);
    mode_caption_ = new QLabel(QStringLiteral("\xc2\xb7 Simple"), toolbar);
    mode_caption_->setProperty("labelRole", "subtle");
    tl->addWidget(mode_caption_);
    tl->addStretch(1);
    // The primary Run-check control + last-check timestamp live in the verdict hero
    // band below (readiness dashboard header), not the toolbar. The toolbar keeps the
    // page identity, the support-bundle export (Expert), and the Expert toggle.
    export_report_btn_ = new QPushButton(QStringLiteral("Create support bundle"), toolbar);
    export_report_btn_->setProperty("role", "ghost");
    export_report_btn_->setProperty("size", "sm");
    export_report_btn_->setToolTip(QStringLiteral("Create a diagnostic package to share with support"));
    tl->addWidget(export_report_btn_, 0, Qt::AlignVCenter);
    tl->addSpacing(M::kSpaceMd);
    expert_mode_label_ = new QLabel(QStringLiteral("Expert mode"), toolbar);
    expert_mode_label_->setObjectName(QStringLiteral("diagExpertModeLabel"));
    expert_mode_label_->setProperty("labelRole", "muted");
    expert_mode_label_->setProperty("expertOn", false);
    tl->addWidget(expert_mode_label_, 0, Qt::AlignVCenter);
    expert_toggle_ = new ui::widgets::ExoToggle(toolbar);
    expert_toggle_->setObjectName(QStringLiteral("diagExpertModeToggleBtn"));
    expert_toggle_->setOn(false);
    tl->addWidget(expert_toggle_, 0, Qt::AlignVCenter);
    root->addWidget(toolbar);

    auto* toolbar_rule = makeHorizontalRule(this);
    root->addWidget(toolbar_rule);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceLg);

    // ── Verdict hero — a top-anchored readiness-dashboard HEADER BAND (slice-5
    // redesign, deliberate divergence from suite-diag2.jsx's centred CompactVerdict):
    // glyph chip + headline + subline on the left, the primary Run-check control and
    // last-check timestamp on the right. Panel chrome (readinessBanner) tints calmly
    // by verdict state. The old status pill object stays alive (its many writers keep
    // working) but is hidden; the headline carries the verdict now.
    readiness_panel_ = new QFrame(content);
    readiness_panel_->setProperty("panelRole", "readinessBanner");
    readiness_panel_->setObjectName(QStringLiteral("diagVerdictBand"));
    auto* rl = new QHBoxLayout(readiness_panel_);
    rl->setContentsMargins(M::kSpaceLg, M::kSpaceLg, M::kSpaceLg, M::kSpaceLg);
    rl->setSpacing(M::kSpaceMd + 3);

    verdict_glyph_ = new QLabel(readiness_panel_);
    verdict_glyph_->setObjectName(QStringLiteral("diagVerdictGlyph"));
    verdict_glyph_->setProperty("verdictTone", "neutral");
    verdict_glyph_->setFixedSize(42, 42);
    verdict_glyph_->setAlignment(Qt::AlignCenter);
    rl->addWidget(verdict_glyph_, 0, Qt::AlignVCenter);

    auto* verdict_text = new QVBoxLayout();
    verdict_text->setSpacing(M::kSpaceXs);
    verdict_headline_ = new QLabel(QStringLiteral("Not checked yet"), readiness_panel_);
    verdict_headline_->setProperty("labelRole", "diagVerdictHeadline");
    verdict_text->addWidget(verdict_headline_);
    summary_label_ = new QLabel(QStringLiteral("Run a check to see whether this machine is set up to record well."),
                                readiness_panel_);
    summary_label_->setProperty("labelRole", "diagVerdictSub");
    summary_label_->setWordWrap(true);
    verdict_text->addWidget(summary_label_);
    rl->addLayout(verdict_text, 1);
    rl->addSpacing(M::kSpaceMd);

    // Right rail of the band: last-check timestamp above the primary Run-check button.
    auto* verdict_actions = new QVBoxLayout();
    verdict_actions->setSpacing(M::kSpaceSm);
    verdict_actions->setAlignment(Qt::AlignRight);
    last_check_label_ = new QLabel(QString::fromUtf8("Last check: \xe2\x80\x94"), readiness_panel_);
    last_check_label_->setProperty("labelRole", "subtle");
    last_check_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    verdict_actions->addWidget(last_check_label_, 0, Qt::AlignRight);
    run_check_btn_ = new QPushButton(QStringLiteral("Run Check"), readiness_panel_);
    run_check_btn_->setProperty("role", "primary");
    run_check_btn_->setProperty("size", "sm");
    verdict_actions->addWidget(run_check_btn_, 0, Qt::AlignRight);
    rl->addLayout(verdict_actions, 0);

    status_pill_ = new QLabel(QStringLiteral("NOT CHECKED"), readiness_panel_);
    status_pill_->hide();
    readiness_icon_ = new QLabel(readiness_panel_);
    readiness_icon_->hide();
    layout->addWidget(readiness_panel_);

    // ── Readiness dashboard tiles (slice-5: more than four, responsive reflow) ───
    // Readiness · Encoder · Disk · Display · Audio · Capture target, plus a Last
    // session tile that appears once a completed recording exists (space-permitting).
    // reflowReadinessTiles() re-columns the grid on resize (4 → 3 → 2 columns).
    auto* tiles_host = new QWidget(content);
    tiles_grid_ = new QGridLayout(tiles_host);
    tiles_grid_->setContentsMargins(0, 0, 0, 0);
    tiles_grid_->setHorizontalSpacing(M::kSpaceMd);
    tiles_grid_->setVerticalSpacing(M::kSpaceMd);
    readiness_tile_ =
        makeReadinessTile(tiles_host, QStringLiteral("readinessTileReadiness"), QStringLiteral("Readiness"),
                          readiness_tile_value_, readiness_tile_sub_, readiness_tile_icon_);
    QLabel* enc_icon = nullptr;
    auto* encoder_tile = makeReadinessTile(tiles_host, QStringLiteral("readinessTileEncoder"),
                                           QStringLiteral("Encoder"), encoder_tile_value_, encoder_tile_sub_, enc_icon);
    QLabel* disk_icon = nullptr;
    auto* disk_tile = makeReadinessTile(tiles_host, QStringLiteral("readinessTileDisk"), QStringLiteral("Disk"),
                                        disk_tile_value_, disk_tile_sub_, disk_icon);
    QLabel* disp_icon = nullptr;
    auto* display_tile =
        makeReadinessTile(tiles_host, QStringLiteral("readinessTileDisplay"), QStringLiteral("Display"),
                          display_tile_value_, display_tile_sub_, disp_icon);
    QLabel* audio_icon = nullptr;
    auto* audio_tile = makeReadinessTile(tiles_host, QStringLiteral("readinessTileAudio"), QStringLiteral("Audio"),
                                         audio_tile_value_, audio_tile_sub_, audio_icon);
    QLabel* target_icon = nullptr;
    auto* target_tile =
        makeReadinessTile(tiles_host, QStringLiteral("readinessTileTarget"), QStringLiteral("Capture target"),
                          target_tile_value_, target_tile_sub_, target_icon);
    QLabel* session_icon = nullptr;
    session_tile_ =
        makeReadinessTile(tiles_host, QStringLiteral("readinessTileSession"), QStringLiteral("Last session"),
                          session_tile_value_, session_tile_sub_, session_icon);
    // Disk tile carries a slim usage bar (canon ReadinessTile pct).
    disk_bar_ = new QProgressBar(disk_tile);
    disk_bar_->setObjectName(QStringLiteral("diagDiskBar"));
    disk_bar_->setRange(0, 100);
    disk_bar_->setTextVisible(false);
    disk_bar_->setFixedHeight(4);
    disk_bar_->setVisible(false);
    if (auto* dl = qobject_cast<QVBoxLayout*>(disk_tile->layout()))
        dl->addWidget(disk_bar_);

    // The six core tiles are always shown; the Last session tile is gated on a
    // completed recording (setHasLastRecording), so it never claims an empty slot.
    for (QFrame* tile : {readiness_tile_, encoder_tile, disk_tile, display_tile, audio_tile, target_tile}) {
        tile->setProperty("tileActive", true);
        readiness_tiles_.push_back(tile);
    }
    session_tile_->setProperty("tileActive", has_last_recording_);
    session_tile_->setVisible(has_last_recording_);
    readiness_tiles_.push_back(session_tile_);
    reflowReadinessTiles();
    layout->addWidget(tiles_host);

    // ── Worst-first cards (shared: Simple + Expert) ─────────────────────────────
    issues_parent_ = new QWidget(content);
    overview_issues_layout_ = new QVBoxLayout(issues_parent_);
    overview_issues_layout_->setContentsMargins(0, 0, 0, 0);
    overview_issues_layout_->setSpacing(M::kSpaceSm);
    layout->addWidget(issues_parent_);

    tip_chip_ = new ui::widgets::TipChip(content);
    connect(tip_chip_, &ui::widgets::TipChip::applyFixRequested, this, &DiagnosticsPage::applyFixActionRequested);
    connect(tip_chip_, &ui::widgets::TipChip::assistedFixRequested, this, &DiagnosticsPage::openAssistedFixRequested);
    layout->addWidget(tip_chip_);

    // Slice-5: the readiness dashboard is top-anchored in BOTH views — the verdict
    // band + tile grid fill from the top and the trailing addStretch() below absorbs
    // the remaining height. (The old SimpleView vertical centring is intentionally
    // dropped; see docs/product-spec §11 and the wave spec §2/§3.3.)

    // ── Expert-only container (phases + elevation) ──────────────────────────────
    expert_container_ = new QWidget(content);
    expert_container_->setObjectName(QStringLiteral("diagExpertContainer"));
    auto* ex = new QVBoxLayout(expert_container_);
    ex->setContentsMargins(0, 0, 0, 0);
    ex->setSpacing(M::kSpaceLg);

    // Environment — audio + elevation baseline; capabilities now live on Device.
    auto* env_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ENVIRONMENT"), expert_container_);
    ex->addWidget(env_header);
    auto* env_panel = makePanel(expert_container_);
    auto* env_l = new QVBoxLayout(env_panel);
    env_l->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    env_l->setSpacing(M::kSpaceXs);
    // Tier-4 facts are repopulated from the diagnostics model each refresh; the
    // host layout is seeded with a baseline elevation row so Expert is never blank.
    auto* env_facts_host = new QWidget(env_panel);
    env_facts_host->setObjectName(QStringLiteral("diagEnvFactsHost"));
    env_facts_layout_ = new QVBoxLayout(env_facts_host);
    env_facts_layout_->setContentsMargins(0, 0, 0, 0);
    env_facts_layout_->setSpacing(M::kSpaceXs);
    env_facts_layout_->addWidget(makeInfoRow(
        QStringLiteral("Elevation"),
        QStringLiteral("Standard \xe2\x80\x94 DXGI / NVAPI baseline \xc2\xb7 monitor judder still measured"), QString(),
        env_facts_host, true));
    env_l->addWidget(env_facts_host);
    auto* device_row = new QWidget(env_panel);
    auto* dr = new QHBoxLayout(device_row);
    dr->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    dr->setSpacing(M::kSpaceMd);
    auto* device_hint =
        new QLabel(QStringLiteral("Hardware capabilities (GPU, codecs, displays, audio devices)"), device_row);
    device_hint->setProperty("labelRole", "subtitle");
    device_hint->setWordWrap(true);
    dr->addWidget(device_hint, 1);
    auto* open_device_btn = new QPushButton(QStringLiteral("Device \xe2\x86\x92"), device_row);
    open_device_btn->setObjectName(QStringLiteral("openDeviceBtn"));
    open_device_btn->setProperty("role", "ghost");
    connect(open_device_btn, &QPushButton::clicked, this, &DiagnosticsPage::openDevicePageRequested);
    dr->addWidget(open_device_btn, 0, Qt::AlignVCenter);
    env_l->addWidget(device_row);
    ex->addWidget(env_panel);

    // ② Pre-flight & Readiness — self-test lives here.
    {
        QToolButton* pre_toggle = nullptr;
        auto* pre_body = makeCollapsibleSection(
            QStringLiteral("2 \xc2\xb7 Pre-flight & Readiness"),
            QStringLiteral("Tier-1 gates the start · Tier-3 informs. Self-test validates core pipeline components."),
            expert_container_, pre_toggle);
        selftest_content_ = new QWidget(pre_body);
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
        selftest_layout_->addLayout(selftest_action_row);
        selftest_layout_->addWidget(
            makeSubLabel(QStringLiteral("Run a system check or click Run Self-Test."), selftest_content_));
        pre_body->layout()->addWidget(selftest_content_);
        pre_toggle->setChecked(true);
        ex->addWidget(pre_toggle->parentWidget());
    }

    // ③ Live — telemetry + six pipeline health cards (only meaningful while recording).
    {
        QToolButton* live_toggle = nullptr;
        auto* live_body = makeCollapsibleSection(
            QStringLiteral("3 \xc2\xb7 Live pipeline"),
            QStringLiteral("Low-overhead runtime metrics for the active recording (~5×/s). Unmeasured values are "
                           "shown as Unavailable, never zero."),
            expert_container_, live_toggle);
        live_pipeline_panel_ = new ui::widgets::LivePipelinePanel(live_body);
        live_body->layout()->addWidget(live_pipeline_panel_);
        pipeline_flow_ = new ui::widgets::PipelineFlow(live_body);
        live_body->layout()->addWidget(pipeline_flow_);
        live_toggle->setChecked(true);
        ex->addWidget(live_toggle->parentWidget());
    }

    // ④ Post-flight & Review — the real report card lives on the Edit overlay's
    // Review step (EditExportPage), reachable once a recording has finished. This
    // section never duplicates it; it only links there when a completed recording
    // exists to open.
    {
        QToolButton* post_toggle = nullptr;
        auto* post_body = makeCollapsibleSection(
            QStringLiteral("4 \xc2\xb7 Post-flight & Review"),
            QStringLiteral("After Stop: drop-%, max drift, achieved vs target and file validity, then a bridge to the "
                           "Edit overlay."),
            expert_container_, post_toggle);
        post_body->layout()->addWidget(makeSubLabel(
            QStringLiteral("The report card appears in the Edit view's Review step after a recording finishes."),
            post_body));
        open_last_report_btn_ = new QPushButton(QStringLiteral("Open last report"), post_body);
        open_last_report_btn_->setObjectName(QStringLiteral("openLastReportBtn"));
        open_last_report_btn_->setProperty("role", "ghost");
        open_last_report_btn_->setEnabled(has_last_recording_);
        connect(open_last_report_btn_, &QPushButton::clicked, this, [this]() { emit openLastReportRequested(); });
        post_body->layout()->addWidget(open_last_report_btn_);
        ex->addWidget(post_toggle->parentWidget());
    }

    // Active configuration (collapsed reference).
    config_content_ = makeCollapsibleSection(QStringLiteral("Active configuration"),
                                             QStringLiteral("Recording settings as currently configured in the app."),
                                             expert_container_, config_toggle_);
    config_content_->setObjectName(QStringLiteral("diagActiveConfigBody"));
    config_layout_ = static_cast<QVBoxLayout*>(config_content_->layout());
    config_layout_->addWidget(
        makeSubLabel(QStringLiteral("Run a system check to populate this list."), config_content_));
    ex->addWidget(config_toggle_->parentWidget());

    // Elevation unlock (opt-in · Tier-4 depth).
    auto* elev_header = new ui::widgets::SectionRuleHeader(QStringLiteral("ELEVATED DIAGNOSTICS"), expert_container_);
    elev_header->setMeta(QStringLiteral("Opt-in · relaunch as admin"));
    ex->addWidget(elev_header);
    elevation_lock_ = new ui::widgets::ElevationLock(expert_container_);
    ex->addWidget(elevation_lock_);

    layout->addWidget(expert_container_);

    // ── Logs redirect (shared, subtle, bottom) ──────────────────────────────────
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
    // Expert-only: the Simple view keeps the canon calm (verdict + tiles + tip).
    if (auto* exl = qobject_cast<QVBoxLayout*>(expert_container_->layout()))
        exl->addWidget(logs_card);

    layout->addStretch();

    // Wider cap than the old centred Simple view so the responsive tile grid has room
    // for 3–4 columns; still centred so the dashboard never stretches edge-to-edge on
    // an ultra-wide window.
    content->setMaximumWidth(1080);
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
    connect(expert_toggle_, &QAbstractButton::toggled, this, [this](bool on) {
        if (expert_mode_enabled_ == on)
            return;
        expert_mode_enabled_ = on;
        applyExpertVisibility();
        emit expertModeChanged(on);
    });

    // Seed static state.
    refreshReadinessTiles(0, 0, 0);
    refreshPipeline();
    applyExpertVisibility();
}

// ── Expert mode (single global state) ───────────────────────────────────────────

void DiagnosticsPage::setExpertModeEnabled(bool enabled) {
    if (expert_mode_enabled_ == enabled)
        return;
    expert_mode_enabled_ = enabled;
    if (expert_toggle_) {
        const QSignalBlocker b(expert_toggle_);
        expert_toggle_->setOn(enabled);
    }
    applyExpertVisibility();
    // No emit: external sync from MainWindow/ConfigPage — the toggle path emits.
}

bool DiagnosticsPage::isExpertModeEnabled() const noexcept {
    return expert_mode_enabled_;
}

void DiagnosticsPage::setHasLastRecording(bool has_last_recording) {
    has_last_recording_ = has_last_recording;
    if (open_last_report_btn_)
        open_last_report_btn_->setEnabled(has_last_recording_);
    // The Last session tile only earns a slot once a completed recording exists.
    // Update just this one tile's text + grid membership — this setter fires on every
    // chrome-state change and page show, and none of those touch disk space, the GPU
    // adapter name, or any other tile, so a full refreshReadinessTiles() re-query would
    // be wasted work unrelated to what changed.
    if (session_tile_) {
        session_tile_->setProperty("tileActive", has_last_recording_);
        session_tile_->setVisible(has_last_recording_);
        updateSessionTileText();
        reflowReadinessTiles();
    }
}

void DiagnosticsPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Review F4: ask MainWindow for the current last-recording gate on every show,
    // so the "Open last report" link cannot stay stale if the recording result
    // settled after the most recent chrome-state push.
    emit lastRecordingGateRefreshRequested();
}

void DiagnosticsPage::applyExpertVisibility() {
    if (expert_container_)
        expert_container_->setVisible(expert_mode_enabled_);
    // Both views are top-anchored (slice-5): no vertical-centring stretch flip. The
    // trailing addStretch() in the ctor absorbs the remaining height in either mode.
    if (export_report_btn_)
        export_report_btn_->setVisible(expert_mode_enabled_);
    // suite-diag2.jsx: tips are bundled (collapsed) in Simple, listed open in Expert.
    if (tip_chip_)
        tip_chip_->setDefaultOpen(expert_mode_enabled_);
    if (mode_caption_)
        mode_caption_->setText(expert_mode_enabled_ ? QStringLiteral("\xc2\xb7 Expert \xe2\x80\x94 full taxonomy")
                                                    : QStringLiteral("\xc2\xb7 Simple"));
    if (expert_mode_label_) {
        expert_mode_label_->setProperty("expertOn", expert_mode_enabled_);
        expert_mode_label_->style()->unpolish(expert_mode_label_);
        expert_mode_label_->style()->polish(expert_mode_label_);
    }
}

// ── Data injection ──────────────────────────────────────────────────────────────

void DiagnosticsPage::setSelectedCaptureTarget(const std::optional<recorder_core::CaptureTarget>& target) {
    selected_capture_target_ = target;
    if (data_ready_ && isVisible()) {
        refreshOverview();
    }
}

void DiagnosticsPage::setCaptureWindowEvidence(const std::optional<diagnostics::WindowTargetFacts>& facts,
                                               const diagnostics::WindowHubEvidence& hub) {
    capture_window_facts_ = facts;
    capture_window_hub_ = hub;
    if (data_ready_ && isVisible()) {
        refreshOverview();
    }
}

void DiagnosticsPage::setSavedDisplayUnresolved(bool unresolved, const std::string& label) {
    if (saved_display_unresolved_ == unresolved && saved_display_label_ == label) {
        return;
    }
    saved_display_unresolved_ = unresolved;
    saved_display_label_ = label;
    if (data_ready_ && isVisible()) {
        refreshOverview();
    }
}

void DiagnosticsPage::setDiagnosticData(const capability::CapabilitySet& caps, const OutputSettingsModel& output,
                                        const VideoSettingsModel& video, const capability::AudioUiState& audio,
                                        const std::string& profile_name, const std::string& hotkeys_summary,
                                        const std::string& settings_path, bool hotkeys_ok) {
    caps_ = caps;
    audio_state_ = audio;
    profile_name_ = profile_name;
    hotkeys_summary_ = hotkeys_summary;
    settings_path_ = settings_path;
    hotkeys_ok_ = hotkeys_ok;
    output_folder_ = output.output_folder;

    {
        diagnostics::Win32DiskSpaceProvider provider;
        output_drive_free_bytes_ = provider.FreeBytesForPath(output_folder_);
    }
    {
        diagnostics::Win32FilesystemProvider provider;
        output_filesystem_name_ = provider.FilesystemNameForPath(output_folder_);
    }

    cap_summary_ = diagnostics::CapabilitySummary::FromCapabilitySet(caps_);
    config_summary_ = diagnostics::ConfigSummary::FromCurrentSettings(
        output, video, audio, std::filesystem::path(settings_path_), profile_name_, hotkeys_summary_);
    active_user_config_ = diagnostics::UserConfigFromSettings(output, video);
    capability::SettingsResolver resolver(caps_);
    profile_validation_ = resolver.ValidateConfig(active_user_config_);
    data_ready_ = true;

    refreshOverview();
    refreshSelfTest();
    refreshConfiguration();
    refreshPipeline();

    const bool live_recording =
        last_live_snapshot_.valid && (last_live_snapshot_.lifecycle == recorder_core::DiagnosticsLifecycle::Recording ||
                                      last_live_snapshot_.lifecycle == recorder_core::DiagnosticsLifecycle::Paused);
    if (live_recording) {
        renderPipelineCards(last_live_snapshot_);
    }
}

void DiagnosticsPage::setPresentProvider(diagnostics::IPresentProvider* provider) noexcept {
    present_provider_ = provider;
}

void DiagnosticsPage::setDpcProvider(diagnostics::DpcLatencyProvider* provider) noexcept {
    dpc_provider_ = provider;
}

void DiagnosticsPage::setElevationProvider(diagnostics::IElevationProvider* provider) noexcept {
    elevation_provider_ = provider;
}

void DiagnosticsPage::applyLiveDiagnostics(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    last_live_snapshot_ = snapshot;

    if (present_provider_ != nullptr) {
        const diagnostics::PresentSample ps = present_provider_->Sample();
        if (ps.available) {
            last_live_snapshot_.capture.source_present_mode = ToSnapshotMode(ps.mode);
            last_live_snapshot_.capture.source_tearing = ps.tearing;
            last_live_snapshot_.capture.present_mode_availability = recorder_core::MetricAvailability::Available;
        }
    }

    updatePipelineCards(last_live_snapshot_);

    if (live_pipeline_panel_ == nullptr) {
        return;
    }
    live_pipeline_panel_->applySnapshot(last_live_snapshot_);
}

void DiagnosticsPage::updatePipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    if (!pipeline_flow_)
        return;

    const bool recording = s.valid && (s.lifecycle == recorder_core::DiagnosticsLifecycle::Recording ||
                                       s.lifecycle == recorder_core::DiagnosticsLifecycle::Paused);
    if (!recording) {
        refreshPipeline();
        last_cards_applied_ = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_cards_applied_ != std::chrono::steady_clock::time_point{} &&
        (now - last_cards_applied_) < std::chrono::milliseconds(500)) {
        return;
    }
    last_cards_applied_ = now;

    renderPipelineCards(s);

    // Live measured problems (judder, disk write-stall, audio device loss) are Tier-2:
    // "measured, not predicted". Re-run the honesty rail on the same throttled cadence
    // so they surface WHILE recording, not only on the next manual check.
    if (data_ready_) {
        refreshOverview();
    }
}

void DiagnosticsPage::renderPipelineCards(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    using recorder_core::MetricAvailability;
    using recorder_core::StageHealth;
    using recorder_core::StageId;
    using recorder_core::StageSignals;
    using Status = ui::widgets::PipelineStepCard::Status;

    if (!pipeline_flow_)
        return;

    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const double budget_ms = (s.capture.target_fps > 0.0) ? 1000.0 / s.capture.target_fps : (1000.0 / 60.0);

    const uint64_t problem_drops = s.capture.frames_dropped_problem();
    if (s.session_generation != cards_last_generation_) {
        cards_last_generation_ = s.session_generation;
        cards_last_problem_drops_ = problem_drops;
    }
    const uint32_t capture_recent_drops = (problem_drops > cards_last_problem_drops_)
                                              ? static_cast<uint32_t>(problem_drops - cards_last_problem_drops_)
                                              : 0;
    cards_last_problem_drops_ = problem_drops;

    constexpr uint32_t kQueueBusyDepth = 8;
    constexpr double kDiskBudgetMs = 8.0;

    StageSignals capture{};
    capture.id = StageId::SourceCapture;
    capture.available = s.capture.target_fps > 0.0 && s.capture.actual_fps > 0.0;
    capture.is_duration_stage = false;
    capture.can_bottleneck = true;
    capture.fps_ratio = (s.capture.target_fps > 0.0) ? s.capture.actual_fps / s.capture.target_fps : 1.0;
    capture.recent_drops = capture_recent_drops;

    StageSignals queue{};
    queue.id = StageId::FrameQueue;
    queue.available = true;
    queue.is_duration_stage = false;
    queue.can_bottleneck = false;
    queue.queue_depth = s.video_queue.current_depth;
    queue.queue_busy_threshold = kQueueBusyDepth;

    StageSignals comp{};
    comp.id = StageId::Compositor;
    comp.available = s.compositor.active;
    comp.is_duration_stage = true;
    comp.can_bottleneck = true;
    comp.avg_ms = s.compositor.average_ms;

    StageSignals enc{};
    enc.id = StageId::Encoder;
    enc.available = s.video_encoder.average_ms > 0.0 || s.video_encoder.frames_encoded > 0;
    enc.is_duration_stage = true;
    enc.can_bottleneck = true;
    enc.avg_ms = s.video_encoder.average_ms;

    StageSignals mux{};
    mux.id = StageId::Muxer;
    mux.available = s.mux.process_availability == MetricAvailability::Available;
    mux.is_duration_stage = true;
    mux.can_bottleneck = true;
    mux.avg_ms = s.mux.process_average_ms;

    StageSignals disk{};
    disk.id = StageId::Disk;
    disk.available = s.disk.latency_availability == MetricAvailability::Available;
    disk.is_duration_stage = true;
    disk.can_bottleneck = true;
    disk.avg_ms = s.disk.average_write_ms;
    disk.budget_ms = kDiskBudgetMs;

    const StageSignals stages[] = {capture, queue, comp, enc, mux, disk};
    const recorder_core::PipelineHealthVerdict verdict = recorder_core::ResolvePipelineHealth(stages, budget_ms);

    auto to_status = [](StageHealth h) -> Status {
        switch (h) {
        case StageHealth::Healthy:
            return Status::Ok;
        case StageHealth::Busy:
            return Status::Hotspot;
        case StageHealth::Bottleneck:
            return Status::Over;
        }
        return Status::Ok;
    };
    auto health_of = [&](StageId id) -> StageHealth {
        for (const auto& sv : verdict.per_stage)
            if (sv.id == id)
                return sv.health;
        return StageHealth::Healthy;
    };

    auto ms = [&](double v, bool avail) { return avail ? QString::number(v, 'f', 1) + QStringLiteral(" ms") : dash; };

    const bool cap_num = s.capture.target_fps > 0.0;
    const QString cap_number = cap_num ? QString::number(s.capture.actual_fps, 'f', 1) + QStringLiteral(" / ") +
                                             QString::number(s.capture.target_fps, 'f', 1) + QStringLiteral(" fps")
                                       : dash;
    const QString cap_tip = (s.capture.acquire_availability == MetricAvailability::Available)
                                ? QStringLiteral("Acquire ") + QString::number(s.capture.acquire_average_ms, 'f', 2) +
                                      QStringLiteral(" ms (CPU)")
                                : QStringLiteral("Acquire timing unavailable for this capture mode");
    pipeline_flow_->setStepLive(0, to_status(health_of(StageId::SourceCapture)), QString(), QStringLiteral("CPU"),
                                cap_number, cap_tip);

    const QString q_number = s.video_queue.bounded && s.video_queue.capacity > 0
                                 ? QString::number(s.video_queue.current_depth) + QStringLiteral(" / ") +
                                       QString::number(s.video_queue.capacity)
                                 : QString::number(s.video_queue.current_depth);
    pipeline_flow_->setStepLive(1, to_status(health_of(StageId::FrameQueue)), QString(), dash, q_number,
                                QStringLiteral("Frames waiting between encode and mux (peak ") +
                                    QString::number(s.video_queue.peak_depth) + QStringLiteral(")"));

    const QString comp_tip = QStringLiteral("CPU submit (GPU execution time not measured in this view). VPBlt ") +
                             ((s.compositor.vpblt_availability == MetricAvailability::Available)
                                  ? QString::number(s.compositor.vpblt_average_ms, 'f', 2) + QStringLiteral(" ms")
                                  : dash);
    pipeline_flow_->setStepLive(2, to_status(health_of(StageId::Compositor)), QString(), QStringLiteral("GPU"),
                                ms(s.compositor.average_ms, s.compositor.average_ms > 0.0), comp_tip);

    pipeline_flow_->setStepLive(3, to_status(health_of(StageId::Encoder)), QString(), QStringLiteral("GPU (NVENC)"),
                                ms(s.video_encoder.average_ms, s.video_encoder.average_ms > 0.0),
                                QStringLiteral("CPU submit\xe2\x86\x92ready latency (peak ") +
                                    QString::number(s.video_encoder.peak_ms, 'f', 1) + QStringLiteral(" ms)"));

    pipeline_flow_->setStepLive(4, to_status(health_of(StageId::Muxer)), QString(), QStringLiteral("CPU"),
                                ms(s.mux.process_average_ms, mux.available),
                                QStringLiteral("Mux drain processing (peak ") +
                                    QString::number(s.mux.process_peak_ms, 'f', 2) + QStringLiteral(" ms)"));

    pipeline_flow_->setStepLive(5, to_status(health_of(StageId::Disk)), QString(), QStringLiteral("CPU"),
                                ms(s.disk.average_write_ms, disk.available),
                                QStringLiteral("Filesystem write-call latency (peak ") +
                                    QString::number(s.disk.peak_write_ms, 'f', 1) + QStringLiteral(" ms)"));
}

// ── Helpers ──────────────────────────────────────────────────────────────────

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

QFrame* DiagnosticsPage::makeReadinessTile(QWidget* parent, const QString& object_name, const QString& title,
                                           QLabel*& out_value, QLabel*& out_sub, QLabel*& out_icon) {
    // Always parented up front — QGridLayout::addWidget() would otherwise be the ONLY
    // thing that parents a tile, and a tile excluded from the active set (like the
    // gated Last-session tile before its first activation) would stay parentless:
    // invisible to findChild() and leaked from the widget tree. See the reflow
    // grid's addWidget()/takeAt() dance in reflowReadinessTiles() for placement, which
    // is a layout-membership concern, kept separate from parenting.
    auto* tile = new QFrame(parent);
    tile->setObjectName(object_name);
    tile->setProperty("panelRole", "readinessTile");
    tile->setProperty("tileTone", "neutral");
    auto* tlay = new QVBoxLayout(tile);
    tlay->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    tlay->setSpacing(M::kSpaceXs);

    auto* top = new QHBoxLayout();
    top->setSpacing(M::kSpaceSm);
    auto* title_label = new QLabel(title.toUpper(), tile);
    title_label->setProperty("labelRole", "readinessTileTitle");
    top->addWidget(title_label, 1);
    out_icon = new QLabel(tile);
    out_icon->setFixedSize(14, 14);
    out_icon->setVisible(false);
    top->addWidget(out_icon, 0, Qt::AlignVCenter);
    tlay->addLayout(top);

    out_value = new QLabel(QString::fromUtf8("\xe2\x80\x94"), tile);
    out_value->setProperty("labelRole", "readinessTileValue");
    tlay->addWidget(out_value);

    out_sub = new QLabel(QString(), tile);
    out_sub->setProperty("labelRole", "readinessTileSub");
    out_sub->setWordWrap(true);
    tlay->addWidget(out_sub);

    return tile;
}

// Re-columns the readiness dashboard grid for the current width: 4 columns on a wide
// window, 3 at typical width, 2 when narrow. Only the active tiles (six core + the
// gated Last-session tile) are placed. Cheap enough to run on every resize — the tile
// count is tiny — but guarded so an unchanged (columns, count) pair is a no-op.
void DiagnosticsPage::reflowReadinessTiles() {
    if (!tiles_grid_)
        return;

    // Key off the PAGE's own width, not the tile-grid host's — the host sits inside a
    // QScrollArea, so a wide grid keeps its natural (unshrunk) width and the scroll
    // area grows a horizontal scrollbar instead of forcing a narrower layout; measuring
    // the host would make the reflow circular (never actually narrows). The page's own
    // width tracks the real window size directly via resizeEvent().
    const int avail = width() - 2 * M::kSpaceXl;
    const int width_columns = (avail >= 1000) ? 4 : (avail >= 620) ? 3 : 2;

    // Cheap early-out BEFORE the active-tile scan below: resizeEvent() calls this on
    // every QResizeEvent, and interactive window dragging fires dozens per second. Most
    // of them don't cross a column-count threshold, so bail using the cached active
    // count (kept in sync by whoever last changed a tile's "tileActive" property and
    // then called this function) before paying for a QVector allocation + a
    // property-map lookup per tile.
    if (tiles_active_count_ > 0 && std::min(width_columns, tiles_active_count_) == tiles_columns_)
        return;

    QVector<QFrame*> active;
    for (QFrame* tile : readiness_tiles_) {
        if (tile && tile->property("tileActive").toBool())
            active.push_back(tile);
    }
    if (active.isEmpty())
        return;

    const int columns = std::min(width_columns, static_cast<int>(active.size()));
    if (columns == tiles_columns_ && active.size() == tiles_active_count_)
        return;
    tiles_columns_ = columns;
    tiles_active_count_ = static_cast<int>(active.size());

    // Detach every item without deleting the tile widgets (QWidgetItem deletion does
    // not delete the widget), then re-place into the new column count.
    while (QLayoutItem* item = tiles_grid_->takeAt(0))
        delete item;
    for (int i = 0; i < active.size(); ++i)
        tiles_grid_->addWidget(active.at(i), i / columns, i % columns);
    // Equalise the live columns; zero the rest so a shrunk grid does not keep width.
    constexpr int kMaxColumns = 4;
    for (int c = 0; c < kMaxColumns; ++c)
        tiles_grid_->setColumnStretch(c, c < columns ? 1 : 0);
}

void DiagnosticsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflowReadinessTiles();
}

QWidget* DiagnosticsPage::makeCollapsibleSection(const QString& title, const QString& subtitle, QWidget* parent,
                                                 QToolButton*& out_toggle) {
    auto* wrap = new QWidget(parent);
    auto* wl = new QVBoxLayout(wrap);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(M::kSpaceXs);

    auto* toggle = new QToolButton(wrap);
    toggle->setProperty("role", "collapseHead");
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setCheckable(true);
    toggle->setChecked(false);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Escape '&' so QToolButton doesn't eat it as a mnemonic ("A & B" → "A B" w/ shortcut).
    QString safe_title = title;
    safe_title.replace(QLatin1Char('&'), QLatin1String("&&"));
    toggle->setText(safe_title);
    const qreal dpr = toggle->devicePixelRatioF();
    toggle->setIcon(
        QIcon(ui::theme::lucidePixmap(QStringLiteral("chevron-right"), QString::fromUtf8(Pal::kText2), 14, dpr)));
    wl->addWidget(toggle);

    auto* body = new QWidget(wrap);
    body->setVisible(false);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(M::kSpaceXs, M::kSpaceSm, M::kSpaceXs, M::kSpaceSm);
    body_layout->setSpacing(M::kSpaceXs);

    if (!subtitle.trimmed().isEmpty()) {
        auto* sub = new QLabel(subtitle, body);
        sub->setProperty("labelRole", "collapseSub");
        sub->setWordWrap(true);
        body_layout->addWidget(sub);
    }

    wl->addWidget(body);

    connect(toggle, &QToolButton::toggled, this, [toggle, body](bool on) {
        body->setVisible(on);
        const qreal dpr = toggle->devicePixelRatioF();
        toggle->setIcon(
            QIcon(ui::theme::lucidePixmap(on ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
                                          QString::fromUtf8(Pal::kText2), 14, dpr)));
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
    // Slice-5: the verdict band is a dashboard header, so the calm "ready" state also
    // earns a soft tint (kept faint — non-alarmist per the diagnostics ethos). Both the
    // band and the hidden legacy status_pill_ (kept alive only so its many writers
    // keep working; superseded by verdict_headline_) share this tint rule.
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
    if (verdict_glyph_) {
        QString glyph_name = QStringLiteral("info");
        QString glyph_color = QString::fromUtf8(Pal::kText2);
        if (state == QStringLiteral("ready")) {
            glyph_name = QStringLiteral("check-circle");
            glyph_color = QString::fromUtf8(Pal::kOk);
        } else if (state == QStringLiteral("warn")) {
            glyph_name = QStringLiteral("alert-triangle");
            glyph_color = QString::fromUtf8(Pal::kWarn);
        } else if (state == QStringLiteral("blocked")) {
            glyph_name = QStringLiteral("x-circle");
            glyph_color = QString::fromUtf8(Pal::kErr);
        }
        verdict_glyph_->setPixmap(
            ui::theme::lucidePixmap(glyph_name, glyph_color, 21, verdict_glyph_->devicePixelRatioF()));
        verdict_glyph_->setProperty("verdictTone", state);
        repolish(verdict_glyph_);
    }
    if (readiness_icon_) {
        QString icon_name;
        QString icon_color;
        if (state == QStringLiteral("ready")) {
            icon_name = QStringLiteral("check-circle");
            icon_color = QString::fromUtf8(Pal::kOk);
        } else if (state == QStringLiteral("warn")) {
            icon_name = QStringLiteral("alert-triangle");
            icon_color = QString::fromUtf8(Pal::kWarn);
        } else if (state == QStringLiteral("blocked")) {
            icon_name = QStringLiteral("x-circle");
            icon_color = QString::fromUtf8(Pal::kErr);
        }
        if (icon_name.isEmpty()) {
            readiness_icon_->clear();
            readiness_icon_->setVisible(false);
        } else {
            const qreal dpr = readiness_icon_->devicePixelRatioF();
            readiness_icon_->setPixmap(ui::theme::lucidePixmap(icon_name, icon_color, 14, dpr));
            readiness_icon_->setVisible(false); // replaced by the hero glyph chip
        }
    }
}

void DiagnosticsPage::onRunCheck() {
    status_pill_->setText(QStringLiteral("CHECKING"));
    if (verdict_headline_)
        verdict_headline_->setText(QStringLiteral("Checking\xe2\x80\xa6"));
    setReadinessState(QStringLiteral("checking"));
    last_check_label_->setText(QStringLiteral("Last check: running..."));
    summary_label_->setText(QStringLiteral("Check in progress."));

    if (!data_ready_) {
        status_pill_->setText(QStringLiteral("NO DATA"));
        if (verdict_headline_)
            verdict_headline_->setText(QStringLiteral("Not checked yet"));
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
    // Second entry point to the one support-bundle action; MainWindow owns the
    // single code path shared with the Logs page.
    emit createSupportBundleRequested();
}

// ── Configuration refresh (Expert reference table) ──────────────────────────────

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
}

// ── Self-Test refresh (Expert · Pre-flight) ─────────────────────────────────────

void DiagnosticsPage::refreshSelfTest() {
    if (!selftest_layout_ || !selftest_content_)
        return;

    while (selftest_layout_->count() > 2) {
        QLayoutItem* child = selftest_layout_->takeAt(selftest_layout_->count() - 1);
        if (child->widget())
            delete child->widget();
        delete child;
    }

    diagnostics::SelfTestRunner runner;
    auto checklist = runner.Run();

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

// ── Worst-first cards + Tier-3 tip bundle ───────────────────────────────────────

void DiagnosticsPage::refreshTopIssues(const diagnostics::DiagnosticChecklist& recommendations) {
    if (!overview_issues_layout_)
        return;

    QLayoutItem* child = nullptr;
    while ((child = overview_issues_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    int issue_count = 0;
    constexpr int kMaxIssues = 6;

    // One entry card (diag-model.jsx EntryCard): title + mono ID chip + optional
    // Elev lock badge, a FixAction, and an L1→L2→L3 Evidence disclosure (Measured
    // value + Why recommendation + collapsed log excerpt).
    const auto add_issue_card = [&](diagnostics::DiagnosticSeverity severity, const QString& id, const QString& title,
                                    const QString& summary, const QString& why, const QString& measured,
                                    const QString& log, bool elev, const diagnostics::FixAction* fix) {
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
        if (!id.isEmpty()) {
            auto* id_chip = new QLabel(id, card);
            id_chip->setObjectName(QStringLiteral("issueIdChip"));
            id_chip->setProperty("labelRole", "issueIdChip");
            title_row->addWidget(id_chip, 0, Qt::AlignTop);
        }
        if (elev) {
            auto* elev_badge = new QLabel(QStringLiteral("Elev"), card);
            elev_badge->setObjectName(QStringLiteral("issueElevBadge"));
            elev_badge->setProperty("labelRole", "elevBadge");
            elev_badge->setToolTip(
                QStringLiteral("Measured from the elevated present-path baseline (PresentMon / DPC-ISR)."));
            title_row->addWidget(elev_badge, 0, Qt::AlignTop);
        }
        card_layout->addLayout(title_row);

        auto* summary_label = new QLabel(summary, card);
        summary_label->setProperty("labelRole", "issueDesc");
        summary_label->setWordWrap(true);
        card_layout->addWidget(summary_label);

        if (fix != nullptr) {
            if (fix->safety == diagnostics::FixAction::Safety::Auto) {
                auto* fix_btn = new QPushButton(QString::fromStdString(fix->label), card);
                fix_btn->setProperty("role", "ghost");
                fix_btn->setObjectName(QStringLiteral("issueFixBtn"));
                const QString fix_id = QString::fromStdString(fix->id);
                const QString changes = QString::fromStdString(fix->changes_summary);
                connect(fix_btn, &QPushButton::clicked, this,
                        [this, fix_id, changes]() { emit applyFixActionRequested(fix_id, changes); });
                card_layout->addWidget(fix_btn);
            } else if (fix->safety == diagnostics::FixAction::Safety::Assisted) {
                auto* fix_btn =
                    new QPushButton(QString::fromStdString(fix->label) + QStringLiteral(" \xe2\x86\x92"), card);
                fix_btn->setProperty("role", "ghost");
                fix_btn->setObjectName(QStringLiteral("issueFixBtn"));
                const QString fix_id = QString::fromStdString(fix->id);
                connect(fix_btn, &QPushButton::clicked, this,
                        [this, fix_id]() { emit openAssistedFixRequested(fix_id); });
                card_layout->addWidget(fix_btn);
            } else if (fix->safety == diagnostics::FixAction::Safety::External) {
                auto* fix_label = new QLabel(QString::fromStdString(fix->label), card);
                fix_label->setProperty("labelRole", "issueMeta");
                fix_label->setWordWrap(true);
                card_layout->addWidget(fix_label);
            }
        }

        // Evidence disclosure (L1 summary above → L2 measured + why → L3 log excerpt).
        const bool has_evidence = !measured.trimmed().isEmpty() || !why.trimmed().isEmpty() || !log.trimmed().isEmpty();
        if (has_evidence) {
            auto* ev_toggle = new QToolButton(card);
            ev_toggle->setObjectName(QStringLiteral("issueEvidenceToggle"));
            ev_toggle->setProperty("role", "collapseHead");
            ev_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            ev_toggle->setCheckable(true);
            ev_toggle->setChecked(false);
            ev_toggle->setCursor(Qt::PointingHandCursor);
            ev_toggle->setText(QStringLiteral("Evidence"));
            const qreal dpr = ev_toggle->devicePixelRatioF();
            ev_toggle->setIcon(QIcon(
                ui::theme::lucidePixmap(QStringLiteral("chevron-right"), QString::fromUtf8(Pal::kText2), 12, dpr)));

            auto* ev_body = new QWidget(card);
            ev_body->setObjectName(QStringLiteral("issueEvidenceBody"));
            ev_body->setVisible(false);
            auto* ev_l = new QVBoxLayout(ev_body);
            ev_l->setContentsMargins(0, M::kSpaceXs, 0, 0);
            ev_l->setSpacing(M::kSpaceXs);
            const auto add_ev_row = [&](const QString& label, const QString& value, const char* role) {
                if (value.trimmed().isEmpty())
                    return;
                auto* lbl = new QLabel(label, ev_body);
                lbl->setProperty("labelRole", "evidenceLabel");
                ev_l->addWidget(lbl);
                auto* val = new QLabel(value, ev_body);
                val->setProperty("labelRole", role);
                val->setWordWrap(true);
                ev_l->addWidget(val);
            };
            add_ev_row(QStringLiteral("Measured"), measured, "evidenceMeasured");
            add_ev_row(QStringLiteral("Why"), why, "evidenceWhy");
            add_ev_row(QStringLiteral("Log excerpt"), log, "evidenceLog");

            connect(ev_toggle, &QToolButton::toggled, this, [ev_toggle, ev_body](bool on) {
                ev_body->setVisible(on);
                const qreal d = ev_toggle->devicePixelRatioF();
                ev_toggle->setIcon(
                    QIcon(ui::theme::lucidePixmap(on ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
                                                  QString::fromUtf8(Pal::kText2), 12, d)));
            });
            card_layout->addWidget(ev_toggle);
            card_layout->addWidget(ev_body);
        }

        overview_issues_layout_->addWidget(card);
        ++issue_count;
    };

    // Tier-1 blockers first: profile invalidity, then engine blockers.
    if (!profile_validation_.succeeded) {
        for (const auto& invalid : profile_validation_.invalidity) {
            const QString field_display = QString::fromStdString(diagnostics::InvalidFieldDisplayName(invalid.field));
            const QString action_hint = QString::fromStdString(diagnostics::InvalidFieldActionHint(invalid.field));
            add_issue_card(diagnostics::DiagnosticSeverity::Blocker, QString(),
                           field_display + QStringLiteral(" is not supported"), QString::fromStdString(invalid.message),
                           action_hint, QString(), QString(), false, nullptr);
        }
    }

    const bool has_profile_invalidity = !profile_validation_.invalidity.empty();
    const std::vector<diagnostics::DiagnosticResult> ordered_recommendations =
        diagnostics::BuildTopIssueRecommendations(recommendations, has_profile_invalidity);

    const auto add_result_card = [&](const diagnostics::DiagnosticResult& result) {
        add_issue_card(result.severity, QString::fromStdString(result.id), QString::fromStdString(result.title),
                       QString::fromStdString(result.summary), QString::fromStdString(result.recommendation),
                       QString::fromStdString(result.current_value), QString::fromStdString(result.detail),
                       needsElevation(result.id), result.fix_action.has_value() ? &result.fix_action.value() : nullptr);
    };

    // Tier-1 blockers (worst-first).
    for (const auto& result : ordered_recommendations) {
        if (result.tier == diagnostics::DiagnosticTier::Blocker)
            add_result_card(result);
    }

    for (const auto& warning : profile_validation_.warnings) {
        add_issue_card(diagnostics::DiagnosticSeverity::Notice, QString(),
                       QStringLiteral("Configuration needs validation"), QString::fromStdString(warning.message),
                       QStringLiteral("Run a short recording to validate quality on this machine."), QString(),
                       QStringLiteral("Code: %1").arg(QString::fromStdString(warning.code)), false, nullptr);
    }

    if (!hotkeys_ok_ && hotkeys_summary_ != "None configured") {
        add_issue_card(
            diagnostics::DiagnosticSeverity::Notice, QString(), QStringLiteral("Global hotkeys are not active"),
            QStringLiteral("Hotkeys are configured but not currently registered."),
            QStringLiteral("Open the Hotkeys page and reapply the binding if shortcuts do not trigger."), QString(),
            QStringLiteral("If the app just launched, this can clear once startup completes."), false, nullptr);
    }

    // Tier-2 measured problems become cards; Tier-3 optimisations bundle into the
    // quiet tip chip. The tier is read straight from each result — never re-derived.
    QVector<ui::widgets::TipChip::Tip> tips;
    for (const auto& result : ordered_recommendations) {
        if (result.tier == diagnostics::DiagnosticTier::MeasuredProblem) {
            add_result_card(result);
        } else if (diagnostics::BundlesIntoTipChip(result.tier)) {
            ui::widgets::TipChip::Tip tip;
            tip.id = QString::fromStdString(result.id);
            tip.summary = QString::fromStdString(result.title);
            if (result.fix_action.has_value()) {
                const auto& fa = result.fix_action.value();
                tip.fix_label = QString::fromStdString(fa.label);
                tip.fix_kind = fixKind(fa);
                tip.fix_id = QString::fromStdString(fa.id);
                tip.changes = QString::fromStdString(fa.changes_summary);
            }
            tips.push_back(tip);
        }
    }

    if (tip_chip_)
        tip_chip_->setTips(tips);

    // Calm by design: no empty-state text when clean — the verdict + tiles carry it.
    issues_parent_->setVisible(issue_count > 0);
}

// ── Readiness tiles ─────────────────────────────────────────────────────────────

void DiagnosticsPage::refreshReadinessTiles(int blockers, int notices, int cap_passes) {
    const QString dash = QString::fromUtf8("\xe2\x80\x94");

    const auto setTone = [](QFrame* tile, const char* tone) {
        if (!tile)
            return;
        tile->setProperty("tileTone", tone);
        tile->style()->unpolish(tile);
        tile->style()->polish(tile);
    };

    // Display names carry the backend suffix ("AV1 (NVENC)"); both the Encoder and
    // Audio tiles add their own backend/source token on the sub line, so both need the
    // bare codec name with that suffix trimmed off.
    const auto stripBackendSuffix = [](QString codec) {
        if (const int paren = codec.indexOf(QStringLiteral(" (")); paren > 0)
            codec.truncate(paren);
        return codec;
    };

    // Tile 1 — Readiness.
    const int total = cap_passes + blockers + notices;
    if (blockers > 0) {
        readiness_tile_value_->setText(QStringLiteral("Action needed"));
        readiness_tile_sub_->setText(blockers == 1
                                         ? QStringLiteral("1 blocker \xc2\xb7 recording is blocked")
                                         : QStringLiteral("%1 blockers \xc2\xb7 recording is blocked").arg(blockers));
        readiness_tile_icon_->setVisible(false);
        setTone(readiness_tile_, "blocker");
    } else if (notices > 0) {
        readiness_tile_value_->setText(QStringLiteral("%1 / %2").arg(cap_passes).arg(total));
        readiness_tile_sub_->setText(notices == 1 ? QStringLiteral("checks pass \xc2\xb7 1 issue")
                                                  : QStringLiteral("checks pass \xc2\xb7 %1 issues").arg(notices));
        readiness_tile_icon_->setVisible(false);
        setTone(readiness_tile_, "notice");
    } else if (data_ready_) {
        readiness_tile_value_->setText(QStringLiteral("%1 / %2").arg(cap_passes).arg(total));
        readiness_tile_sub_->setText(QStringLiteral("checks passed"));
        readiness_tile_icon_->setPixmap(ui::theme::lucidePixmap(QStringLiteral("check-circle"),
                                                                QString::fromUtf8(Pal::kOk), 14,
                                                                readiness_tile_icon_->devicePixelRatioF()));
        readiness_tile_icon_->setVisible(true);
        setTone(readiness_tile_, "neutral");
    } else {
        readiness_tile_value_->setText(dash);
        readiness_tile_sub_->setText(QStringLiteral("run a check"));
        readiness_tile_icon_->setVisible(false);
        setTone(readiness_tile_, "neutral");
    }

    // Tile 2 — Encoder: the GPU carrying the encode, codec as the detail line
    // (canon suite-diag2.jsx READINESS). Falls back to the codec + container
    // when the adapter name is unknown.
    if (data_ready_) {
        const QString gpu = QString::fromStdString(caps_.gpu_adapter_name).trimmed();
        const QString codec = stripBackendSuffix(
            QString::fromStdString(diagnostics::VideoCodecDisplayName(active_user_config_.video_codec)));
        encoder_tile_value_->setText(gpu.isEmpty() ? codec : gpu);
        encoder_tile_sub_->setText(
            gpu.isEmpty() ? QString::fromStdString(diagnostics::ContainerDisplayName(active_user_config_.container))
                          : QStringLiteral("%1 \xc2\xb7 NVENC").arg(codec));
    } else {
        encoder_tile_value_->setText(dash);
        encoder_tile_sub_->setText(QStringLiteral("active encoder"));
    }

    // Tile 3 — Disk (free space on the output drive). A queried zero is a full
    // drive and must read "0 B", not blank; only an unqueryable volume shows the dash.
    if (data_ready_ && output_drive_free_bytes_.has_value()) {
        disk_tile_value_->setText(humanBytes(*output_drive_free_bytes_));
        // Slim usage bar: how full the output volume is (display-only; the
        // low-disk guard owns the actual policy).
        const QStorageInfo storage(QString::fromStdWString(output_folder_.wstring()));
        if (disk_bar_ && storage.isValid() && storage.bytesTotal() > 0) {
            const double used =
                1.0 - static_cast<double>(*output_drive_free_bytes_) / static_cast<double>(storage.bytesTotal());
            disk_bar_->setValue(std::clamp(static_cast<int>(used * 100.0 + 0.5), 0, 100));
            disk_bar_->setVisible(true);
        } else if (disk_bar_) {
            disk_bar_->setVisible(false);
        }
        QString drive = QString::fromStdString(output_folder_.root_name().string());
        if (drive.isEmpty())
            drive = QString::fromStdString(output_folder_.string());
        disk_tile_sub_->setText(drive.isEmpty() ? QStringLiteral("free \xc2\xb7 output drive")
                                                : QStringLiteral("free \xc2\xb7 %1").arg(drive));
    } else {
        disk_tile_value_->setText(dash);
        disk_tile_sub_->setText(QStringLiteral("output drive"));
        if (disk_bar_)
            disk_bar_->setVisible(false);
    }

    // Tile 4 — Display (current screen resolution + refresh; honest static fact).
    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        const QSize px = screen->size();
        display_tile_value_->setText(QStringLiteral("%1 \xc3\x97 %2").arg(px.width()).arg(px.height()));
        display_tile_sub_->setText(
            QStringLiteral("%1 Hz \xc2\xb7 primary display").arg(QString::number(std::lround(screen->refreshRate()))));
    } else {
        display_tile_value_->setText(dash);
        display_tile_sub_->setText(QStringLiteral("display"));
    }

    // Tile 5 — Audio (codec + routing summary of the configured sources; a plain
    // capability/environment readout, never coloured as a problem).
    if (data_ready_) {
        const QString codec = stripBackendSuffix(
            QString::fromStdString(diagnostics::AudioCodecDisplayName(active_user_config_.audio_codec)));
        const int sources = static_cast<int>(audio_state_.IsAppEnabled()) +
                            static_cast<int>(audio_state_.IsSysEnabled()) +
                            static_cast<int>(audio_state_.IsMicEnabled());
        const QString channels = audio_state_.audio_channels <= 1 ? QStringLiteral("Mono") : QStringLiteral("Stereo");
        // Non-breaking space between the number and its unit: word-wrap must
        // never split "48 kHz" onto two lines mid-value inside the tile subline.
        const QString rate =
            QStringLiteral("%1\xc2\xa0kHz").arg(QString::number(audio_state_.audio_sample_rate / 1000.0, 'g', 3));
        audio_tile_value_->setText(codec.isEmpty() ? dash : codec);
        if (sources == 0) {
            audio_tile_sub_->setText(QStringLiteral("no sources \xc2\xb7 silent"));
        } else {
            const QString src_word = sources == 1 ? QStringLiteral("source") : QStringLiteral("sources");
            audio_tile_sub_->setText(
                QStringLiteral("%1 %2 \xc2\xb7 %3 \xc2\xb7 %4").arg(sources).arg(src_word).arg(rate).arg(channels));
        }
    } else {
        audio_tile_value_->setText(dash);
        audio_tile_sub_->setText(QStringLiteral("audio sources"));
    }

    // Tile 6 — Capture target (what will be recorded). Prefers the concrete selected
    // target's description; otherwise falls back to the configured target kind so the
    // tile still reads honestly before a target is picked.
    {
        QString target_value;
        QString target_sub;
        if (selected_capture_target_.has_value()) {
            const bool window = selected_capture_target_->kind == recorder_core::CaptureTarget::Kind::Window;
            target_value = window ? QStringLiteral("Window") : QStringLiteral("Screen");
            target_sub = QString::fromStdString(selected_capture_target_->description);
            if (target_sub.trimmed().isEmpty())
                target_sub = window ? QStringLiteral("application window") : QStringLiteral("full display");
        } else if (data_ready_) {
            const bool window = audio_state_.target_kind == capability::CaptureTargetKind::Window;
            target_value = window ? QStringLiteral("Window") : QStringLiteral("Screen");
            target_sub = window ? QStringLiteral("application window") : QStringLiteral("full display");
        } else {
            target_value = dash;
            target_sub = QStringLiteral("capture target");
        }
        target_tile_value_->setText(target_value);
        target_tile_sub_->setText(target_sub);
    }

    // Tile 7 — Last session.
    updateSessionTileText();
}

// Tile 7 — Last session (gated on a completed recording by setHasLastRecording). The
// real report card lives on the Edit overlay's Review step; this tile is only a calm
// signpost to it, never a fabricated metric. Split out from refreshReadinessTiles() so
// setHasLastRecording() can update just this one tile's text without re-querying disk
// space, GPU/codec names, and every other tile that has nothing to do with recording
// completion.
void DiagnosticsPage::updateSessionTileText() {
    if (!session_tile_)
        return;
    if (has_last_recording_) {
        session_tile_value_->setText(QStringLiteral("Recorded"));
        session_tile_sub_->setText(QStringLiteral("report in Edit \xc2\xb7 Review"));
    } else {
        session_tile_value_->setText(QString::fromUtf8("\xe2\x80\x94"));
        session_tile_sub_->setText(QStringLiteral("no recording yet"));
    }
}

// ── Overview refresh ────────────────────────────────────────────────────────────

void DiagnosticsPage::refreshOverview() {
    if (!data_ready_)
        return;

    const uint32_t monitor_refresh_hz = 0;

    const recorder_core::RecordingDiagnosticsSnapshot* live =
        last_live_snapshot_.valid ? &last_live_snapshot_ : nullptr;

    const diagnostics::PresentSample* present_ptr = nullptr;
    diagnostics::PresentSample present_sample;
    if (present_provider_ != nullptr) {
        present_sample = present_provider_->Sample();
        if (present_sample.available) {
            present_ptr = &present_sample;
        }
    }

    diagnostics::RecommendationEngine engine(caps_, active_user_config_, monitor_refresh_hz, output_drive_free_bytes_,
                                             profile_validation_.succeeded, output_filesystem_name_, live, present_ptr);

    if (dpc_provider_ != nullptr) {
        engine.SetDpcLatency(dpc_provider_->Read());
    }

    engine.SetOutputPathWritable(diagnostics::SelfTestRunner::CheckOutputPathWritable(output_folder_.string()).passed);
    // Feed the measured process-elevation state so the Tier-4 Elevation fact reads
    // truthfully ("Elevated" vs "Standard") rather than a hard-coded baseline string.
    if (elevation_provider_ != nullptr) {
        engine.SetElevated(elevation_provider_->IsElevated());
    }
    // Feed the selected capture target's live HDR status so the HDR10 + H.264
    // pre-flight blocker (rec.hdr.h264) fires only on an HDR-active desktop.
    engine.SetCaptureTargetHdrActive(SelectedTargetHdrActive(selected_capture_target_, caps_));
    engine.SetSavedDisplayUnresolved(saved_display_unresolved_, saved_display_label_);
    engine.SetCaptureWindowEvidence(capture_window_facts_, capture_window_hub_);

    auto recs = engine.Generate();

    // Honesty rail: the verdict counts ONLY Tier-1 blockers and Tier-2 measured /
    // environment problems, read straight from each result's declared tier. Tier-3
    // optimisation tips ("better, but it runs" — bundled into the quiet tip chip by
    // refreshTopIssues) must never turn the verdict amber or promise cards that don't
    // exist; Tier-4 facts are neutral environment data (suite-diag2.jsx 'ready').
    int blockers = 0, tier2_notices = 0;
    for (const auto& r : recs.results) {
        switch (r.tier) {
        case diagnostics::DiagnosticTier::Blocker:
            ++blockers;
            break;
        case diagnostics::DiagnosticTier::MeasuredProblem:
            ++tier2_notices;
            break;
        case diagnostics::DiagnosticTier::Optimisation:
        case diagnostics::DiagnosticTier::Fact:
            break;
        }
    }
    const std::vector<diagnostics::DiagnosticResult> facts = engine.GenerateEnvironmentFacts();

    int cap_passes = 0;
    for (const auto& r : cap_summary_.entries) {
        if (r.available)
            ++cap_passes;
    }

    if (blockers > 0) {
        const QString blocker_word = (blockers == 1) ? QStringLiteral("BLOCKER") : QStringLiteral("BLOCKERS");
        status_pill_->setText(QStringLiteral("CAN'T RECORD \xc2\xb7 %1 %2").arg(blockers).arg(blocker_word));
        if (verdict_headline_)
            verdict_headline_->setText(blockers == 1
                                           ? QStringLiteral("1 thing to fix before recording")
                                           : QStringLiteral("%1 things to fix before recording").arg(blockers));
        setReadinessState(QStringLiteral("blocked"));
        const QString bw = (blockers == 1) ? QStringLiteral("blocker") : QStringLiteral("blockers");
        summary_label_->setText(
            QStringLiteral("%1 %2 must be resolved before recording. See the cards below.").arg(blockers).arg(bw));
    } else if (tier2_notices > 0) {
        const QString issue_word = (tier2_notices == 1) ? QStringLiteral("ISSUE") : QStringLiteral("ISSUES");
        status_pill_->setText(QStringLiteral("ATTENTION \xc2\xb7 %1 %2").arg(tier2_notices).arg(issue_word));
        if (verdict_headline_)
            verdict_headline_->setText(
                tier2_notices == 1 ? QStringLiteral("Recording works \xe2\x80\x94 1 thing could hurt the result")
                                   : QStringLiteral("Recording works \xe2\x80\x94 %1 things could hurt the result")
                                         .arg(tier2_notices));
        setReadinessState(QStringLiteral("warn"));
        const QString iw = (tier2_notices == 1) ? QStringLiteral("issue") : QStringLiteral("issues");
        summary_label_->setText(
            QStringLiteral("You can record, but %1 %2 could affect the result. See the cards below.")
                .arg(tier2_notices)
                .arg(iw));
    } else {
        status_pill_->setText(QStringLiteral("READY"));
        setReadinessState(QStringLiteral("ready"));
        if (verdict_headline_)
            verdict_headline_->setText(QStringLiteral("Ready to record"));
        summary_label_->setText(
            QStringLiteral("Everything checks out \xe2\x80\x94 %1 capability checks passed.").arg(cap_passes));
    }

    last_check_label_->setText(QStringLiteral("Last check: %1")
                                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd MMM yyyy, hh:mm"))));

    refreshReadinessTiles(blockers, tier2_notices, cap_passes);
    refreshTopIssues(recs);
    refreshEnvironmentFacts(facts);
}

// ── Environment facts (Tier-4, Expert panel) ────────────────────────────────────

void DiagnosticsPage::refreshEnvironmentFacts(const std::vector<diagnostics::DiagnosticResult>& facts) {
    if (!env_facts_layout_)
        return;

    QLayoutItem* child = nullptr;
    while ((child = env_facts_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    auto* host = env_facts_layout_->parentWidget();
    bool first = true;
    for (const auto& fact : facts) {
        env_facts_layout_->addWidget(makeInfoRow(QString::fromStdString(fact.title),
                                                 QString::fromStdString(fact.summary), QString(), host, first));
        first = false;
    }

    // Never leave Expert blank: if the model produced no facts, keep a truthful
    // elevation row so the panel still reads as environment. GenerateEnvironmentFacts
    // always emits fact.elevation today, so this is a defensive fallback — mirror the
    // measured state rather than a fixed string.
    if (facts.empty()) {
        const bool elevated = elevation_provider_ != nullptr && elevation_provider_->IsElevated();
        env_facts_layout_->addWidget(makeInfoRow(
            QStringLiteral("Elevation"),
            elevated ? QStringLiteral("Elevated \xe2\x80\x94 PresentMon ETW present diagnostics available")
                     : QStringLiteral("Standard \xe2\x80\x94 DXGI / NVAPI baseline \xc2\xb7 present diagnostics need "
                                      "elevation"),
            QString(), host, true));
    }
}

// ── Pipeline refresh (static readiness) ─────────────────────────────────────────

void DiagnosticsPage::refreshPipeline() {
    if (!pipeline_flow_)
        return;

    using Status = ui::widgets::PipelineStepCard::Status;

    pipeline_flow_->setStepStatus(0, Status::Planned, QStringLiteral("Live during recording."));
    pipeline_flow_->setStepStatus(1, Status::Planned, QStringLiteral("Live during recording."));
    pipeline_flow_->setStepStatus(2, Status::Planned, QStringLiteral("Live during recording."));

    if (!data_ready_) {
        pipeline_flow_->setStepStatus(3, Status::Planned, QStringLiteral("Run a check to probe the encoder."));
        pipeline_flow_->setStepStatus(4, Status::Planned, QStringLiteral("Run a check to probe the muxer."));
        pipeline_flow_->setStepStatus(5, Status::Planned, QStringLiteral("Run a check to probe the output path."));
        return;
    }

    const bool encoder_ok = capability::IsSelectable(caps_.QueryVideoCodec(active_user_config_.video_codec).level);
    pipeline_flow_->setStepStatus(
        3, encoder_ok ? Status::Ok : Status::Unavailable,
        encoder_ok ? QStringLiteral("Selected video encoder is available. Live encoder load is not measured.")
                   : QStringLiteral("Selected video codec is not available on this system."));

    const bool muxer_ok = capability::IsSelectable(caps_.QueryContainer(active_user_config_.container).level);
    pipeline_flow_->setStepStatus(
        4, muxer_ok ? Status::Ok : Status::Unavailable,
        muxer_ok ? QStringLiteral("Selected container muxer is available. Write throughput is not measured.")
                 : QStringLiteral("Selected container is not available on this system."));

    const bool disk_ok = diagnostics::SelfTestRunner::CheckOutputPathWritable(output_folder_.string()).passed;
    pipeline_flow_->setStepStatus(5, disk_ok ? Status::Ok : Status::Unavailable,
                                  disk_ok
                                      ? QStringLiteral("Output path is writable. Live disk throughput is not measured.")
                                      : QStringLiteral("Output path is not writable."));
}

} // namespace exosnap
