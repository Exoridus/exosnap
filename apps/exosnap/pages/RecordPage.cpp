#include "RecordPage.h"

#include "../diagnostics/AppLog.h"
#include "../settings/AppSettingsStore.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/CaptureTargetCard.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/PreviewSurface.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include "../ui/widgets/StatusPill.h"
#include "../ui/widgets/VUMeterWidget.h"

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <recorder_core/audio_input_device.h>

#include <QComboBox>
#include <QDebug>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include <exception>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>

namespace exosnap {
namespace {

QFrame* makePanel(QWidget* parent, const char* role = "panel") {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", role);
    return panel;
}

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setProperty("labelRole", role);
    return label;
}

void setStyledStringProperty(QWidget* widget, const char* property_name, const QString& value) {
    if (widget->property(property_name).toString() == value)
        return;
    widget->setProperty(property_name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString stateDisplay(UiRecordingState state) {
    switch (state) {
    case UiRecordingState::LoadingCapabilities:
        return "CHECKING";
    case UiRecordingState::Ready:
        return "READY";
    case UiRecordingState::Blocked:
        return "BLOCKED";
    case UiRecordingState::Preparing:
        return "STARTING";
    case UiRecordingState::Recording:
        return "RECORDING";
    case UiRecordingState::Stopping:
        return "RECORDING";
    case UiRecordingState::Completed:
        return "READY";
    case UiRecordingState::Failed:
        return "FAILED";
    default:
        return "BLOCKED";
    }
}

QString toClock(const std::wstring& elapsed_text) {
    const QString text = QString::fromStdWString(elapsed_text);
    const QStringList parts = text.split(':');
    bool ok_a = false;
    bool ok_b = false;
    int minutes = 0;
    int seconds = 0;
    if (parts.size() == 2) {
        minutes = parts[0].toInt(&ok_a);
        seconds = parts[1].toInt(&ok_b);
    }
    if (!ok_a || !ok_b)
        return "00:00:00";

    const int hours = minutes / 60;
    const int rem_minutes = minutes % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(rem_minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

QString checkGlyph(bool ok, bool blocked) {
    if (ok)
        return "✓";
    if (blocked)
        return "✕";
    return "·";
}

int LinearToSliderDb(float gain_linear) {
    if (gain_linear <= 0.0f)
        return 0;
    const int db = static_cast<int>(std::round(20.0f * std::log10f(gain_linear)));
    return std::clamp(db, 0, 24);
}

float SliderDbToLinear(int db) {
    return std::pow(10.0f, static_cast<float>(db) / 20.0f);
}

void PersistMicGainForMvp(float gain_linear) {
    AppSettingsStore store;
    PersistedAppSettings persisted = store.Load();
    persisted.audio_ui_state.mic_gain_linear = gain_linear;
    store.Save(persisted);
}

QString displayLabelFromTarget(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::DisplayLabelFromTarget(target.description));
}

QString windowLabelFromTarget(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::WindowLabelFromTarget(target.description));
}

QString normalizedTargetLabel(const recorder_core::CaptureTarget& target) {
    return QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target));
}

capability::UserRecorderConfig primaryRecorderConfig() {
    capability::UserRecorderConfig config;
    config.container = capability::Container::Matroska;
    config.video_codec = capability::VideoCodec::Av1Nvenc;
    config.audio_codec = capability::AudioCodec::Opus;
    config.chroma = capability::ChromaSubsampling::Cs420;
    config.bit_depth = capability::BitDepth::Bit8;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;
    return config;
}

std::vector<std::string> BuildMicDeviceLabels(const std::vector<recorder_core::AudioInputDeviceInfo>& devices) {
    std::vector<std::string> base_names;
    base_names.reserve(devices.size());

    std::unordered_map<std::string, int> base_counts;
    for (const auto& dev : devices) {
        const std::string base_name = dev.display_name.empty() ? dev.device_id : dev.display_name;
        base_names.push_back(base_name);
        ++base_counts[base_name];
    }

    std::unordered_map<std::string, int> base_seen;
    std::vector<std::string> labels;
    labels.reserve(devices.size());

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        const std::string& base_name = base_names[i];

        std::string label = base_name;
        if (base_counts[base_name] > 1) {
            const int dedup_index = ++base_seen[base_name];
            label += " [" + std::to_string(dedup_index) + "]";
        }

        if (dev.is_default) {
            label += " (Default)";
        }

        labels.push_back(std::move(label));
    }

    return labels;
}

} // namespace

