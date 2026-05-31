#include "GlobalRecordingBar.h"

#include "../widgets/StatusPill.h"
#include "RecordingStatusGuards.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>

namespace exosnap::ui::chrome {
namespace {

constexpr int kPresetSummaryMaxChars = 32;
constexpr int kTargetSummaryMaxChars = 40;
constexpr int kOutputSummaryMaxChars = 32;
constexpr int kRuntimeSummaryMaxChars = 36;
// Hide planned, disabled action buttons (Mic/Marker/Overlay) below this width
// to preserve transport control usability in compact layouts.
constexpr int kHidePlannedActionsBelowWidth = 1340;
// Hide secondary context slots (Output/Runtime) below this width so summaries
// degrade before transport controls at narrow widths.
constexpr int kHideSecondarySummaryBelowWidth = 1230;

QFrame* makeSeparator(QWidget* parent) {
    auto* separator = new QFrame(parent);
    separator->setObjectName("globalBarSeparator");
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedWidth(1);
    return separator;
}

QPushButton* makeActionButton(const QString& text, const QString& role, const QString& tooltip, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setProperty("role", role);
    button->setMinimumWidth(72);
    button->setMaximumWidth(118);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(tooltip);
    button->setAccessibleName(text);
    button->setAccessibleDescription(tooltip);
    button->setEnabled(false);
    return button;
}

QWidget* makeSummarySlot(const QString& key, QLabel** out_value_label, QWidget* parent) {
    auto* slot = new QWidget(parent);
    slot->setObjectName("globalBarSummarySlot");
    slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    slot->setMinimumWidth(0);
    auto* layout = new QHBoxLayout(slot);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* key_label = new QLabel(key, slot);
    key_label->setProperty("labelRole", "globalBarContextKey");
    key_label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* value_label = new QLabel(QStringLiteral("-"), slot);
    value_label->setProperty("labelRole", "globalBarContextValue");
    value_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    value_label->setMinimumWidth(0);
    QString summary_label = key;
    if (key == QStringLiteral("PRESET"))
        summary_label = QStringLiteral("Preset");
    else if (key == QStringLiteral("TARGET"))
        summary_label = QStringLiteral("Target");
    else if (key == QStringLiteral("OUTPUT"))
        summary_label = QStringLiteral("Output");
    else if (key == QStringLiteral("RUNTIME"))
        summary_label = QStringLiteral("Runtime");

    if (key == QStringLiteral("PRESET"))
        value_label->setObjectName(QStringLiteral("globalBarProfileSummaryValue"));
    else if (key == QStringLiteral("TARGET"))
        value_label->setObjectName(QStringLiteral("globalBarTargetSummaryValue"));
    else if (key == QStringLiteral("OUTPUT"))
        value_label->setObjectName(QStringLiteral("globalBarOutputSummaryValue"));
    else if (key == QStringLiteral("RUNTIME"))
        value_label->setObjectName(QStringLiteral("globalBarRuntimeSummaryValue"));

    key_label->setAccessibleName(QStringLiteral("%1 summary label").arg(summary_label));
    value_label->setAccessibleName(QStringLiteral("%1 summary value").arg(summary_label));
    value_label->setAccessibleDescription(summary_label + QStringLiteral(" summary value"));

    layout->addWidget(key_label, 0, Qt::AlignVCenter);
    layout->addWidget(value_label, 1, Qt::AlignVCenter);

    if (out_value_label)
        *out_value_label = value_label;

    return slot;
}

} // namespace

GlobalRecordingBar::GlobalRecordingBar(QWidget* parent) : QWidget(parent) {
    setObjectName("globalRecordingBar");
    setFixedHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 6, 14, 6);
    root->setSpacing(10);

    auto* status_slot = new QWidget(this);
    status_slot->setObjectName("globalBarStatusSlot");
    auto* status_layout = new QHBoxLayout(status_slot);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(0);

    status_pill_ = new ui::widgets::StatusPill(status_slot);
    status_pill_->setObjectName(QStringLiteral("globalBarStatusChip"));
    status_pill_->setText(QStringLiteral("READY"));
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
    status_pill_->setDotVisible(false);

