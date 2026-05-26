#include "GlobalRecordingBar.h"

#include "../widgets/StatusPill.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>

namespace exosnap::ui::chrome {
namespace {

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
    button->setEnabled(false);
    return button;
}

QWidget* makeSummarySlot(const QString& key, QLabel** out_value_label, QWidget* parent) {
    auto* slot = new QWidget(parent);
    slot->setObjectName("globalBarSummarySlot");
    auto* layout = new QHBoxLayout(slot);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* key_label = new QLabel(key, slot);
    key_label->setProperty("labelRole", "globalBarContextKey");
    auto* value_label = new QLabel(QStringLiteral("-"), slot);
    value_label->setProperty("labelRole", "globalBarContextValue");
    if (key == QStringLiteral("PROFILE"))
        value_label->setObjectName(QStringLiteral("globalBarProfileSummaryValue"));
    else if (key == QStringLiteral("TARGET"))
        value_label->setObjectName(QStringLiteral("globalBarTargetSummaryValue"));
    else if (key == QStringLiteral("OUTPUT"))
        value_label->setObjectName(QStringLiteral("globalBarOutputSummaryValue"));
    else if (key == QStringLiteral("RUNTIME"))
        value_label->setObjectName(QStringLiteral("globalBarRuntimeSummaryValue"));

    layout->addWidget(key_label, 0, Qt::AlignVCenter);
    layout->addWidget(value_label, 0, Qt::AlignVCenter);

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
    status_pill_->setText(QStringLiteral("READY"));
    status_pill_->setTone(ui::widgets::StatusPill::Tone::Ready);
    status_pill_->setDotVisible(false);

    status_layout->addWidget(status_pill_, 0, Qt::AlignVCenter);

    auto* actions_slot = new QWidget(this);
    actions_slot->setObjectName("globalBarActionsSlot");
    auto* actions_layout = new QHBoxLayout(actions_slot);
    actions_layout->setContentsMargins(0, 0, 0, 0);
    actions_layout->setSpacing(6);

    primary_action_button_ =
        makeActionButton(QStringLiteral("Start"), QStringLiteral("globalBarPrimaryAction"),
                         QStringLiteral("Global start/stop action (wiring in next commit)."), actions_slot);
    primary_action_button_->setObjectName(QStringLiteral("globalBarPrimaryActionButton"));
    pause_action_button_ =
        makeActionButton(QStringLiteral("Pause"), QStringLiteral("globalBarSecondaryAction"),
                         QStringLiteral("Pause or resume recording globally (coming soon)."), actions_slot);
    pause_action_button_->setObjectName(QStringLiteral("globalBarPauseActionButton"));
    mic_action_button_ = makeActionButton(QStringLiteral("Mic"), QStringLiteral("globalBarSecondaryAction"),
                                          QStringLiteral("Global microphone control (coming soon)."), actions_slot);
    mic_action_button_->setObjectName(QStringLiteral("globalBarMicActionButton"));
    marker_action_button_ = makeActionButton(QStringLiteral("Marker"), QStringLiteral("globalBarSecondaryAction"),
                                             QStringLiteral("Add recording marker (coming soon)."), actions_slot);
    marker_action_button_->setObjectName(QStringLiteral("globalBarMarkerActionButton"));
    overlay_action_button_ =
        makeActionButton(QStringLiteral("Overlay"), QStringLiteral("globalBarSecondaryAction"),
                         QStringLiteral("Toggle overlay/HUD visibility (coming soon)."), actions_slot);
    overlay_action_button_->setObjectName(QStringLiteral("globalBarOverlayActionButton"));

    actions_layout->addWidget(primary_action_button_);
    actions_layout->addWidget(pause_action_button_);
    actions_layout->addWidget(mic_action_button_);
    actions_layout->addWidget(marker_action_button_);
    actions_layout->addWidget(overlay_action_button_);

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

    auto* profile_summary_slot = makeSummarySlot(QStringLiteral("PROFILE"), &profile_summary_value_, context_slot);
    auto* target_summary_slot = makeSummarySlot(QStringLiteral("TARGET"), &target_summary_value_, context_slot);
    output_summary_slot_ = makeSummarySlot(QStringLiteral("OUTPUT"), &output_summary_value_, context_slot);
    runtime_summary_slot_ = makeSummarySlot(QStringLiteral("RUNTIME"), &runtime_summary_value_, context_slot);

    auto* profile_separator = makeSeparator(context_slot);
    output_separator_ = makeSeparator(context_slot);
    runtime_separator_ = makeSeparator(context_slot);

    context_layout->addWidget(profile_summary_slot, 0, Qt::AlignVCenter);
    context_layout->addWidget(profile_separator, 0, Qt::AlignVCenter);
    context_layout->addWidget(target_summary_slot, 0, Qt::AlignVCenter);
    context_layout->addWidget(output_separator_, 0, Qt::AlignVCenter);
    context_layout->addWidget(output_summary_slot_, 0, Qt::AlignVCenter);
    context_layout->addWidget(runtime_separator_, 0, Qt::AlignVCenter);
    context_layout->addWidget(runtime_summary_slot_, 0, Qt::AlignVCenter);
    context_layout->addStretch(1);

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
}

QString GlobalRecordingBar::statusLabel() const {
    return status_label_;
}

void GlobalRecordingBar::setProfileSummary(const QString& summary_text) {
    setSummaryLabel(profile_summary_value_, summary_text, 28);
}

void GlobalRecordingBar::setTargetSummary(const QString& summary_text) {
    setSummaryLabel(target_summary_value_, summary_text, 34);
}

void GlobalRecordingBar::setOutputSummary(const QString& summary_text) {
    setSummaryLabel(output_summary_value_, summary_text, 24);
}

void GlobalRecordingBar::setRuntimeSummary(const QString& summary_text) {
    setSummaryLabel(runtime_summary_value_, summary_text, 28);
}

void GlobalRecordingBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyCompactLayout();
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
    } else if (is_paused) {
        primary_action_button_->setText(QStringLiteral("Resume"));
        primary_action_button_->setToolTip(QStringLiteral("Resume recording."));
    } else if (has_details) {
        primary_action_button_->setText(QStringLiteral("Details"));
        primary_action_button_->setToolTip(QStringLiteral("Open diagnostics details."));
    } else if (is_working) {
        primary_action_button_->setText(QStringLiteral("Working..."));
        primary_action_button_->setToolTip(QStringLiteral("Action unavailable while state transition is in progress."));
    } else {
        primary_action_button_->setText(QStringLiteral("Start"));
        primary_action_button_->setToolTip(QStringLiteral("Start recording."));
    }
    primary_action_button_->setEnabled(is_ready || is_recording || is_paused || has_details);

    if (is_paused) {
        pause_action_button_->setText(QStringLiteral("Paused"));
        pause_action_button_->setToolTip(QStringLiteral("Recording is paused. Use Resume to continue."));
        pause_action_button_->setEnabled(false);
    } else {
        pause_action_button_->setText(QStringLiteral("Pause"));
        pause_action_button_->setToolTip(QStringLiteral("Pause recording."));
        pause_action_button_->setEnabled(is_recording);
    }

    mic_action_button_->setToolTip(QStringLiteral("Global mic toggle is not exposed in MVP."));
    mic_action_button_->setEnabled(false);
    marker_action_button_->setToolTip(QStringLiteral("Global marker action is planned for a future milestone."));
    marker_action_button_->setEnabled(false);
    overlay_action_button_->setToolTip(QStringLiteral("Overlay/HUD controls are not available in MVP."));
    overlay_action_button_->setEnabled(false);
}

void GlobalRecordingBar::applyCompactLayout() {
    const int width_px = width();

    const bool compact_actions = width_px < 1340;
    const bool compact_context = width_px < 1230;

    marker_action_button_->setVisible(!compact_actions);
    overlay_action_button_->setVisible(!compact_actions);

    output_summary_slot_->setVisible(!compact_context);
    output_separator_->setVisible(!compact_context);
    runtime_summary_slot_->setVisible(!compact_context);
    runtime_separator_->setVisible(!compact_context);
}

void GlobalRecordingBar::setSummaryLabel(QLabel* label, const QString& summary_text, int max_chars) {
    if (!label)
        return;

    const QString normalized = normalizeSummaryText(summary_text);
    label->setText(clipSummaryText(normalized, max_chars));
    label->setToolTip(normalized);
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