RecordPage::RecordPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(18);

    capability_label_ = makeLabel("", "recordCapabilityBanner", content);
    capability_label_->setWordWrap(true);
    capability_label_->setProperty("panelRole", "note");
    capability_label_->setVisible(false);
    layout->addWidget(capability_label_);

    auto* hero_grid = new QGridLayout();
    hero_grid->setHorizontalSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);
    hero_grid->setVerticalSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);
    hero_grid->setColumnStretch(0, 5);
    hero_grid->setColumnStretch(1, 2);
    layout->addLayout(hero_grid);

    preview_surface_ = new ui::widgets::PreviewSurface(content);
    hero_grid->addWidget(preview_surface_, 0, 0, 1, 1);

    auto* rail_column = new QWidget(content);
    auto* rail_layout = new QVBoxLayout(rail_column);
    rail_layout->setContentsMargins(0, 0, 0, 0);
    rail_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceMd);

    auto* control_panel = makePanel(rail_column);
    control_panel->setObjectName("recordControlPanel");
    auto* control_layout = new QVBoxLayout(control_panel);
    control_layout->setContentsMargins(14, 14, 14, 14);
    control_layout->setSpacing(14);

    auto* control_head = new QHBoxLayout();
    control_head->setContentsMargins(0, 0, 0, 0);
    control_head->setSpacing(8);
    control_state_label_ = makeLabel("READY", "recordControlState", control_panel);
    auto* hotkey_label = makeLabel("ALT+F9", "recordHotkeyBadge", control_panel);
    hotkey_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_head->addWidget(control_state_label_);
    control_head->addStretch(1);
    control_head->addWidget(hotkey_label);
    control_layout->addLayout(control_head);

    start_btn_ = new QPushButton("▶  START", control_panel);
    start_btn_->setProperty("role", "primaryRecordStart");
    start_btn_->setMinimumHeight(46);
    control_layout->addWidget(start_btn_);

    stop_btn_ = new QPushButton("■  STOP", control_panel);
    stop_btn_->setProperty("role", "recordStop");
    stop_btn_->setMinimumHeight(46);
    stop_btn_->hide();
    control_layout->addWidget(stop_btn_);

    timer_label_ = makeLabel("00:00:00", "recordTimer", control_panel);
    timer_label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    timer_label_->setProperty("timerState", "idle");
    control_layout->addWidget(timer_label_);

    rail_layout->addWidget(control_panel);
    rail_layout->addStretch(1);

    hero_grid->addWidget(rail_column, 0, 1, 1, 1);

    capture_header_ = new ui::widgets::SectionRuleHeader("CAPTURE TARGET", content);
    capture_header_->setMeta("DISPLAY1 · 2560×1440 · 60 fps");
    layout->addWidget(capture_header_);

    auto* cards_row = new QWidget(content);
    auto* cards_layout = new QHBoxLayout(cards_row);
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceSm);
    monitor_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    monitor_card_->setTitle("Monitor");
    window_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    window_card_->setTitle("Window");
    cards_layout->addWidget(monitor_card_, 1);
    cards_layout->addWidget(window_card_, 1);
    monitor_card_->setAccessibleName("Monitor target");
    window_card_->setAccessibleName("Window target");
    QWidget::setTabOrder(monitor_card_, window_card_);
    layout->addWidget(cards_row);

    target_picker_panel_ = makePanel(content);
    target_picker_panel_->setObjectName("captureTargetPickerPanel");
    auto* target_picker_layout = new QVBoxLayout(target_picker_panel_);
    target_picker_layout->setContentsMargins(14, 12, 14, 12);
    target_picker_layout->setSpacing(8);

    auto* target_picker_row = new QWidget(target_picker_panel_);
    auto* target_picker_row_layout = new QHBoxLayout(target_picker_row);
    target_picker_row_layout->setContentsMargins(0, 0, 0, 0);
    target_picker_row_layout->setSpacing(10);
    target_picker_kind_label_ = makeLabel("Display", "captureTargetPickerLabel", target_picker_row);
    target_picker_combo_ = new QComboBox(target_picker_row);
    target_picker_combo_->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    target_refresh_btn_ = new QPushButton("Refresh", target_picker_row);
    target_refresh_btn_->setProperty("role", "ghost");
    target_picker_row_layout->addWidget(target_picker_kind_label_);
    target_picker_row_layout->addWidget(target_picker_combo_, 1);
    target_picker_row_layout->addWidget(target_refresh_btn_);
    target_picker_layout->addWidget(target_picker_row);

    target_picker_note_label_ = makeLabel("", "captureTargetPickerNote", target_picker_panel_);
    target_picker_note_label_->setWordWrap(true);
    target_picker_note_label_->setVisible(false);
    target_picker_layout->addWidget(target_picker_note_label_);
    layout->addWidget(target_picker_panel_);

    target_combo_ = new QComboBox(content);
    target_combo_->setVisible(false);
    target_combo_->setEnabled(false);

    readiness_header_ = new ui::widgets::SectionRuleHeader("READINESS", content);
    readiness_header_->setMeta("ALL CLEAR");
    layout->addWidget(readiness_header_);

    readiness_panel_ = makePanel(content);
    auto* readiness_layout = new QVBoxLayout(readiness_panel_);
    readiness_layout->setContentsMargins(0, 0, 0, 0);
    readiness_layout->setSpacing(0);
    for (const QString& title :
         {QString("NVENC AV1 encoder"), QString("Display capture"), QString("Audio loopback (APP)"),
          QString("Output destination"), QString("Session state")}) {
        auto* row = new QWidget(readiness_panel_);
        row->setObjectName("readinessRow");
        row->setProperty("firstRow", readiness_rows_.empty());
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(14, 10, 14, 10);
        row_layout->setSpacing(10);

        auto* icon = makeLabel("·", "readinessGlyph", row);
        icon->setFixedWidth(12);
        auto* row_title = makeLabel(title, "readinessTitle", row);
        auto* detail = makeLabel("", "readinessDetail", row);
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        detail->setWordWrap(false);

        row_layout->addWidget(icon);
        row_layout->addWidget(row_title);
        row_layout->addStretch(1);
        row_layout->addWidget(detail, 0, Qt::AlignRight | Qt::AlignVCenter);
        readiness_layout->addWidget(row);

        readiness_rows_.push_back({icon, row_title, detail});
    }
    layout->addWidget(readiness_panel_);

    audio_settings_header_ = new ui::widgets::SectionRuleHeader("AUDIO SETTINGS", content);
    audio_settings_header_->setMeta("OUTPUT · INPUT · TRACK PREVIEW");
    layout->addWidget(audio_settings_header_);

    auto* audio_settings_panel = makePanel(content);
    auto* audio_settings_layout = new QVBoxLayout(audio_settings_panel);
    audio_settings_layout->setContentsMargins(14, 12, 14, 12);
    audio_settings_layout->setSpacing(10);

    audio_settings_layout->addWidget(makeLabel("Audio Output", "audioSettingsGroupTitle", audio_settings_panel));
    app_audio_check_ = new ui::widgets::ExoCheckBox("Record Application Audio", audio_settings_panel);
    sys_audio_check_ = new ui::widgets::ExoCheckBox("Record System Audio", audio_settings_panel);
    separate_tracks_check_ = new ui::widgets::ExoCheckBox("Separate output tracks", audio_settings_panel);
    audio_settings_layout->addWidget(app_audio_check_);
    audio_settings_layout->addWidget(sys_audio_check_);
    audio_settings_layout->addWidget(separate_tracks_check_);

    audio_settings_layout->addSpacing(6);
    audio_settings_layout->addWidget(makeLabel("Audio Input", "audioSettingsGroupTitle", audio_settings_panel));
    mic_check_ = new ui::widgets::ExoCheckBox("Record Microphone", audio_settings_panel);
    audio_settings_layout->addWidget(mic_check_);

    mic_device_row_ = new QWidget(audio_settings_panel);
    auto* mic_device_row_layout = new QHBoxLayout(mic_device_row_);
    mic_device_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_device_row_layout->setSpacing(10);
    mic_device_row_layout->addWidget(makeLabel("Input Device", "audioSettingsRowLabel", mic_device_row_));
    mic_device_combo_ = new QComboBox(mic_device_row_);
    mic_device_row_layout->addWidget(mic_device_combo_, 1);
    mic_refresh_btn_ = new QPushButton("Refresh", mic_device_row_);
    mic_device_row_layout->addWidget(mic_refresh_btn_);
    populateMicDeviceCombo();
    audio_settings_layout->addWidget(mic_device_row_);
    mic_device_note_label_ = makeLabel("", "audioSettingsNote", audio_settings_panel);
    mic_device_note_label_->setProperty("labelRole", "audioSettingsNote");
    mic_device_note_label_->setWordWrap(true);
    mic_device_note_label_->setVisible(false);
    audio_settings_layout->addWidget(mic_device_note_label_);

    mic_channel_row_ = new QWidget(audio_settings_panel);
    auto* mic_channel_row_layout = new QHBoxLayout(mic_channel_row_);
    mic_channel_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_channel_row_layout->setSpacing(10);
    mic_channel_row_layout->addWidget(makeLabel("Channel", "audioSettingsRowLabel", mic_channel_row_));
    mic_channel_combo_ = new QComboBox(mic_channel_row_);
    mic_channel_combo_->addItem("Auto");
    mic_channel_combo_->addItem("Preserve Stereo");
    mic_channel_combo_->addItem("Mono Mix");
    mic_channel_combo_->addItem("Left to Stereo");
    mic_channel_combo_->addItem("Right to Stereo");
    mic_channel_row_layout->addWidget(mic_channel_combo_, 1);
    audio_settings_layout->addWidget(mic_channel_row_);

    mic_gain_row_ = new QWidget(audio_settings_panel);
    auto* mic_gain_row_layout = new QHBoxLayout(mic_gain_row_);
    mic_gain_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_gain_row_layout->setSpacing(10);
    mic_gain_row_layout->addWidget(makeLabel("Gain", "audioSettingsRowLabel", mic_gain_row_));
    mic_gain_slider_ = new QSlider(Qt::Horizontal, mic_gain_row_);
    mic_gain_slider_->setRange(0, 24);
    mic_gain_slider_->setSingleStep(1);
    mic_gain_slider_->setPageStep(3);
    mic_gain_slider_->setTickInterval(6);
    mic_gain_slider_->setTickPosition(QSlider::TicksBelow);
    mic_gain_slider_->setValue(0);
    mic_gain_row_layout->addWidget(mic_gain_slider_, 1);
    mic_gain_value_label_ = makeLabel("0 dB", "audioSettingsRowLabel", mic_gain_row_);
    mic_gain_value_label_->setFixedWidth(54);
    mic_gain_value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mic_gain_row_layout->addWidget(mic_gain_value_label_);
    audio_settings_layout->addWidget(mic_gain_row_);

    audio_settings_layout->addSpacing(6);
    audio_settings_layout->addWidget(makeLabel("Resulting Tracks", "audioSettingsGroupTitle", audio_settings_panel));
    track_preview_panel_ = makePanel(audio_settings_panel);
    track_preview_panel_->setObjectName("resultingTracksPanel");
    track_preview_layout_ = new QVBoxLayout(track_preview_panel_);
    track_preview_layout_->setContentsMargins(0, 0, 0, 0);
    track_preview_layout_->setSpacing(0);
    audio_settings_layout->addWidget(track_preview_panel_);

    layout->addWidget(audio_settings_panel);

    audio_header_ = new ui::widgets::SectionRuleHeader("AUDIO ACTIVITY", content);
    audio_header_->setMeta("LIVE · RMS");
    layout->addWidget(audio_header_);

    auto* audio_panel = makePanel(content);
    auto* audio_layout = new QVBoxLayout(audio_panel);
    audio_layout->setContentsMargins(0, 0, 0, 0);
    audio_layout->setSpacing(0);

    auto addAudioRow = [&](const QString& tag, const QString& title, ui::widgets::VUMeterWidget** meter_out,
                           QLabel** db_label_out) {
        auto* row = new QWidget(audio_panel);
        row->setObjectName("audioActivityRow");
        row->setProperty("firstRow", (*meter_out == nullptr));
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(14, 9, 14, 9);
        row_layout->setSpacing(12);

        auto* tag_label = makeLabel(tag, "audioTag", row);
        tag_label->setFixedWidth(44);
        row_layout->addWidget(tag_label);

        auto* title_label = makeLabel(title, "audioRowTitle", row);
        row_layout->addWidget(title_label, 1);

        auto* meter = new ui::widgets::VUMeterWidget(row);
        meter->setSegmentCount(24);
        row_layout->addWidget(meter, 1);

        auto* db_label = makeLabel("– dB", "audioDb", row);
        db_label->setFixedWidth(118);
        db_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row_layout->addWidget(db_label);

        audio_layout->addWidget(row);

        *meter_out = meter;
        *db_label_out = db_label;
    };

    addAudioRow("APP", "Selected application audio", &app_meter_, &app_db_label_);
    addAudioRow("MIC", "Microphone input", &mic_meter_, &mic_db_label_);
    addAudioRow("SYS", "Other system audio", &sys_meter_, &sys_db_label_);
    layout->addWidget(audio_panel);

    destination_header_ = new ui::widgets::SectionRuleHeader("DESTINATION", content);
    destination_header_->setMeta("MKV · AV1 · OPUS");
    layout->addWidget(destination_header_);

    auto* destination_panel = makePanel(content);
    auto* destination_layout = new QHBoxLayout(destination_panel);
    destination_layout->setContentsMargins(14, 12, 14, 12);
    destination_layout->setSpacing(10);

    auto* dest_left = new QWidget(destination_panel);
    auto* dest_left_layout = new QVBoxLayout(dest_left);
    dest_left_layout->setContentsMargins(0, 0, 0, 0);
    dest_left_layout->setSpacing(2);
    output_path_label_ = makeLabel("--", "destinationPath", dest_left);
    output_meta_label_ = makeLabel("No file saved yet — configure in Output settings.", "destinationMeta", dest_left);
    output_meta_label_->setWordWrap(true);
    dest_left_layout->addWidget(output_path_label_);
    dest_left_layout->addWidget(output_meta_label_);

    auto* dest_buttons = new QWidget(destination_panel);
    auto* dest_buttons_layout = new QHBoxLayout(dest_buttons);
    dest_buttons_layout->setContentsMargins(0, 0, 0, 0);
    dest_buttons_layout->setSpacing(6);
    open_folder_btn_ = new QPushButton("Open Folder", dest_buttons);
    open_folder_btn_->setProperty("role", "ghost");
    open_folder_btn_->setEnabled(false);
    destination_settings_btn_ = new QPushButton("Settings", dest_buttons);
    destination_settings_btn_->setProperty("role", "ghost");
    destination_settings_btn_->setEnabled(true);
    dest_buttons_layout->addWidget(open_folder_btn_);
    dest_buttons_layout->addWidget(destination_settings_btn_);

    destination_layout->addWidget(dest_left, 1);
    destination_layout->addWidget(dest_buttons, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(destination_panel);

    result_panel_ = makePanel(content, "resultPanel");
    auto* result_layout = new QVBoxLayout(result_panel_);
    result_layout->setContentsMargins(14, 12, 14, 12);
    result_layout->setSpacing(6);
    result_title_label_ = makeLabel("", "resultTitleOk", result_panel_);
    result_message_label_ = makeLabel("", "resultUserMessage", result_panel_);
    result_action_label_ = makeLabel("", "resultActionHint", result_panel_);
    result_stats_label_ = makeLabel("", "resultStats", result_panel_);
    result_path_label_ = makeLabel("", "resultPath", result_panel_);
    result_technical_separator_ = new QFrame(result_panel_);
    result_technical_separator_->setFrameShape(QFrame::NoFrame);
    result_technical_separator_->setFixedHeight(1);
    result_technical_separator_->setProperty("frameRole", "resultTechnicalSeparator");
    result_technical_label_ = makeLabel("", "resultTechnical", result_panel_);
    result_message_label_->setWordWrap(true);
    result_action_label_->setWordWrap(true);
    result_path_label_->setWordWrap(true);
    result_technical_label_->setWordWrap(true);
    result_layout->addWidget(result_title_label_);
    result_layout->addWidget(result_message_label_);
    result_layout->addWidget(result_action_label_);
    result_layout->addWidget(result_stats_label_);
    result_layout->addWidget(result_path_label_);
    result_layout->addWidget(result_technical_separator_);
    result_layout->addWidget(result_technical_label_);
    result_technical_separator_->setVisible(false);
    result_technical_label_->setVisible(false);
    result_panel_->setVisible(false);
    layout->addWidget(result_panel_);

    layout->addStretch(1);
    scroll->setWidget(content);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(scroll);

    connect(start_btn_, &QPushButton::clicked, this, &RecordPage::onStart);
    connect(stop_btn_, &QPushButton::clicked, this, &RecordPage::onStop);
    connect(monitor_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectMonitorTarget);
    connect(window_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectWindowTarget);
    connect(target_picker_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onTargetPickerChanged);
    connect(target_refresh_btn_, &QPushButton::clicked, this, &RecordPage::onRefreshTargets);
    connect(app_audio_check_, &ui::widgets::ExoCheckBox::toggled, this, &RecordPage::onAppAudioToggled);
    connect(sys_audio_check_, &ui::widgets::ExoCheckBox::toggled, this, &RecordPage::onSysAudioToggled);
    connect(separate_tracks_check_, &ui::widgets::ExoCheckBox::toggled, this, &RecordPage::onSeparateTracksToggled);
    connect(mic_check_, &ui::widgets::ExoCheckBox::toggled, this, &RecordPage::onMicToggled);
    connect(mic_refresh_btn_, &QPushButton::clicked, this, &RecordPage::populateMicDeviceCombo);
    connect(mic_device_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onMicDeviceChanged);
    connect(mic_channel_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onMicChannelChanged);
    connect(mic_gain_slider_, &QSlider::valueChanged, this, &RecordPage::onMicGainChanged);
    connect(open_folder_btn_, &QPushButton::clicked, this, &RecordPage::openOutputFolder);
    connect(destination_settings_btn_, &QPushButton::clicked, this, [this]() { emit navigateToOutputPage(); });

    initCoordinator();
}