    status_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);

    auto* actions_slot = new QWidget(this);
    actions_slot->setObjectName("globalBarActionsSlot");
    auto* actions_layout = new QHBoxLayout(actions_slot);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(6);

    primary_action_button_ = makeActionButton(QStringLiteral("Start"), QStringLiteral("globalBarPrimaryAction"),
                                              QStringLiteral("Start recording."), actions_slot);
    primary_action_button_->setObjectName(QStringLiteral("globalBarPrimaryActionButton"));
    pause_action_button_ = makeActionButton(QStringLiteral("Pause"), QStringLiteral("globalBarSecondaryAction"),
                                            QStringLiteral("Pause is available while recording."), actions_slot);
    pause_action_button_->setObjectName(QStringLiteral("globalBarPauseActionButton"));
    mic_action_button_ = makeActionButton(QStringLiteral("Mic"), QStringLiteral("globalBarSecondaryAction"),
                                          QStringLiteral("Global mic toggle is not available in this MVP build. "
                                                         "Use Audio settings to change microphone state."),
                                          actions_slot);
    mic_action_button_->setObjectName(QStringLiteral("globalBarMicActionButton"));
    marker_action_button_ =
        makeActionButton(QStringLiteral("Marker"), QStringLiteral("globalBarSecondaryAction"),
                         QStringLiteral("Markers are not available in this MVP build."), actions_slot);
    marker_action_button_->setObjectName(QStringLiteral("globalBarMarkerActionButton"));
    overlay_action_button_ =
        makeActionButton(QStringLiteral("Overlay"), QStringLiteral("globalBarSecondaryAction"),
                         QStringLiteral("Overlay/HUD controls are not available in this MVP build."), actions_slot);
    overlay_action_button_->setObjectName(QStringLiteral("globalBarOverlayActionButton"));
    overlay_action_button_->hide();

    actions_layout->addWidget(primary_action_button_);
    actions_layout->addWidget(pause_action_button_);
    actions_layout->addWidget(mic_action_button_);
    actions_layout->addWidget(marker_action_button_);
    // overlay_action_button_ is intentionally excluded from transport — Overlay/HUD is deferred.

    connect(primary_action_button_, &QPushButton::clicked, this, &GlobalRecordingBar::primaryActionRequested);
    connect(pause_action_button_, &QPushButton::clicked, this, &GlobalRecordingBar::pauseActionRequested);
    connect(mic_action_button_, &QPushButton::clicked, this, &GlobalRecordingBar::micActionRequested);
    connect(marker_action_button_, &QPushButton::clicked, this, &GlobalRecordingBar::markerActionRequested);
    connect(overlay_action_button_, &QPushButton::clicked, this, &GlobalRecordingBar::overlayActionRequested);

    auto* context_slot = new QWidget(this);
    context_slot->setObjectName("globalBarContextSlot");
    context_slot->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* context_layout = new QHBoxLayout(context_slot);
    context_layout->setContentsMargins(0, 0, 0, 0);
    context_layout->setSpacing(10);

    auto* profile_summary_slot = makeSummarySlot(QStringLiteral("PRESET"), &profile_summary_value_, context_slot);
    auto* target_summary_slot = makeSummarySlot(QStringLiteral("TARGET"), &target_summary_value_, context_slot);
    output_summary_slot_ = makeSummarySlot(QStringLiteral("OUTPUT"), &output_summary_value_, context_slot);
    runtime_summary_slot_ = makeSummarySlot(QStringLiteral("RUNTIME"), &runtime_summary_value_, context_slot);

    auto* profile_separator = makeSeparator(context_slot);
    output_separator_ = makeSeparator(context_slot);
    runtime_separator_ = makeSeparator(context_slot);

    context_layout->addWidget(profile_summary_slot, 2, Qt::AlignVCenter);
    context_layout->addWidget(profile_separator, 0, Qt::AlignVCenter);
    context_layout->addWidget(target_summary_slot, 3, Qt::AlignVCenter);
    context_layout->addWidget(output_separator_, 0, Qt::AlignVCenter);
    context_layout->addWidget(output_summary_slot_, 2, Qt::AlignVCenter);
    context_layout->addWidget(runtime_separator_, 0, Qt::AlignVCenter);
    context_layout->addWidget(runtime_summary_slot_, 2, Qt::AlignVCenter);

    root->addWidget(status_slot, 0, Qt::AlignVCenter);
    root->addWidget(makeSeparator(this), 0, Qt::AlignVCenter);
    root->addWidget(actions_slot, 0, Qt::AlignVCenter);
    root->addWidget(makeSeparator(this), 0, Qt::AlignVCenter);
    root->addWidget(context_slot, 1, Qt::AlignVCenter);

    setProfileSummary(QStringLiteral("-"));
    setTargetSummary(QStringLiteral("-"));
    setOutputSummary(QStringLiteral("-"));
    setRuntimeSummary(QStringLiteral("DUR --:--:-- · SIZE -"));
    refreshVisualState();
    applyCompactLayout();
}

void GlobalRecordingBar::setStatusLabel(const QString& status_text) {
    const QString normalized = normalizeStatusLabel(status_text);
    if (status_label_ == normalized)
        return;

    status_label_ = normalized;
    refreshVisualState();
}

void GlobalRecordingBar::refreshVisualState() {
    refreshStatusChip();
    refreshActionLabels();
    applyCompactLayout();
}

const QString& GlobalRecordingBar::statusLabel() const {
    return status_label_;
}

