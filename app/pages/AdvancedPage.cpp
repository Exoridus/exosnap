#include "AdvancedPage.h"

#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/SectionRuleHeader.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../ui/theme/ExoSnapAccents.h"
#include "../ui/theme/ExoSnapMetrics.h"

namespace exosnap {

namespace {

using M = ui::theme::ExoSnapMetrics;

QString ContainerLabel(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("MKV");
    case capability::Container::Mp4:
        return QStringLiteral("MP4");
    case capability::Container::WebM:
        return QStringLiteral("WebM");
    }
    return QStringLiteral("MKV");
}

QString VideoCodecLabel(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264 (NVENC)");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("HEVC (NVENC)");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1 (NVENC)");
    }
    return QStringLiteral("H.264 (NVENC)");
}

QString AudioCodecLabel(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("AAC");
    case capability::AudioCodec::Opus:
        return QStringLiteral("Opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    }
    return QStringLiteral("AAC");
}

QString QualityLabel(recorder_core::NvencQualityPreset quality) {
    switch (quality) {
    case recorder_core::NvencQualityPreset::High:
        return QStringLiteral("High  ·  CQ 19");
    case recorder_core::NvencQualityPreset::Balanced:
        return QStringLiteral("Balanced  ·  CQ 24");
    case recorder_core::NvencQualityPreset::Small:
        return QStringLiteral("Small  ·  CQ 30");
    }
    return QStringLiteral("Balanced  ·  CQ 24");
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", "subtitle");
    label->setWordWrap(true);
    return label;
}

QLabel* makeCardTitle(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", "cardTitle");
    return label;
}

QLabel* makeFieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setProperty("labelRole", "fieldLabel");
    return label;
}

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

ui::widgets::ExoCheckBox* makeCheck(const QString& text, QWidget* parent) {
    return new ui::widgets::ExoCheckBox(text, parent);
}

QWidget* makeBaselineRow(const QString& key, QLabel*& value_label, QWidget* parent) {
    auto* row = new QFrame(parent);
    row->setProperty("panelRole", "compactRow");

    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceSm);

    auto* key_label = makeFieldLabel(key, row);
    value_label = new QLabel(QStringLiteral("—"), row);
    value_label->setProperty("labelRole", "mono");

    row_layout->addWidget(key_label);
    row_layout->addStretch();
    row_layout->addWidget(value_label);
    return row;
}

} // namespace