RecordPage::~RecordPage() {
    if (preview_service_)
        preview_service_->Stop();
    if (coordinator_) {
        coordinator_->StopMicMeter();
    }
}

void RecordPage::setOutputSettings(const OutputSettingsModel& settings) {
    current_container_ = settings.container;
    last_output_folder_ = settings.output_folder;
    if (!settings.output_folder.empty()) {
        view_model_.output_path_display = settings.output_folder.wstring();
        if (output_path_label_)
            output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));
    }
    setOutputSettingsSummary(settings);
    if (coordinator_) {
        coordinator_->SetOutputSettings(settings);
        syncCoordinatorTargetContext();
    }
    updateOpenFolderButtonState();
    updateAudioControls();
    diagnostics::AppLog(QStringLiteral("[output] settings applied: ") +
                        QString::fromStdWString(view_model_.output_path_display));
}

void RecordPage::applyPersistedAudioSettings(const capability::AudioUiState& state) {
    const capability::CaptureTargetKind target_kind = view_model_.audio_ui_state.target_kind;
    const std::optional<uint32_t> selected_window_pid = view_model_.audio_ui_state.selected_window_pid;

    view_model_.audio_ui_state.record_application_audio = state.record_application_audio;
    view_model_.audio_ui_state.record_system_audio = state.record_system_audio;
    view_model_.audio_ui_state.separate_output_tracks = state.separate_output_tracks;
    view_model_.audio_ui_state.record_microphone = state.record_microphone;
    view_model_.audio_ui_state.mic_channel_mode = state.mic_channel_mode;
    view_model_.audio_ui_state.selected_mic_device_id = state.selected_mic_device_id;
    view_model_.audio_ui_state.mic_gain_linear = state.mic_gain_linear;
    view_model_.audio_ui_state.target_kind = target_kind;
    view_model_.audio_ui_state.selected_window_pid = selected_window_pid;

    populateMicDeviceCombo();
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
}

void RecordPage::setVideoSettings(const VideoSettingsModel& settings) {
    if (coordinator_) {
        coordinator_->SetVideoSettings(settings);
    }
}

void RecordPage::setOutputSettingsSummary(const OutputSettingsModel& settings) {
    const QString container =
        settings.container == capability::Container::Matroska
            ? QStringLiteral("MKV")
            : (settings.container == capability::Container::Mp4 ? QStringLiteral("MP4") : QStringLiteral("WEBM"));

    const QString audio =
        settings.audio_codec == capability::AudioCodec::Opus
            ? QStringLiteral("OPUS")
            : (settings.audio_codec == capability::AudioCodec::AacMf ? QStringLiteral("AAC") : QStringLiteral("PCM"));

    if (destination_header_) {
        destination_header_->setMeta(container + QStringLiteral(" · AV1 · ") + audio);
    }
    if (output_meta_label_) {
        output_meta_label_->setText(QStringLiteral("Files are saved using the configured output settings."));
    }
}