void GlobalRecordingBar::setProfileSummary(const QString& summary_text) {
    profile_summary_text_ = normalizeSummaryText(summary_text);
    setSummaryLabel(profile_summary_value_, profile_summary_text_, kPresetSummaryMaxChars);
}

void GlobalRecordingBar::setTargetSummary(const QString& summary_text) {
    target_summary_text_ = normalizeSummaryText(summary_text);
    setSummaryLabel(target_summary_value_, target_summary_text_, kTargetSummaryMaxChars);
}

void GlobalRecordingBar::setOutputSummary(const QString& summary_text) {
    output_summary_text_ = normalizeSummaryText(summary_text);
    setSummaryLabel(output_summary_value_, output_summary_text_, kOutputSummaryMaxChars);
}

void GlobalRecordingBar::setRuntimeSummary(const QString& summary_text) {
    runtime_summary_text_ = normalizeSummaryText(summary_text);
    setSummaryLabel(runtime_summary_value_, runtime_summary_text_, kRuntimeSummaryMaxChars);
}

void GlobalRecordingBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyCompactLayout();
    refreshSummaryLabels();
}

void GlobalRecordingBar::refreshStatusChip() {
    if (status_label_ == QStringLiteral("REC")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Recording);
        status_pill_->setDotVisible(true);
    } else if (status_label_ == QStringLiteral("READY")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
        status_pill_->setDotVisible(false);
    } else if (status_label_ == QStringLiteral("BLOCKED") || status_label_ == QStringLiteral("ERROR")) {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Blocked);
        status_pill_->setDotVisible(true);
    } else {
        status_pill_->setTone(ui::widgets::StatusPill::Tone::Warn);
        status_pill_->setDotVisible(true);
    }

    status_pill_->setText(status_label_);
    const QString tooltip = QStringLiteral("Current recording status: %1.").arg(status_label_);
    status_pill_->setToolTip(tooltip);
    status_pill_->setAccessibleName(QStringLiteral("Recording status: %1").arg(status_label_));
    status_pill_->setAccessibleDescription(tooltip);
}

void GlobalRecordingBar::refreshActionLabels() {
    const bool is_recording = (status_label_ == QStringLiteral("REC"));
    const bool is_paused = (status_label_ == QStringLiteral("PAUSED"));
    const bool has_details = (status_label_ == QStringLiteral("BLOCKED") || status_label_ == QStringLiteral("ERROR"));
    const bool is_working =
        (status_label_ == QStringLiteral("CHECKING") || status_label_ == QStringLiteral("STARTING") ||
         status_label_ == QStringLiteral("STOPPING"));
    const bool is_ready = (status_label_ == QStringLiteral("READY"));

    if (is_recording) {
        primary_action_button_->setText(QStringLiteral("Stop"));
        primary_action_button_->setToolTip(QStringLiteral("Stop recording."));
        primary_action_button_->setAccessibleName(QStringLiteral("Stop recording"));
    } else if (is_paused) {
        primary_action_button_->setText(QStringLiteral("Resume"));
        primary_action_button_->setToolTip(QStringLiteral("Resume recording."));
        primary_action_button_->setAccessibleName(QStringLiteral("Resume recording"));
    } else if (has_details) {
        primary_action_button_->setText(QStringLiteral("Details"));
        primary_action_button_->setToolTip(QStringLiteral("Open Diagnostics to review blockers and failures."));
        primary_action_button_->setAccessibleName(QStringLiteral("Open diagnostics details"));
    } else if (is_working) {
        primary_action_button_->setText(QStringLiteral("Working..."));
        primary_action_button_->setToolTip(
            QStringLiteral("State transition in progress. Action is temporarily unavailable."));
        primary_action_button_->setAccessibleName(QStringLiteral("Working state transition"));
    } else {
        primary_action_button_->setText(QStringLiteral("Start"));
        primary_action_button_->setToolTip(QStringLiteral("Start recording."));
        primary_action_button_->setAccessibleName(QStringLiteral("Start recording"));
    }
    primary_action_button_->setEnabled(is_ready || is_recording || is_paused || has_details);
    primary_action_button_->setAccessibleDescription(primary_action_button_->toolTip());

    if (is_paused) {
        pause_action_button_->setText(QStringLiteral("Paused"));
        pause_action_button_->setToolTip(QStringLiteral("Recording is paused. Use Resume to continue."));
        pause_action_button_->setAccessibleName(QStringLiteral("Recording paused"));
        pause_action_button_->setEnabled(false);
    } else {
        pause_action_button_->setText(QStringLiteral("Pause"));
        pause_action_button_->setToolTip(is_recording ? QStringLiteral("Pause recording.")
                                                      : QStringLiteral("Pause is available while recording."));
        pause_action_button_->setAccessibleName(is_recording ? QStringLiteral("Pause recording")
                                                             : QStringLiteral("Pause recording unavailable"));
        pause_action_button_->setEnabled(is_recording);
    }
    pause_action_button_->setAccessibleDescription(pause_action_button_->toolTip());

    mic_action_button_->setToolTip(QStringLiteral("Global mic toggle is not available in this MVP build. "
                                                  "Use Audio settings to change microphone state."));
    mic_action_button_->setEnabled(false);
    mic_action_button_->setAccessibleName(QStringLiteral("Microphone control unavailable"));
    mic_action_button_->setAccessibleDescription(mic_action_button_->toolTip());

    marker_action_button_->setToolTip(QStringLiteral("Markers are not available in this MVP build."));
    marker_action_button_->setEnabled(false);
    marker_action_button_->setAccessibleName(QStringLiteral("Marker control unavailable"));
    marker_action_button_->setAccessibleDescription(marker_action_button_->toolTip());

    overlay_action_button_->setToolTip(QStringLiteral("Overlay/HUD controls are not available in this MVP build."));
    overlay_action_button_->setEnabled(false);
    overlay_action_button_->setAccessibleName(QStringLiteral("Overlay control unavailable"));
    overlay_action_button_->setAccessibleDescription(overlay_action_button_->toolTip());
}