AdvancedPage::AdvancedPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(M::kSpaceXl, M::kSpaceXl, M::kSpaceXl, M::kSpaceXl);
    layout->setSpacing(M::kSpaceLg);

    // ---- Detail header: ‹ Settings / Advanced ----
    {
        auto* header = new QWidget(content);
        header->setObjectName(QStringLiteral("detailPageHeader"));
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(4);
        auto* back_btn = new QPushButton(QString::fromUtf8("\xe2\x80\xb9 Settings"), header);
        back_btn->setProperty("role", "back");
        back_btn->setCursor(Qt::PointingHandCursor);
        auto* crumb = new QLabel(QStringLiteral("/ Advanced"), header);
        crumb->setProperty("labelRole", "detailBreadcrumb");
        hl->addWidget(back_btn);
        hl->addWidget(crumb);
        hl->addStretch(1);
        layout->addWidget(header);
        connect(back_btn, &QPushButton::clicked, this, &AdvancedPage::backToSettingsRequested);
    }

    auto* guidance_panel = new QFrame(content);
    guidance_panel->setProperty("panelRole", "note");
    auto* guidance_layout = new QVBoxLayout(guidance_panel);
    guidance_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    guidance_layout->setSpacing(M::kSpaceXs);
    guidance_layout->addWidget(makeCardTitle(QStringLiteral("Expert controls"), guidance_panel));
    guidance_layout->addWidget(makeSubLabel(
        QStringLiteral("Most users should use Settings for day-to-day recording setup. This page is for expert or "
                       "debug workflows and keeps current behavior unchanged."),
        guidance_panel));
    layout->addWidget(guidance_panel);

    auto* columns = new QWidget(content);
    auto* columns_layout = new QHBoxLayout(columns);
    columns_layout->setContentsMargins(0, 0, 0, 0);
    columns_layout->setSpacing(M::kSpaceLg);

    auto* left_col = new QWidget(columns);
    auto* left_layout = new QVBoxLayout(left_col);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(M::kSpaceSm);

    auto* right_col = new QWidget(columns);
    auto* right_layout = new QVBoxLayout(right_col);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(M::kSpaceSm);

    columns_layout->addWidget(left_col, 1);
    columns_layout->addWidget(right_col, 1);
    layout->addWidget(columns);

    auto* baseline_header = new ui::widgets::SectionRuleHeader(QStringLiteral("CURRENT PROFILE SETTINGS"), left_col);
    baseline_header->setMeta(QStringLiteral("Read-only"));
    left_layout->addWidget(baseline_header);
    left_layout->addWidget(makeSubLabel(
        QStringLiteral("Resolved settings for the active preset and profile. Use this as a baseline snapshot before "
                       "changing any expert overrides."),
        left_col));

    auto* baseline_panel = makePanel(left_col);
    auto* baseline_layout = new QVBoxLayout(baseline_panel);
    baseline_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    baseline_layout->setSpacing(M::kSpaceSm);

    auto* baseline_state = new QLabel(QStringLiteral("Baseline reflects current preset"), baseline_panel);
    baseline_state->setProperty("labelRole", "profileStatusBadge");
    baseline_state->setProperty("stateRole", "ready");
    baseline_state->setAlignment(Qt::AlignCenter);
    baseline_layout->addWidget(baseline_state, 0, Qt::AlignLeft);

    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Profile"), baseline_profile_label_, baseline_panel));
    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Container"), baseline_container_label_, baseline_panel));
    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Video codec"), baseline_video_label_, baseline_panel));
    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Quality"), baseline_quality_label_, baseline_panel));
    baseline_layout->addWidget(
        makeBaselineRow(QStringLiteral("Frame rate"), baseline_framerate_label_, baseline_panel));
    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Audio codec"), baseline_audio_label_, baseline_panel));
    baseline_layout->addWidget(makeBaselineRow(QStringLiteral("Cursor"), baseline_cursor_label_, baseline_panel));
    left_layout->addWidget(baseline_panel);
    left_layout->addStretch();

    // ---- APPEARANCE section (ACCENT-PICKER-R1) ----
    auto* appearance_header = new ui::widgets::SectionRuleHeader(QStringLiteral("APPEARANCE"), right_col);
    right_layout->addWidget(appearance_header);

    auto* appearance_panel = makePanel(right_col);
    auto* appearance_layout = new QVBoxLayout(appearance_panel);
    appearance_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    appearance_layout->setSpacing(M::kSpaceSm);

    {
        auto* accent_row = new QFrame(appearance_panel);
        accent_row->setProperty("panelRole", "compactRow");
        auto* accent_layout = new QVBoxLayout(accent_row);
        accent_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
        accent_layout->setSpacing(M::kSpaceXs);
        accent_layout->addWidget(makeFieldLabel(QStringLiteral("Accent color"), accent_row));
        accent_combo_ = new QComboBox(accent_row);
        accent_combo_->setMinimumWidth(220);
        accent_combo_->setMaximumWidth(320);
        for (const auto& a : ui::theme::kExoSnapAccents) {
            accent_combo_->addItem(QString::fromUtf8(a.name), QString::fromUtf8(a.id));
        }
        accent_layout->addWidget(accent_combo_);
        accent_layout->addWidget(makeSubLabel(
            QStringLiteral("Selects the primary accent color used throughout the app. Changes apply immediately."),
            accent_row));
        appearance_layout->addWidget(accent_row);
    }

    right_layout->addWidget(appearance_panel);

    connect(accent_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        const QString id = accent_combo_->itemData(index).toString();
        if (!id.isEmpty())
            emit accentIdChanged(id);
    });

    // ---- DEVELOPER / EXPERIMENTAL section ----
    auto* controls_header = new ui::widgets::SectionRuleHeader(QStringLiteral("DEVELOPER / EXPERIMENTAL"), right_col);
    controls_header->setMeta(QStringLiteral("Use with care"));
    right_layout->addWidget(controls_header);
    right_layout->addWidget(makeSubLabel(
        QStringLiteral("These controls are intended for debugging, profiling, and controlled experiments."),
        right_col));

    auto* controls_panel = makePanel(right_col);
    auto* controls_layout = new QVBoxLayout(controls_panel);
    controls_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    controls_layout->setSpacing(M::kSpaceSm);

    auto* logging_row = new QFrame(controls_panel);
    logging_row->setProperty("panelRole", "compactRow");
    auto* logging_layout = new QVBoxLayout(logging_row);
    logging_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    logging_layout->setSpacing(M::kSpaceXs);
    logging_layout->addWidget(makeFieldLabel(QStringLiteral("Developer logging level"), logging_row));
    log_level_combo_ = new QComboBox(logging_row);
    log_level_combo_->setMinimumWidth(220);
    log_level_combo_->setMaximumWidth(320);
    log_level_combo_->addItems({"Off", "Error", "Warning", "Info", "Debug", "Trace"});
    log_level_combo_->setCurrentIndex(3);
    logging_layout->addWidget(log_level_combo_);
    controls_layout->addWidget(logging_row);

    auto* profiling_row = new QFrame(controls_panel);
    profiling_row->setProperty("panelRole", "compactRow");
    auto* profiling_layout = new QVBoxLayout(profiling_row);
    profiling_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    profiling_layout->setSpacing(M::kSpaceXs);
    profiling_layout->addWidget(makeFieldLabel(QStringLiteral("Profiling"), profiling_row));
    nvtx_check_ = makeCheck(QStringLiteral("Enable NVTX / profiling markers"), profiling_row);
    profiling_layout->addWidget(nvtx_check_);
    controls_layout->addWidget(profiling_row);

    // RECORDING-OVERLAY-R1: overlay toggle
    auto* overlay_row = new QFrame(controls_panel);
    overlay_row->setProperty("panelRole", "compactRow");
    auto* overlay_layout = new QVBoxLayout(overlay_row);
    overlay_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    overlay_layout->setSpacing(M::kSpaceXs);
    overlay_layout->addWidget(makeFieldLabel(QStringLiteral("Recording overlay"), overlay_row));
    overlay_check_ = makeCheck(QStringLiteral("Show on-screen status overlay during recording"), overlay_row);
    overlay_check_->setChecked(true); // default ON; overridden by setShowOverlay()
    overlay_layout->addWidget(overlay_check_);
    overlay_layout->addWidget(makeSubLabel(
        QStringLiteral("A compact pill (REC \xc2\xb7 elapsed time) shown on the recorded monitor. Excluded from "
                       "capture via SetWindowDisplayAffinity — hidden if exclusion fails."),
        overlay_row));
    controls_layout->addWidget(overlay_row);

    connect(overlay_check_, &ui::widgets::ExoCheckBox::toggled, this, &AdvancedPage::showOverlayChanged);

    // DIAGNOSTICS-OVERLAY-R1 + OVERLAY-SKIN-AND-PROOF-R1: diagnostics overlay toggle
    auto* diag_overlay_row = new QFrame(controls_panel);
    diag_overlay_row->setProperty("panelRole", "compactRow");
    auto* diag_overlay_layout = new QVBoxLayout(diag_overlay_row);
    diag_overlay_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    diag_overlay_layout->setSpacing(M::kSpaceXs);

    // Header row: field label + anti-cheat ⓘ info affordance (Mappe Wave 0.3 spec)
    {
        auto* header_row = new QWidget(diag_overlay_row);
        auto* header_hl = new QHBoxLayout(header_row);
        header_hl->setContentsMargins(0, 0, 0, 0);
        header_hl->setSpacing(M::kSpaceXs);
        header_hl->addWidget(makeFieldLabel(QStringLiteral("Diagnostics overlay"), header_row));

        // ⓘ anti-cheat note — hover tooltip per ADR 0016 convention
        auto* anticheat_info = new QLabel(QString::fromUtf8("\xe2\x93\x98"), header_row); // ⓘ U+24D8
        anticheat_info->setProperty("labelRole", "infoGlyph");
        anticheat_info->setToolTip(
            QStringLiteral("Anti-cheat note: This overlay is read-only and capture-excluded "
                           "(SetWindowDisplayAffinity WDA_EXCLUDEFROMCAPTURE). It injects nothing into "
                           "any process. However, some anti-cheat systems may still flag overlays rendered "
                           "by any third-party process — disable this overlay if you encounter issues."));
        anticheat_info->setCursor(Qt::WhatsThisCursor);
        header_hl->addWidget(anticheat_info);
        header_hl->addStretch(1);

        diag_overlay_layout->addWidget(header_row);
    }

    diagnostics_overlay_check_ =
        makeCheck(QStringLiteral("Show live diagnostics on the recorded monitor during recording"), diag_overlay_row);
    diagnostics_overlay_check_->setChecked(false); // default OFF; overridden by setShowDiagnosticsOverlay()
    diag_overlay_layout->addWidget(diagnostics_overlay_check_);
    diag_overlay_layout->addWidget(makeSubLabel(
        QStringLiteral("Displays fps, A/V drift, dropped frames, output size, and muted-source indicators. "
                       "Excluded from capture via SetWindowDisplayAffinity \xe2\x80\x94 hidden if exclusion fails."),
        diag_overlay_row));
    controls_layout->addWidget(diag_overlay_row);

    connect(diagnostics_overlay_check_, &ui::widgets::ExoCheckBox::toggled, this,
            &AdvancedPage::showDiagnosticsOverlayChanged);

    // NOTIFY-TOASTS-R1: notification toasts toggle
    auto* notifications_row = new QFrame(controls_panel);
    notifications_row->setProperty("panelRole", "compactRow");
    auto* notifications_layout = new QVBoxLayout(notifications_row);
    notifications_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    notifications_layout->setSpacing(M::kSpaceXs);
    notifications_layout->addWidget(makeFieldLabel(QStringLiteral("Notifications"), notifications_row));
    notifications_check_ = makeCheck(QStringLiteral("Show on-screen notification toasts"), notifications_row);
    notifications_check_->setChecked(true); // default ON; overridden by setShowNotifications()
    notifications_layout->addWidget(notifications_check_);
    notifications_layout->addWidget(
        makeSubLabel(QStringLiteral("Transient toasts for low storage, saved recordings, and recovery. Excluded from "
                                    "capture via SetWindowDisplayAffinity."),
                     notifications_row));
    controls_layout->addWidget(notifications_row);

    connect(notifications_check_, &ui::widgets::ExoCheckBox::toggled, this, &AdvancedPage::showNotificationsChanged);

    // TRAY-CLOSE-TO-TRAY-R1: close-to-tray opt-in toggle
    auto* tray_row = new QFrame(controls_panel);
    tray_row->setProperty("panelRole", "compactRow");
    auto* tray_layout = new QVBoxLayout(tray_row);
    tray_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    tray_layout->setSpacing(M::kSpaceXs);
    tray_layout->addWidget(makeFieldLabel(QStringLiteral("Tray behavior"), tray_row));
    keep_in_tray_check_ = makeCheck(QStringLiteral("Keep running in tray when window closed"), tray_row);
    keep_in_tray_check_->setChecked(false); // default OFF; overridden by setKeepRunningInTray()
    tray_layout->addWidget(keep_in_tray_check_);
    tray_layout->addWidget(
        makeSubLabel(QStringLiteral("When enabled, closing the window hides ExoSnap to the tray instead of quitting. "
                                    "Right-click the tray icon to quit."),
                     tray_row));
    controls_layout->addWidget(tray_row);

    connect(keep_in_tray_check_, &ui::widgets::ExoCheckBox::toggled, this, &AdvancedPage::keepRunningInTrayChanged);

    // QUICK-PILL-R1: interactive quick-control pill toggle
    auto* quick_controls_row = new QFrame(controls_panel);
    quick_controls_row->setProperty("panelRole", "compactRow");
    auto* quick_controls_layout = new QVBoxLayout(quick_controls_row);
    quick_controls_layout->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
    quick_controls_layout->setSpacing(M::kSpaceXs);
    quick_controls_layout->addWidget(makeFieldLabel(QStringLiteral("Quick controls"), quick_controls_row));
    quick_controls_check_ = makeCheck(QStringLiteral("Show quick-control pill during recording"), quick_controls_row);
    quick_controls_check_->setChecked(false); // default OFF; overridden by setShowQuickControls()
    quick_controls_layout->addWidget(quick_controls_check_);
    quick_controls_layout->addWidget(makeSubLabel(
        QStringLiteral("An interactive on-screen control pill (pause, stop, capture frame) shown "
                       "over the recording target. Excluded from capture — interactive, not click-through."),
        quick_controls_row));
    controls_layout->addWidget(quick_controls_row);

    connect(quick_controls_check_, &ui::widgets::ExoCheckBox::toggled, this, &AdvancedPage::showQuickControlsChanged);

    right_layout->addWidget(controls_panel);

    auto* danger_panel = new QFrame(right_col);
    danger_panel->setProperty("panelRole", "blocker");
    auto* danger_layout = new QVBoxLayout(danger_panel);
    danger_layout->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    danger_layout->setSpacing(M::kSpaceXs);
    danger_layout->addWidget(makeCardTitle(QStringLiteral("Reset overrides"), danger_panel));
    danger_layout->addWidget(makeSubLabel(
        QStringLiteral("Resets only Advanced-page overrides to their defaults. Preset and recording behavior remain "
                       "unchanged."),
        danger_panel));

    auto* reset_btn = new QPushButton(QStringLiteral("Reset Advanced Overrides"), danger_panel);
    reset_btn->setProperty("role", "danger");
    danger_layout->addWidget(reset_btn, 0, Qt::AlignLeft);
    right_layout->addWidget(danger_panel);
    right_layout->addStretch();

    layout->addStretch();

    content->setMaximumWidth(980);
    auto* centering_host = new QWidget();
    auto* ch_layout = new QHBoxLayout(centering_host);
    ch_layout->setContentsMargins(0, 0, 0, 0);
    ch_layout->addStretch(1);
    ch_layout->addWidget(content, 0);
    ch_layout->addStretch(1);
    scroll->setWidget(centering_host);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(log_level_combo_);
    combo_wheel_filter->installOn(accent_combo_);

    connect(reset_btn, &QPushButton::clicked, this, &AdvancedPage::onReset);
}