void RecordPage::openOutputFolder() {
    const QString result_path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    QString folder;

    if (!result_path.isEmpty()) {
        QFileInfo info(result_path);
        folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    } else if (!last_output_folder_.empty()) {
        folder = QString::fromStdWString(last_output_folder_.wstring());
    }

    if (folder.trimmed().isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void RecordPage::updateOpenFolderButtonState() {
    if (!open_folder_btn_) {
        return;
    }

    const bool has_result_path = !QString::fromStdWString(view_model_.result_output_path).trimmed().isEmpty();
    const bool has_output_folder = !last_output_folder_.empty();
    open_folder_btn_->setEnabled(has_result_path || has_output_folder);
}

void RecordPage::startPreviewIfIdle() {
    if (!preview_service_)
        return;

    const bool is_idle =
        (view_model_.state == UiRecordingState::Ready || view_model_.state == UiRecordingState::Completed);
    const bool has_target = view_model_.selected_target_index >= 0 &&
                            view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());

    preview_service_->Stop();
    if (preview_surface_)
        preview_surface_->setLiveFrame(QImage{});

    if (!is_idle || !has_target)
        return;

    const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
    preview_service_->Start(target);
}

void RecordPage::initCoordinator() {
    coordinator_ = std::make_unique<RecordingCoordinator>();
    view_model_.ApplyTargetKind(capability::CaptureTargetKind::Display);

    preview_service_ = std::make_unique<PreviewService>();
    QPointer<ui::widgets::PreviewSurface> safeSurface = preview_surface_;
    preview_service_->SetFrameCallback([safeSurface](QImage frame) {
        if (safeSurface)
            safeSurface->setLiveFrame(std::move(frame));
    });

    try {
        const auto caps = capability::CapabilityBuilder::BuildFromHardwareQuery();
        capability::SettingsResolver resolver(caps);
        const auto validation = resolver.ValidateConfig(primaryRecorderConfig());
        coordinator_->OnCapabilitiesReady(caps, validation);
    } catch (const std::exception& ex) {
        coordinator_->OnCapabilityFailure(L"Capability check failed.");
        qWarning() << "Capability check failed:" << ex.what();
    } catch (...) {
        coordinator_->OnCapabilityFailure(L"Capability check failed.");
        qWarning() << "Capability check failed with unknown error.";
    }

    coordinator_->SetStateChangedCallback([this](UiRecordingState state) {
        view_model_.SetState(state);
        if (state == UiRecordingState::Recording)
            diagnostics::AppLog(QStringLiteral("[record] recording started"));
        else if (state == UiRecordingState::Stopping)
            diagnostics::AppLog(QStringLiteral("[record] stopping"));
        else if (state == UiRecordingState::Ready || state == UiRecordingState::Completed)
            startPreviewIfIdle();
        refresh();
    });
    coordinator_->SetStatsUpdatedCallback([this](const recorder_core::SessionStats& stats) {
        view_model_.UpdateStats(stats);
        updateStatsDisplay();
    });
    coordinator_->SetMicMeterUpdatedCallback([this](float rms_linear) {
        preflight_mic_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetSysMeterUpdatedCallback([this](float rms_linear) {
        preflight_sys_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetAppMeterUpdatedCallback([this](float rms_linear) {
        preflight_app_rms_ = std::clamp(rms_linear, 0.0f, 1.0f);
        updateAudioMeterLevels();
    });
    coordinator_->SetResultReadyCallback([this](const UiRecordingResult& result) {
        view_model_.SetResult(result);
        if (result.succeeded)
            diagnostics::AppLog(
                QStringLiteral("[record] result: success  path=%1").arg(QString::fromStdWString(result.output_path)));
        else
            diagnostics::AppLog(QStringLiteral("[record] result: failed  phase=%1  hr=%2")
                                    .arg(QString::fromStdWString(result.error_phase))
                                    .arg(QString::fromStdWString(result.hresult_text)));
        refresh();
    });

    enumerateTargets(false);

    view_model_.SetState(coordinator_->State());
    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
    refresh();
    startPreviewIfIdle();
}

void RecordPage::enumerateTargets(bool preserve_current_selection) {
    const bool had_previous_selection =
        preserve_current_selection && view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    recorder_core::CaptureTarget previous_target{};
    if (had_previous_selection) {
        previous_target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
    }
    const capability::CaptureTargetKind previous_kind = view_model_.audio_ui_state.target_kind;

    view_model_.targets = coordinator_->EnumerateTargets();
    view_model_.target_display_names.clear();
    target_combo_->clear();

    monitor_target_indices_.clear();
    window_target_indices_.clear();
    monitor_target_index_ = -1;
    window_target_index_ = -1;

    for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(i)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
            monitor_target_indices_.push_back(i);
            if (monitor_target_index_ < 0) {
                monitor_target_index_ = i;
            }
        } else if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
            window_target_indices_.push_back(i);
        }

        const bool monitor = target.kind == recorder_core::CaptureTarget::Kind::Monitor;
        const QString prefix = monitor ? QStringLiteral("[Monitor] ") : QStringLiteral("[Window] ");
        const QString label = prefix + normalizedTargetLabel(target);
        view_model_.target_display_names.push_back(label.toStdWString());
        target_combo_->addItem(label);
    }

    window_target_indices_ = RecordViewModel::SortWindowTargetIndices(view_model_.targets, window_target_indices_);
    if (!window_target_indices_.empty()) {
        window_target_index_ = window_target_indices_.front();
    }

    int resolved_selection = -1;
    if (had_previous_selection) {
        for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
            const auto& target = view_model_.targets[static_cast<std::size_t>(i)];
            if (target.kind == previous_target.kind && target.native_id == previous_target.native_id) {
                resolved_selection = i;
                break;
            }
        }
    }

    if (resolved_selection < 0 && previous_kind == capability::CaptureTargetKind::Window && window_target_index_ >= 0) {
        resolved_selection = window_target_index_;
    }
    if (resolved_selection < 0 && previous_kind == capability::CaptureTargetKind::Display &&
        monitor_target_index_ >= 0) {
        resolved_selection = monitor_target_index_;
    }
    const bool keep_mode_without_selection =
        preserve_current_selection &&
        ((previous_kind == capability::CaptureTargetKind::Window && window_target_index_ < 0) ||
         (previous_kind == capability::CaptureTargetKind::Display && monitor_target_index_ < 0));
    if (resolved_selection < 0 && !keep_mode_without_selection && monitor_target_index_ >= 0) {
        resolved_selection = monitor_target_index_;
    }
    if (resolved_selection < 0 && !keep_mode_without_selection && window_target_index_ >= 0) {
        resolved_selection = window_target_index_;
    }

    view_model_.selected_target_index = -1;
    target_combo_->setCurrentIndex(-1);

    if (resolved_selection >= 0) {
        syncTargetSelectionToCombo(resolved_selection);
    } else {
        view_model_.ApplyTargetKindPreservingAudio(previous_kind);
        updateTargetCards();
        rebuildTargetPicker();
    }

    syncCoordinatorTargetContext();

    diagnostics::AppLog(QStringLiteral("[target] enumerated: total=%1 monitors=%2 windows=%3")
                            .arg(static_cast<int>(view_model_.targets.size()))
                            .arg(static_cast<int>(monitor_target_indices_.size()))
                            .arg(static_cast<int>(window_target_indices_.size())));
}

void RecordPage::rebuildTargetPicker() {
    if (!target_picker_kind_label_ || !target_picker_combo_ || !target_refresh_btn_ || !target_picker_note_label_) {
        return;
    }

    const bool busy = view_model_.state == UiRecordingState::Preparing ||
                      view_model_.state == UiRecordingState::Recording ||
                      view_model_.state == UiRecordingState::Stopping;

    const bool window_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;
    const auto& active_indices = window_mode ? window_target_indices_ : monitor_target_indices_;
    target_picker_kind_label_->setText(window_mode ? QStringLiteral("Window") : QStringLiteral("Display"));

    QSignalBlocker picker_blocker(target_picker_combo_);
    target_picker_combo_->clear();
    for (const int target_index : active_indices) {
        if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
            continue;
        }
        const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
        const QString label = window_mode ? windowLabelFromTarget(target) : displayLabelFromTarget(target);
        target_picker_combo_->addItem(label, target_index);
    }

    const int selected_item = target_picker_combo_->findData(view_model_.selected_target_index);
    if (selected_item >= 0) {
        target_picker_combo_->setCurrentIndex(selected_item);
    } else if (target_picker_combo_->count() > 0) {
        target_picker_combo_->setCurrentIndex(0);
    }

    QString note;
    if (active_indices.empty()) {
        note = window_mode ? QStringLiteral("No capturable windows found. Open a window and press Refresh.")
                           : QStringLiteral("No displays detected. Connect a display and press Refresh.");
    }

    target_picker_combo_->setEnabled(!busy && !active_indices.empty());
    target_refresh_btn_->setEnabled(!busy);
    target_picker_note_label_->setText(note);
    target_picker_note_label_->setVisible(!note.isEmpty());
}