void GlobalRecordingBar::applyCompactLayout() {
    const int width_px = width();

    const bool compact_actions = width_px < kHidePlannedActionsBelowWidth;
    const bool compact_context = width_px < kHideSecondarySummaryBelowWidth;
    const bool show_runtime = ShouldShowRecordingRuntimeForStatus(status_label_);

    mic_action_button_->setVisible(!compact_actions);
    marker_action_button_->setVisible(!compact_actions);
    // overlay_action_button_ is not in the transport layout — no visibility update needed.

    output_summary_slot_->setVisible(!compact_context);
    output_separator_->setVisible(!compact_context);
    runtime_summary_slot_->setVisible(!compact_context && show_runtime);
    runtime_separator_->setVisible(!compact_context && show_runtime);
}

void GlobalRecordingBar::refreshSummaryLabels() {
    setSummaryLabel(profile_summary_value_, profile_summary_text_, kPresetSummaryMaxChars);
    setSummaryLabel(target_summary_value_, target_summary_text_, kTargetSummaryMaxChars);
    setSummaryLabel(output_summary_value_, output_summary_text_, kOutputSummaryMaxChars);
    setSummaryLabel(runtime_summary_value_, runtime_summary_text_, kRuntimeSummaryMaxChars);
}

void GlobalRecordingBar::setSummaryLabel(QLabel* label, const QString& summary_text, int max_chars) {
    if (!label)
        return;

    const QString normalized = normalizeSummaryText(summary_text);
    const QString clipped = clipSummaryText(normalized, max_chars);
    QString visible_text = clipped;
    const int available_width =
        label->contentsRect().width() > label->width() ? label->contentsRect().width() : label->width();
    if (isVisible() && label->isVisibleTo(this) && available_width > 0) {
        visible_text = label->fontMetrics().elidedText(clipped, Qt::ElideRight, available_width);
    }
    label->setText(visible_text);
    label->setToolTip(normalized);
    label->setAccessibleDescription(normalized);
}

QString GlobalRecordingBar::normalizeStatusLabel(const QString& status_text) {
    const QString normalized = status_text.trimmed().toUpper();
    if (normalized.contains(QStringLiteral("CHECK")))
        return QStringLiteral("CHECKING");
    if (normalized.contains(QStringLiteral("START")))
        return QStringLiteral("STARTING");
    if (normalized.contains(QStringLiteral("STOP")))
        return QStringLiteral("STOPPING");
    if (normalized.contains(QStringLiteral("PAUSED")))
        return QStringLiteral("PAUSED");
    if (normalized.contains(QStringLiteral("REC")))
        return QStringLiteral("REC");
    if (normalized.contains(QStringLiteral("BLOCK")))
        return QStringLiteral("BLOCKED");
    if (normalized.contains(QStringLiteral("ERROR")))
        return QStringLiteral("ERROR");
    return QStringLiteral("READY");
}

QString GlobalRecordingBar::normalizeSummaryText(const QString& summary_text) {
    const QString trimmed = summary_text.simplified();
    return trimmed.isEmpty() ? QStringLiteral("-") : trimmed;
}

QString GlobalRecordingBar::clipSummaryText(const QString& summary_text, int max_chars) {
    if (max_chars <= 0 || summary_text.size() <= max_chars)
        return summary_text;
    if (max_chars <= 3)
        return QStringLiteral("...");
    return summary_text.left(max_chars - 3) + QStringLiteral("...");
}

} // namespace exosnap::ui::chrome