void AdvancedPage::setBaseline(const OutputSettingsModel& output, const VideoSettingsModel& video,
                               const QString& profile_name) {
    if (baseline_profile_label_)
        baseline_profile_label_->setText(profile_name.isEmpty() ? QStringLiteral("—") : profile_name);
    if (baseline_container_label_)
        baseline_container_label_->setText(ContainerLabel(output.container));
    if (baseline_video_label_)
        baseline_video_label_->setText(VideoCodecLabel(output.video_codec));
    if (baseline_quality_label_)
        baseline_quality_label_->setText(QualityLabel(video.quality));
    if (baseline_framerate_label_) {
        const QString mode = video.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR");
        baseline_framerate_label_->setText(QStringLiteral("%1 %2 fps").arg(mode).arg(video.frame_rate_num));
    }
    if (baseline_audio_label_)
        baseline_audio_label_->setText(AudioCodecLabel(output.audio_codec));
    if (baseline_cursor_label_)
        baseline_cursor_label_->setText(video.capture_cursor ? QStringLiteral("Captured") : QStringLiteral("Hidden"));
}

void AdvancedPage::setShowOverlay(bool show) {
    if (overlay_check_)
        overlay_check_->setChecked(show);
}

void AdvancedPage::setShowDiagnosticsOverlay(bool show) {
    if (diagnostics_overlay_check_)
        diagnostics_overlay_check_->setChecked(show);
}