void RecordPage::syncTargetSelectionToCombo(int target_index) {
    if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size())) {
        return;
    }

    if (target_index == view_model_.selected_target_index) {
        updateTargetCards();
        rebuildTargetPicker();
        return;
    }

    const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
    const capability::CaptureTargetKind new_kind = (target.kind == recorder_core::CaptureTarget::Kind::Window)
                                                       ? capability::CaptureTargetKind::Window
                                                       : capability::CaptureTargetKind::Display;
    const bool kind_changed = (new_kind != view_model_.audio_ui_state.target_kind);

    view_model_.selected_target_index = target_index;
    target_combo_->setCurrentIndex(target_index);

    if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
        window_target_index_ = target_index;
    } else if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        monitor_target_index_ = target_index;
    }

    view_model_.ApplyTargetKindPreservingAudio(new_kind);
    syncCoordinatorTargetContext();

    diagnostics::AppLog(QStringLiteral("[target] selected: %1 (kind_changed=%2)")
                            .arg(normalizedTargetLabel(target))
                            .arg(kind_changed ? QStringLiteral("yes") : QStringLiteral("no")));

    updateTargetCards();
    rebuildTargetPicker();
    startPreviewIfIdle();
}

void RecordPage::onStart() {
    const int idx = view_model_.selected_target_index;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
        return;

    const QString target_label = normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(idx)]);
    diagnostics::AppLog(QStringLiteral("[record] start requested  target=%1").arg(target_label));

    view_model_.ResetStats();
    syncCoordinatorTargetContext();
    coordinator_->StartRecording(view_model_.targets[static_cast<std::size_t>(idx)], view_model_.audio_ui_state);
}

void RecordPage::onStop() {
    diagnostics::AppLog(QStringLiteral("[record] stop requested"));
    coordinator_->StopRecording();
}

void RecordPage::onHotkeyToggle() {
    if (view_model_.CanStart())
        onStart();
    else if (view_model_.CanStop())
        onStop();
}

void RecordPage::onSelectMonitorTarget() {
    if (monitor_target_indices_.empty()) {
        view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Display);
        view_model_.selected_target_index = -1;
        target_combo_->setCurrentIndex(-1);
        diagnostics::AppLog(QStringLiteral("[target] monitor mode selected with no display targets"));
        updateTargetCards();
        rebuildTargetPicker();
        refresh();
        return;
    }

    const int next_target =
        monitor_target_index_ >= 0 ? monitor_target_index_ : monitor_target_indices_[static_cast<std::size_t>(0)];
    syncTargetSelectionToCombo(next_target);
    refresh();
}

void RecordPage::onSelectWindowTarget() {
    if (window_target_indices_.empty()) {
        view_model_.ApplyTargetKindPreservingAudio(capability::CaptureTargetKind::Window);
        view_model_.selected_target_index = -1;
        target_combo_->setCurrentIndex(-1);
        diagnostics::AppLog(QStringLiteral("[target] window mode selected with no window targets"));
        updateTargetCards();
        rebuildTargetPicker();
        refresh();
        return;
    }

    const int next_target =
        window_target_index_ >= 0 ? window_target_index_ : window_target_indices_[static_cast<std::size_t>(0)];
    syncTargetSelectionToCombo(next_target);
    refresh();
}

void RecordPage::onTargetPickerChanged(int index) {
    if (index < 0 || !target_picker_combo_) {
        return;
    }

    bool ok = false;
    const int target_index = target_picker_combo_->itemData(index).toInt(&ok);
    if (!ok) {
        return;
    }

    syncTargetSelectionToCombo(target_index);
    refresh();
}

void RecordPage::onRefreshTargets() {
    diagnostics::AppLog(QStringLiteral("[target] refresh requested"));
    enumerateTargets(true);
    refresh();
}

void RecordPage::onAppAudioToggled(bool checked) {
    view_model_.audio_ui_state.record_application_audio = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
    emitAudioSettingsChanged();
}

void RecordPage::onSysAudioToggled(bool checked) {
    view_model_.audio_ui_state.record_system_audio = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
    emitAudioSettingsChanged();
}

void RecordPage::onSeparateTracksToggled(bool checked) {
    view_model_.audio_ui_state.separate_output_tracks = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
    emitAudioSettingsChanged();
}

void RecordPage::onMicToggled(bool checked) {
    view_model_.audio_ui_state.record_microphone = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    emitAudioSettingsChanged();
}

void RecordPage::populateMicDeviceCombo() {
    if (!mic_device_combo_) {
        return;
    }

    const auto previous_id = view_model_.audio_ui_state.selected_mic_device_id;

    QSignalBlocker blocker(mic_device_combo_);

    mic_device_combo_->clear();
    mic_devices_.clear();

    mic_device_combo_->addItem("System Default Microphone");
    mic_devices_.push_back({});

    const auto devices = recorder_core::EnumerateAudioInputDevices();
    const auto labels = BuildMicDeviceLabels(devices);
    for (std::size_t i = 0; i < devices.size() && i < labels.size(); ++i) {
        mic_device_combo_->addItem(QString::fromStdString(labels[i]));
        mic_devices_.push_back(devices[i]);
    }

    int restore_index = 0;
    if (previous_id.has_value()) {
        for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
            if (mic_devices_[static_cast<std::size_t>(i)].device_id == *previous_id) {
                restore_index = i;
                break;
            }
        }
    }

    mic_device_combo_->setCurrentIndex(restore_index);
    if (restore_index <= 0 || restore_index >= static_cast<int>(mic_devices_.size())) {
        view_model_.audio_ui_state.selected_mic_device_id = std::nullopt;
    } else {
        const auto& selected = mic_devices_[static_cast<std::size_t>(restore_index)];
        view_model_.audio_ui_state.selected_mic_device_id =
            selected.device_id.empty() ? std::nullopt : std::optional<std::string>(selected.device_id);
    }

    view_model_.RebuildAudioPlan();
    updateMicDeviceNoteLabel();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
}

void RecordPage::onMicDeviceChanged(int index) {
    if (index <= 0 || index >= static_cast<int>(mic_devices_.size())) {
        view_model_.audio_ui_state.selected_mic_device_id = std::nullopt;
    } else {
        const auto& dev = mic_devices_[static_cast<std::size_t>(index)];
        view_model_.audio_ui_state.selected_mic_device_id =
            dev.device_id.empty() ? std::nullopt : std::optional<std::string>(dev.device_id);
    }

    view_model_.RebuildAudioPlan();
    updateMicDeviceNoteLabel();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    emitAudioSettingsChanged();
}

void RecordPage::onMicChannelChanged(int index) {
    static constexpr recorder_core::MicChannelMode kModes[] = {
        recorder_core::MicChannelMode::Auto,          recorder_core::MicChannelMode::PreserveStereo,
        recorder_core::MicChannelMode::MonoMix,       recorder_core::MicChannelMode::LeftToStereo,
        recorder_core::MicChannelMode::RightToStereo,
    };

    if (index >= 0 && index < static_cast<int>(std::size(kModes))) {
        view_model_.audio_ui_state.mic_channel_mode = kModes[static_cast<std::size_t>(index)];
    }

    view_model_.RebuildAudioPlan();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    emitAudioSettingsChanged();
}

void RecordPage::onMicGainChanged(int db_value) {
    view_model_.audio_ui_state.mic_gain_linear = SliderDbToLinear(db_value);
    if (mic_gain_value_label_) {
        mic_gain_value_label_->setText(db_value == 0 ? QStringLiteral("0 dB") : QStringLiteral("+%1 dB").arg(db_value));
    }
    view_model_.RebuildAudioPlan();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    emitAudioSettingsChanged();
}

void RecordPage::emitAudioSettingsChanged() {
    diagnostics::AppLog(QStringLiteral("[audio] settings changed: app=%1 sys=%2 mic=%3 sep=%4 mic_gain=%5")
                            .arg(view_model_.audio_ui_state.record_application_audio ? 1 : 0)
                            .arg(view_model_.audio_ui_state.record_system_audio ? 1 : 0)
                            .arg(view_model_.audio_ui_state.record_microphone ? 1 : 0)
                            .arg(view_model_.audio_ui_state.separate_output_tracks ? 1 : 0)
                            .arg(view_model_.audio_ui_state.mic_gain_linear, 0, 'f', 2));
    emit audioSettingsChanged(view_model_.audio_ui_state);
    PersistMicGainForMvp(view_model_.audio_ui_state.mic_gain_linear);
}

