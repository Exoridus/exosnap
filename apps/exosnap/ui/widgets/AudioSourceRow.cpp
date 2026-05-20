#include "AudioSourceRow.h"

#include "TogglePill.h"
#include "VUMeterWidget.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {
namespace {

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", role);
    return label;
}

QFrame* makeSourceDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    divider->setProperty("frameRole", "audioSourceDivider");
    divider->setFixedHeight(34);
    return divider;
}

void restyle(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

} // namespace

AudioSourceRow::AudioSourceRow(const Config& config, QWidget* parent) : QWidget(parent) {
    setObjectName("audioSourceRow");
    setAttribute(Qt::WA_StyledBackground, true);
    setProperty("active", config.enabled);
    setMinimumHeight(96);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* grip = makeLabel("::", "audioSourceGrip", this);
    grip->setFixedWidth(22);
    grip->setAlignment(Qt::AlignCenter);

    auto* tag_label = makeLabel(config.tag, "audioSourceTag", this);
    tag_label->setAlignment(Qt::AlignCenter);
    tag_label->setMinimumWidth(96);
    tag_label->setFixedHeight(38);

    auto* text_col = new QWidget(this);
    auto* text_col_layout = new QVBoxLayout(text_col);
    text_col_layout->setContentsMargins(0, 0, 0, 0);
    text_col_layout->setSpacing(2);

    auto* title_label = makeLabel(config.title, "audioSourceTitle", text_col);
    auto* subtitle_label = makeLabel(config.subtitle, "audioSourceSubtitle", text_col);
    title_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    subtitle_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    text_col_layout->addWidget(title_label);
    text_col_layout->addWidget(subtitle_label);

    meter_ = new VUMeterWidget(this);
    meter_->setSegmentCount(22);
    meter_->setMinimumWidth(164);
    meter_->setMaximumWidth(176);

    db_label_ = makeLabel(config.db_value, "audioDbValue", this);
    db_label_->setFixedWidth(64);
    db_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    enabled_toggle_ = new TogglePill(this);
    enabled_toggle_->setOn(config.enabled);

    root->addWidget(grip);
    root->addWidget(tag_label, 0, Qt::AlignVCenter);
    root->addWidget(text_col, 1);
    root->addWidget(meter_, 0, Qt::AlignVCenter);
    root->addWidget(db_label_, 0, Qt::AlignVCenter);
    root->addWidget(makeSourceDivider(this), 0, Qt::AlignVCenter);

    if (config.has_merge_control) {
        auto* merge_widget = new QWidget(this);
        auto* merge_layout = new QHBoxLayout(merge_widget);
        merge_layout->setContentsMargins(0, 0, 0, 0);
        merge_layout->setSpacing(6);

        merge_check_ = new QCheckBox(merge_widget);
        merge_check_->setProperty("controlRole", "audioMerge");
        merge_label_ = makeLabel("Merge with above", "audioMergeLabel", merge_widget);

        merge_layout->addWidget(merge_check_);
        merge_layout->addWidget(merge_label_);
        root->addWidget(merge_widget, 0, Qt::AlignVCenter);
        root->addWidget(makeSourceDivider(this), 0, Qt::AlignVCenter);

        connect(merge_check_, &QCheckBox::toggled, this, [this](bool checked) { emit mergeChanged(checked); });
    }

    root->addWidget(enabled_toggle_, 0, Qt::AlignVCenter);

    connect(enabled_toggle_, &QPushButton::toggled, this, [this](bool enabled) {
        applyActiveState(enabled);
        emit sourceEnabledChanged(enabled);
    });

    applyActiveState(config.enabled);
}

void AudioSourceRow::setLevel(float level01) {
    meter_->setLevel(level01);
}

void AudioSourceRow::setDbText(const QString& db_text) {
    db_label_->setText(db_text);
}

void AudioSourceRow::setSourceEnabled(bool enabled) {
    enabled_toggle_->setOn(enabled);
    applyActiveState(enabled);
}

bool AudioSourceRow::isSourceEnabled() const noexcept {
    return enabled_toggle_->isOn();
}

void AudioSourceRow::setMergeChecked(bool checked) {
    if (merge_check_ != nullptr)
        merge_check_->setChecked(checked);
}

bool AudioSourceRow::mergeChecked() const noexcept {
    return merge_check_ != nullptr && merge_check_->isChecked();
}

bool AudioSourceRow::hasMergeControl() const noexcept {
    return merge_check_ != nullptr;
}

void AudioSourceRow::applyActiveState(bool active) {
    setProperty("active", active);
    meter_->setActive(active);
    if (merge_check_ != nullptr)
        merge_check_->setEnabled(active);
    if (merge_label_ != nullptr)
        merge_label_->setEnabled(active);
    restyle(this);
}

} // namespace exosnap::ui::widgets