void AdvancedPage::setShowNotifications(bool show) {
    if (notifications_check_)
        notifications_check_->setChecked(show);
}

void AdvancedPage::setKeepRunningInTray(bool keep) {
    if (keep_in_tray_check_)
        keep_in_tray_check_->setChecked(keep);
}

void AdvancedPage::setShowQuickControls(bool show) {
    if (quick_controls_check_)
        quick_controls_check_->setChecked(show);
}

void AdvancedPage::setAccentId(const QString& accent_id) {
    if (!accent_combo_)
        return;
    for (int i = 0; i < accent_combo_->count(); ++i) {
        if (accent_combo_->itemData(i).toString() == accent_id) {
            const QSignalBlocker blocker(accent_combo_);
            accent_combo_->setCurrentIndex(i);
            return;
        }
    }
    // Unknown id: leave selection unchanged (default stays selected).
}

void AdvancedPage::onReset() {
    log_level_combo_->setCurrentIndex(3);
    nvtx_check_->setChecked(false);
    // Reset overlay to default ON and emit so MainWindow persists the change.
    if (overlay_check_)
        overlay_check_->setChecked(true);
    // Reset diagnostics overlay to default OFF and emit so MainWindow persists the change.
    if (diagnostics_overlay_check_)
        diagnostics_overlay_check_->setChecked(false);
    // Reset notifications to default ON and emit so MainWindow persists the change.
    if (notifications_check_)
        notifications_check_->setChecked(true);
    // Reset keep-in-tray to default OFF and emit so MainWindow persists the change.
    if (keep_in_tray_check_)
        keep_in_tray_check_->setChecked(false);
    // Reset quick controls to default OFF and emit so MainWindow persists the change.
    if (quick_controls_check_)
        quick_controls_check_->setChecked(false);
    // ACCENT-PICKER-R1: reset accent to default (mint) and emit so MainWindow persists + re-applies.
    if (accent_combo_) {
        for (int i = 0; i < accent_combo_->count(); ++i) {
            if (accent_combo_->itemData(i).toString() == QLatin1String("mint")) {
                accent_combo_->setCurrentIndex(i);
                break;
            }
        }
    }
}

} // namespace exosnap