void RecordPage::updateAudioControls() {
    if (!app_audio_check_ || !sys_audio_check_ || !separate_tracks_check_ || !mic_check_ || !mic_channel_combo_ ||
        !mic_gain_slider_) {
        return;
    }

    QSignalBlocker b1(app_audio_check_);
    QSignalBlocker b2(sys_audio_check_);
    QSignalBlocker b3(separate_tracks_check_);
    QSignalBlocker b4(mic_check_);
    QSignalBlocker b5(mic_channel_combo_);
    QSignalBlocker b6(mic_gain_slider_);

    app_audio_check_->setChecked(view_model_.audio_ui_state.record_application_audio);
    sys_audio_check_->setChecked(view_model_.audio_ui_state.record_system_audio);
    separate_tracks_check_->setChecked(view_model_.audio_ui_state.separate_output_tracks);
    mic_check_->setChecked(view_model_.audio_ui_state.record_microphone);

    const auto mode = view_model_.audio_ui_state.mic_channel_mode;
    int channel_index = 0;
    switch (mode) {
    case recorder_core::MicChannelMode::Auto:
        channel_index = 0;
        break;
    case recorder_core::MicChannelMode::PreserveStereo:
        channel_index = 1;
        break;
    case recorder_core::MicChannelMode::MonoMix:
        channel_index = 2;
        break;
    case recorder_core::MicChannelMode::LeftToStereo:
        channel_index = 3;
        break;
    case recorder_core::MicChannelMode::RightToStereo:
        channel_index = 4;
        break;
    }
    mic_channel_combo_->setCurrentIndex(channel_index);
    {
        const int db = LinearToSliderDb(view_model_.audio_ui_state.mic_gain_linear);
        mic_gain_slider_->setValue(db);
        if (mic_gain_value_label_) {
            mic_gain_value_label_->setText(db == 0 ? QStringLiteral("0 dB") : QStringLiteral("+%1 dB").arg(db));
        }
    }

    updateAudioControlsVisibility();
}

void RecordPage::updateAudioControlsVisibility() {
    const bool is_window = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;

    const bool app = view_model_.audio_ui_state.record_application_audio;
    const bool sys = view_model_.audio_ui_state.record_system_audio;
    const bool mic = view_model_.audio_ui_state.record_microphone;

    const bool busy = view_model_.state == UiRecordingState::Preparing ||
                      view_model_.state == UiRecordingState::Recording ||
                      view_model_.state == UiRecordingState::Stopping;

    app_audio_check_->setVisible(is_window);
    (void)app;
    (void)sys;

    // Separate tracks: MKV only (WebM/MP4 muxers are untested for multi-track).
    const bool container_sep_ok = (current_container_ == capability::Container::Matroska);
    const int source_count = (is_window ? ((view_model_.audio_ui_state.record_application_audio ? 1 : 0) +
                                           (view_model_.audio_ui_state.record_system_audio ? 1 : 0))
                                        : (view_model_.audio_ui_state.record_system_audio ? 1 : 0)) +
                             (mic ? 1 : 0);
    const bool sep_applicable = container_sep_ok && (source_count >= 2);
    separate_tracks_check_->setVisible(sep_applicable);

    app_audio_check_->setEnabled(!busy);
    sys_audio_check_->setEnabled(!busy);
    separate_tracks_check_->setEnabled(sep_applicable && !busy);
    mic_check_->setEnabled(!busy);

    mic_device_row_->setVisible(mic);
    mic_channel_row_->setVisible(mic);
    mic_gain_row_->setVisible(mic);

    mic_device_combo_->setEnabled(!busy);
    mic_channel_combo_->setEnabled(!busy);
    mic_gain_slider_->setEnabled(!busy);
    if (mic_refresh_btn_) {
        mic_refresh_btn_->setEnabled(!busy);
    }
    updateMicDeviceNoteLabel();
}

void RecordPage::updateMicDeviceNoteLabel() {
    if (!mic_device_note_label_ || !mic_device_combo_ || !mic_device_row_) {
        return;
    }

    const bool mic_on = view_model_.audio_ui_state.record_microphone;
    const bool show_note = mic_on && mic_device_row_->isVisible() && mic_device_combo_->currentIndex() == 0;
    if (!show_note) {
        mic_device_note_label_->setVisible(false);
        return;
    }

    QString default_name;
    for (std::size_t i = 1; i < mic_devices_.size(); ++i) {
        const auto& dev = mic_devices_[i];
        if (!dev.is_default) {
            continue;
        }
        const std::string fallback = dev.display_name.empty() ? dev.device_id : dev.display_name;
        default_name = QString::fromStdString(fallback);
        break;
    }

    if (!default_name.isEmpty()) {
        mic_device_note_label_->setText(QStringLiteral("→ Currently: %1").arg(default_name));
    } else {
        mic_device_note_label_->setText(QStringLiteral("→ Follows the Windows default input device"));
    }
    mic_device_note_label_->setVisible(true);
}

void RecordPage::syncMicMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;
    const bool should_run = view_model_.audio_ui_state.record_microphone && !transition_busy;

    if (!should_run) {
        coordinator_->StopMicMeter();
        preflight_mic_rms_ = 0.0f;
        return;
    }

    if (!coordinator_->StartMicMeter(view_model_.audio_ui_state.selected_mic_device_id,
                                     view_model_.audio_ui_state.mic_channel_mode)) {
        preflight_mic_rms_ = 0.0f;
    }
}

void RecordPage::syncSysMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;
    const bool should_run = view_model_.audio_active_sys && !transition_busy;

    if (!should_run) {
        coordinator_->StopSysMeter();
        preflight_sys_rms_ = 0.0f;
        return;
    }

    coordinator_->StartSysMeter();
}

void RecordPage::syncAppMeterService() {
    if (!coordinator_) {
        return;
    }

    const bool transition_busy =
        view_model_.state == UiRecordingState::Preparing || view_model_.state == UiRecordingState::Stopping;

    uint32_t target_pid = 0;
    if (!transition_busy && view_model_.audio_active_app && view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Window && target.native_id != 0) {
            DWORD pid = 0;
            if (::GetWindowThreadProcessId(reinterpret_cast<HWND>(target.native_id), &pid) != 0 && pid != 0) {
                target_pid = static_cast<uint32_t>(pid);
            }
        }
    }

    if (target_pid == 0) {
        coordinator_->StopAppMeter();
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
        return;
    }

    if (target_pid != preflight_app_pid_) {
        coordinator_->StopAppMeter();
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
    }

    if (!coordinator_->StartAppMeter(target_pid)) {
        preflight_app_rms_ = 0.0f;
        preflight_app_pid_ = 0;
    } else {
        preflight_app_pid_ = target_pid;
    }
}

void RecordPage::updateAudioTrackPreview() {
    if (!track_preview_layout_) {
        return;
    }

    const auto sourceTag = [](const std::string& source_key) {
        if (source_key == "app")
            return QStringLiteral("APP");
        if (source_key == "sys")
            return QStringLiteral("SYS");
        if (source_key == "mic")
            return QStringLiteral("MIC");
        if (source_key == "system_output")
            return QStringLiteral("OUT");
        return QString::fromStdString(source_key).toUpper();
    };

    QLayoutItem* item = nullptr;
    while ((item = track_preview_layout_->takeAt(0)) != nullptr) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    if (view_model_.audio_track_preview.empty()) {
        auto* label = makeLabel("No audio tracks will be recorded.", "audioTrackPreviewEmpty", track_preview_panel_);
        label->setContentsMargins(14, 10, 14, 10);
        track_preview_layout_->addWidget(label);
        return;
    }

    bool first = true;
    for (const auto& preview_item : view_model_.audio_track_preview) {
        auto* row = new QWidget(track_preview_panel_);
        row->setObjectName("audioTrackPreviewRow");
        row->setProperty("firstRow", first);
        first = false;

        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(14, 8, 14, 8);
        rl->setSpacing(10);

        auto* idx = makeLabel(sourceTag(preview_item.source_key), "audioTrackPreviewTag", row);
        idx->setFixedWidth(44);
        rl->addWidget(idx);

        rl->addWidget(makeLabel(QString::fromStdString(preview_item.display_label), "audioTrackName", row));
        rl->addStretch(1);

        track_preview_layout_->addWidget(row);
    }
}

