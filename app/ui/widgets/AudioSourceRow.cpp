#include "AudioSourceRow.h"

#include "ExoCheckBox.h"
#include "ExoToggle.h"
#include "VUMeterWidget.h"

#include <QAbstractButton>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>

#include <recorder_core/audio_track_model.h>

#include <cmath>

namespace exosnap::ui::widgets {
namespace {

// Slider integer range for gain: -60 to +24 dB, step 0.5 dB → 169 integer steps.
// We store tenths-of-dB (× 10) as the integer to keep QSlider simple.
constexpr int kGainSliderMin = -600; // -60.0 dB
constexpr int kGainSliderMax = +240; // +24.0 dB
constexpr int kGainSliderTick = 10;  // 1.0 dB per tick

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

    // --- Audio v2: gain slider + label + mute button ---
    gain_slider_ = new QSlider(Qt::Horizontal, this);
    gain_slider_->setRange(kGainSliderMin, kGainSliderMax);
    gain_slider_->setTickInterval(kGainSliderTick);
    gain_slider_->setValue(static_cast<int>(std::roundf(config.gain_db * 10.0f)));
    gain_slider_->setFixedWidth(80);
    gain_slider_->setToolTip("Gain (dB)");

    gain_label_ = makeLabel("0 dB", "audioGainLabel", this);
    gain_label_->setFixedWidth(52);
    gain_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto* mute_btn = new QPushButton("M", this);
    mute_btn->setCheckable(true);
    mute_btn->setChecked(config.muted);
    mute_btn->setFixedSize(28, 28);
    mute_btn->setToolTip("Mute");
    mute_btn->setProperty("buttonRole", "audioMuteButton");
    mute_button_ = mute_btn;

    // Init gain label text from initial config value.
    {
        const float g = config.gain_db;
        gain_label_->setText(QStringLiteral("%1 dB").arg(static_cast<double>(g), 0, 'f', 1));
    }

    enabled_toggle_ = new ExoToggle(this);
    enabled_toggle_->setOn(config.enabled);

    root->addWidget(grip);
    root->addWidget(tag_label, 0, Qt::AlignVCenter);
    root->addWidget(text_col, 1);
    root->addWidget(meter_, 0, Qt::AlignVCenter);
    root->addWidget(db_label_, 0, Qt::AlignVCenter);
    root->addWidget(makeSourceDivider(this), 0, Qt::AlignVCenter);

    // Gain controls
    root->addWidget(gain_slider_, 0, Qt::AlignVCenter);
    root->addWidget(gain_label_, 0, Qt::AlignVCenter);
    root->addWidget(mute_button_, 0, Qt::AlignVCenter);
    root->addWidget(makeSourceDivider(this), 0, Qt::AlignVCenter);

    if (config.has_merge_control) {
        merge_container_ = new QWidget(this);
        auto* merge_widget = merge_container_;
        auto* merge_layout = new QHBoxLayout(merge_widget);
        merge_layout->setContentsMargins(0, 0, 0, 0);
        merge_layout->setSpacing(6);

        merge_check_ = new ExoCheckBox(QString(), merge_widget);
        merge_label_ = makeLabel("Merge with above", "audioMergeLabel", merge_widget);

        merge_layout->addWidget(merge_check_);
        merge_layout->addWidget(merge_label_);
        root->addWidget(merge_container_, 0, Qt::AlignVCenter);
        root->addWidget(makeSourceDivider(this), 0, Qt::AlignVCenter);

        connect(merge_check_, &ExoCheckBox::toggled, this, [this](bool checked) { emit mergeChanged(checked); });
    }

    root->addWidget(enabled_toggle_, 0, Qt::AlignVCenter);

    connect(enabled_toggle_, &ExoToggle::toggled, this, [this](bool enabled) {
        applyActiveState(enabled);
        emit sourceEnabledChanged(enabled);
    });

    connect(gain_slider_, &QSlider::valueChanged, this, [this](int val) {
        const float db = static_cast<float>(val) / 10.0f;
        gain_label_->setText(QStringLiteral("%1 dB").arg(static_cast<double>(db), 0, 'f', 1));
        emit gainDbChanged(db);
    });

    connect(mute_button_, &QAbstractButton::toggled, this, [this](bool muted) { emit mutedChanged(muted); });

    if (!config.has_gain_control) {
        gain_slider_->hide();
        gain_label_->hide();
    }

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

void AudioSourceRow::setMergeControlVisible(bool visible) {
    if (merge_container_ != nullptr)
        merge_container_->setVisible(visible);
}

void AudioSourceRow::setGainControlVisible(bool visible) {
    if (gain_slider_ != nullptr)
        gain_slider_->setVisible(visible);
    if (gain_label_ != nullptr)
        gain_label_->setVisible(visible);
}

// Audio v2 — gain + mute

void AudioSourceRow::setGainDb(float gain_db) {
    const int val = static_cast<int>(std::roundf(gain_db * 10.0f));
    gain_slider_->setValue(val);
    // The valueChanged signal will update the label and emit gainDbChanged.
}

float AudioSourceRow::gainDb() const noexcept {
    return static_cast<float>(gain_slider_->value()) / 10.0f;
}

void AudioSourceRow::setMuted(bool muted) {
    mute_button_->setChecked(muted);
    // The toggled signal will emit mutedChanged.
}

bool AudioSourceRow::isMuted() const noexcept {
    return mute_button_->isChecked();
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