void RecordPage::syncCoordinatorTargetContext() {
    if (!coordinator_) {
        return;
    }

    FilenameTargetContext context;
    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        context = RecordViewModel::FilenameContextFromCaptureTarget(target);
    } else {
        context.target_name = L"Desktop - Display 1";
        context.app_name = L"Desktop";
        context.window_title = L"Display 1";
        context.process_name = L"desktop";
    }

    coordinator_->SetOutputTargetContext(context);
}

QString RecordPage::buildChromeStatusLabel() const {
    switch (view_model_.state) {
    case UiRecordingState::Recording:
        return QStringLiteral("REC");
    case UiRecordingState::Blocked:
    case UiRecordingState::Failed:
        return QStringLiteral("BLOCKED");
    case UiRecordingState::LoadingCapabilities:
        return QStringLiteral("CHECKING");
    case UiRecordingState::Preparing:
        return QStringLiteral("STARTING");
    case UiRecordingState::Stopping:
        return QStringLiteral("STOPPING");
    case UiRecordingState::Ready:
    case UiRecordingState::Completed:
    default:
        return QStringLiteral("READY");
    }
}

QString RecordPage::buildPreviewBottomLeftText(bool recording) const {
    if (!recording) {
        return QString();
    }

    QString frame_text = QStringLiteral("–");
    QString bitrate_text = QStringLiteral("–");
    QString drop_text = QStringLiteral("–");

    if (view_model_.live_stats_available) {
        if (view_model_.elapsed_seconds > 0.0 && view_model_.frames_captured > 0) {
            const double frame_ms =
                (view_model_.elapsed_seconds * 1000.0) / static_cast<double>(view_model_.frames_captured);
            frame_text = QString::number(frame_ms, 'f', 2);
        }

        if (view_model_.elapsed_seconds > 0.0) {
            const double bitrate_mbps = (static_cast<double>(view_model_.video_bytes + view_model_.audio_bytes) * 8.0) /
                                        (view_model_.elapsed_seconds * 1000000.0);
            bitrate_text = QStringLiteral("%1 Mb/s").arg(bitrate_mbps, 0, 'f', 1);
        }

        drop_text = QString::number(view_model_.dropped_frames);
    }

    const QString drift_text = view_model_.live_stats_available
                                   ? QStringLiteral("%1 ms").arg(view_model_.av_drift_ms, 0, 'f', 0)
                                   : QStringLiteral("–");

    return QStringLiteral("FRAME %1 ms · BITRATE %2 · DROP %3 · DRIFT %4")
        .arg(frame_text, bitrate_text, drop_text, drift_text);
}

QString RecordPage::buildPreviewBottomRightText(bool recording) const {
    if (!recording) {
        return QStringLiteral("AV1 · CQ 24");
    }

    const uint64_t live_bytes = view_model_.video_bytes + view_model_.audio_bytes;
    const QString size_text = view_model_.live_stats_available
                                  ? QString::fromStdWString(RecordViewModel::FormatBytes(live_bytes))
                                  : QStringLiteral("–");
    return QStringLiteral("AV1 · CQ 24 · SIZE %1").arg(size_text);
}

QString RecordPage::buildTimerText(bool recording) const {
    if (!recording) {
        return QStringLiteral("00:00:00");
    }
    if (!view_model_.live_stats_available) {
        return QStringLiteral("--:--:--");
    }
    return toClock(view_model_.elapsed_text);
}

void RecordPage::emitChromeState() {
    const bool recording =
        view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping;
    const QString status = buildChromeStatusLabel();

    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        emit chromeStateChanged(recording, status,
                                QStringLiteral("%1 · 60 fps · AV1").arg(normalizedTargetLabel(target)));
        return;
    }

    emit chromeStateChanged(recording, status, QStringLiteral("NO TARGET · 60 fps · AV1"));
}

void RecordPage::refresh() {
    const QString capability_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool checking = (view_model_.state == UiRecordingState::LoadingCapabilities);
    const bool recording =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping);

    capability_label_->setText(capability_text);
    capability_label_->setVisible((blocked || checking) && !capability_text.isEmpty());
    if (capability_label_->isVisible()) {
        setStyledStringProperty(capability_label_, "panelRole", blocked ? "blocker" : "note");
    }

    start_btn_->setVisible(!recording);
    stop_btn_->setVisible(recording);
    start_btn_->setEnabled(view_model_.CanStart());
    stop_btn_->setEnabled(view_model_.CanStop());

    control_state_label_->setText(stateDisplay(view_model_.state));
    const bool failed = (view_model_.state == UiRecordingState::Failed);
    setStyledStringProperty(control_state_label_, "stateRole",
                            (blocked || failed) ? "blocked" : (recording ? "recording" : "ready"));

    output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));

    preview_surface_->setRecording(recording);
    preview_surface_->setStatusText(recording ? QStringLiteral("REC")
                                              : (blocked ? QStringLiteral("BLOCKED") : QStringLiteral("READY")));
    preview_surface_->statusPill()->setTone(
        recording ? ui::widgets::StatusPill::Tone::Recording
                  : (blocked ? ui::widgets::StatusPill::Tone::Blocked : ui::widgets::StatusPill::Tone::Ready));
    QString target_desc = QStringLiteral("No target selected");
    const bool has_selected_target = view_model_.selected_target_index >= 0 &&
                                     view_model_.selected_target_index < static_cast<int>(view_model_.targets.size());
    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        target_desc = normalizedTargetLabel(target);
        capture_header_->setMeta(QStringLiteral("%1 · 60 fps").arg(target_desc));
        preview_surface_->setTopMetaText(QStringLiteral("%1 · 60 fps").arg(target_desc));
    } else {
        capture_header_->setMeta("NO TARGET");
        preview_surface_->setTopMetaText("NO TARGET");
    }

    if (recording) {
        preview_surface_->setCenterTitle(QStringLiteral("RECORDING"));
        preview_surface_->setCenterSubtitle(target_desc);
    } else if (blocked || failed) {
        preview_surface_->setCenterTitle(QStringLiteral("BLOCKED"));
        const QString blocker_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();
        preview_surface_->setCenterSubtitle(blocker_text.isEmpty() ? QStringLiteral("Check diagnostics for details")
                                                                   : blocker_text);
    } else {
        const bool preview_live = preview_service_ && preview_service_->IsRunning();
        preview_surface_->setCenterTitle(has_selected_target ? target_desc : QStringLiteral("NO TARGET"));
        preview_surface_->setCenterSubtitle(
            has_selected_target ? (preview_live ? QString{} : QStringLiteral("Static — preview unavailable in alpha"))
                                : QStringLiteral("Select a capture source above"));
    }

    updateTargetCards();
    rebuildTargetPicker();
    updateReadinessRows();
    updateAudioControls();
    updateAudioTrackPreview();
    syncMicMeterService();
    syncSysMeterService();
    syncAppMeterService();
    updateAudioMeterLevels();
    updateStatsDisplay();

    updateResultDisplay();
    updateOpenFolderButtonState();

    emitChromeState();
}

void RecordPage::updateStatsDisplay() {
    const bool recording =
        view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping;

    const QString timer_text = buildTimerText(recording);
    timer_label_->setText(timer_text);
    setStyledStringProperty(timer_label_, "timerState", recording ? "recording" : "idle");

    preview_surface_->setBottomLeftText(buildPreviewBottomLeftText(recording));
    preview_surface_->setBottomRightText(buildPreviewBottomRightText(recording));

    QString bitrate_text = QStringLiteral("–");
    if (recording && view_model_.live_stats_available && view_model_.elapsed_seconds > 0.0) {
        const double bitrate_mbps = (static_cast<double>(view_model_.video_bytes + view_model_.audio_bytes) * 8.0) /
                                    (view_model_.elapsed_seconds * 1000000.0);
        bitrate_text = QStringLiteral("%1 Mb/s").arg(bitrate_mbps, 0, 'f', 1);
    }
    const QString drop_text = recording && view_model_.live_stats_available
                                  ? QString::number(view_model_.dropped_frames)
                                  : QStringLiteral("–");
    emit chromeRuntimeMetricsChanged(timer_text, bitrate_text, drop_text);

    updateAudioMeterLevels();
}

void RecordPage::updateResultDisplay() {
    if (!result_panel_ || !view_model_.HasResult()) {
        if (result_panel_) {
            result_panel_->setVisible(false);
        }
        return;
    }

    result_panel_->setVisible(true);

    setStyledStringProperty(result_panel_, "resultKind", view_model_.last_succeeded ? "success" : "error");
    setStyledStringProperty(result_title_label_, "labelRole",
                            view_model_.last_succeeded ? "resultTitleOk" : "resultTitleErr");

    result_title_label_->setText(QString::fromStdWString(view_model_.result_user_title));
    result_title_label_->setVisible(!view_model_.result_user_title.empty());

    result_message_label_->setText(QString::fromStdWString(view_model_.result_user_message));
    result_message_label_->setVisible(!view_model_.last_succeeded && !view_model_.result_user_message.empty());

    result_action_label_->setText(QString::fromStdWString(view_model_.result_action_hint));
    result_action_label_->setVisible(!view_model_.last_succeeded && !view_model_.result_action_hint.empty());

    result_stats_label_->setText(QString::fromStdWString(view_model_.result_stats_text));
    result_stats_label_->setVisible(view_model_.last_succeeded && !view_model_.result_stats_text.empty());

    const QString path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    result_path_label_->setText(path.isEmpty() ? QString{} : QStringLiteral("→ ") + path);
    result_path_label_->setVisible(!path.isEmpty());

    const QString phase = QString::fromStdWString(view_model_.result_error_phase).trimmed();
    const QString hr = QString::fromStdWString(view_model_.result_hresult_text).trimmed();
    const QString detail = QString::fromStdWString(view_model_.result_error_detail).trimmed();

    QString technical;
    if (!phase.isEmpty()) {
        technical += QStringLiteral("Phase: ") + phase;
    }
    if (!hr.isEmpty()) {
        technical += (technical.isEmpty() ? QString{} : QStringLiteral("  ·  "));
        technical += QStringLiteral("HRESULT: ") + hr;
    }
    if (!detail.isEmpty()) {
        const QString short_detail = detail.length() > 120 ? detail.left(120) + QStringLiteral("…") : detail;
        technical += technical.isEmpty() ? QString{} : QStringLiteral("\n");
        technical += short_detail;
    }

    result_technical_label_->setText(technical);
    result_technical_label_->setVisible(!technical.isEmpty());
    if (result_technical_separator_) {
        result_technical_separator_->setVisible(!technical.isEmpty());
    }

    result_panel_->style()->unpolish(result_panel_);
    result_panel_->style()->polish(result_panel_);
}

void RecordPage::updateTargetCards() {
    const int current = view_model_.selected_target_index;
    const bool recording =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping ||
         view_model_.state == UiRecordingState::Preparing);
    const bool has_selected_target = current >= 0 && current < static_cast<int>(view_model_.targets.size());
    const bool selected_monitor = has_selected_target && view_model_.targets[static_cast<std::size_t>(current)].kind ==
                                                             recorder_core::CaptureTarget::Kind::Monitor;
    const bool selected_window = has_selected_target && view_model_.targets[static_cast<std::size_t>(current)].kind ==
                                                            recorder_core::CaptureTarget::Kind::Window;
    const bool monitor_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Display;
    const bool window_mode = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;

    const bool any_monitor_selected = !recording && (selected_monitor || (!has_selected_target && monitor_mode));
    const bool window_selected = !recording && (selected_window || (!has_selected_target && window_mode));

    monitor_card_->setSelected(any_monitor_selected);
    window_card_->setSelected(window_selected);

    // Subtitle for the monitor card: use the currently active monitor target
    const int active_monitor_idx = monitor_target_index_;
    if (active_monitor_idx >= 0 && active_monitor_idx < static_cast<int>(view_model_.targets.size())) {
        const QString display_name =
            displayLabelFromTarget(view_model_.targets[static_cast<std::size_t>(active_monitor_idx)]);
        const QString suffix =
            monitor_target_indices_.size() > 1
                ? QStringLiteral("  [%1/%2]")
                      .arg(static_cast<int>(std::find(monitor_target_indices_.begin(), monitor_target_indices_.end(),
                                                      active_monitor_idx) -
                                            monitor_target_indices_.begin()) +
                           1)
                      .arg(static_cast<int>(monitor_target_indices_.size()))
                : QString{};
        monitor_card_->setSubtitle(display_name + suffix);
    } else {
        monitor_card_->setSubtitle("No display detected");
    }

    const int active_window_idx = window_target_index_;
    if (active_window_idx >= 0 && active_window_idx < static_cast<int>(view_model_.targets.size())) {
        window_card_->setSubtitle(
            windowLabelFromTarget(view_model_.targets[static_cast<std::size_t>(active_window_idx)]));
    } else {
        window_card_->setSubtitle("No capturable windows");
    }
}

void RecordPage::updateReadinessRows() {
    if (readiness_rows_.size() < 5)
        return;

    const bool is_window_target = view_model_.audio_ui_state.target_kind == capability::CaptureTargetKind::Window;
    readiness_rows_[2].title->setText(is_window_target ? "Audio loopback (APP)" : "Audio loopback (SYS)");

    const bool blocked = (view_model_.state == UiRecordingState::Blocked);
    const bool checking = (view_model_.state == UiRecordingState::LoadingCapabilities);
    readiness_header_->setMeta(checking ? "CHECKING" : (blocked ? "BLOCKERS PRESENT" : "ALL CLEAR"));

    const QString target_detail =
        (view_model_.selected_target_index >= 0 &&
         view_model_.selected_target_index < static_cast<int>(view_model_.targets.size()))
            ? normalizedTargetLabel(view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)])
            : QString("No target selected");

    const QString output_detail = QString::fromStdWString(view_model_.output_path_display);
    const QString session_state = QString::fromStdWString(view_model_.state_text);
    const QString capability_text = QString::fromStdWString(view_model_.capability_status_text).trimmed();

    const bool encoder_ok = !blocked && !checking;
    const bool target_ok = !target_detail.isEmpty() && target_detail != "No target selected";
    const bool output_ok = !output_detail.isEmpty() && output_detail != "--";

    const struct RowData {
        bool ok;
        bool hard_blocked;
        QString detail;
    } rows[] = {
        {encoder_ok, blocked,
         checking ? QString("Checking capabilities...") : (blocked ? capability_text : QString("Available"))},
        {target_ok, false, target_detail},
        {true, false, QString("WASAPI loopback path available")},
        {output_ok, false, output_detail},
        {!blocked && !checking, blocked, session_state},
    };

    for (int i = 0; i < 5; ++i) {
        auto& row_widgets = readiness_rows_[static_cast<std::size_t>(i)];
        row_widgets.icon->setText(checkGlyph(rows[i].ok, rows[i].hard_blocked));
        setStyledStringProperty(row_widgets.icon, "stateRole",
                                rows[i].hard_blocked ? "blocked" : (rows[i].ok ? "ready" : "muted"));
        row_widgets.detail->setText(rows[i].detail);
    }
}

void RecordPage::updateAudioMeterLevels() {
    const bool recording_live =
        view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping;

    auto applyMeter = [](ui::widgets::VUMeterWidget* meter, QLabel* db_label, float rms, bool show_level) {
        if (!meter || !db_label) {
            return;
        }

        meter->setActive(show_level);

        if (show_level) {
            const float db = rms > 0.0f ? (std::max)(-60.0f, 20.0f * std::log10(rms)) : -60.0f;
            const float meter01 = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            meter->setLevel(meter01);
            db_label->setText(QString::number(static_cast<int>(std::round(db))) + QStringLiteral(" dB"));
        } else {
            meter->setLevel(0.0f);
            db_label->setText(QStringLiteral("– dB"));
        }
    };

    const bool sys_meter_live =
        coordinator_ != nullptr && coordinator_->IsSysMeterRunning() && view_model_.audio_active_sys;
    const float sys_rms = sys_meter_live ? preflight_sys_rms_ : (recording_live ? view_model_.audio_rms_sys : 0.0f);
    applyMeter(sys_meter_, sys_db_label_, sys_rms, sys_meter_live || (recording_live && view_model_.audio_active_sys));

    const bool app_meter_live =
        coordinator_ != nullptr && coordinator_->IsAppMeterRunning() && view_model_.audio_active_app;
    const float app_rms = app_meter_live ? preflight_app_rms_ : (recording_live ? view_model_.audio_rms_app : 0.0f);
    applyMeter(app_meter_, app_db_label_, app_rms, app_meter_live || (recording_live && view_model_.audio_active_app));

    const bool mic_meter_live = coordinator_ != nullptr && coordinator_->IsMicMeterRunning() &&
                                view_model_.audio_ui_state.record_microphone && view_model_.audio_active_mic;
    const float mic_rms = mic_meter_live
                              ? std::clamp(preflight_mic_rms_ * view_model_.audio_ui_state.mic_gain_linear, 0.0f, 1.0f)
                              : (recording_live ? view_model_.audio_rms_mic : 0.0f);
    applyMeter(mic_meter_, mic_db_label_, mic_rms, mic_meter_live || (recording_live && view_model_.audio_active_mic));
}

} // namespace exosnap
