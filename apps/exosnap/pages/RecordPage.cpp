#include "RecordPage.h"

#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/widgets/CaptureTargetCard.h"
#include "../ui/widgets/PreviewSurface.h"
#include "../ui/widgets/SectionRuleHeader.h"
#include "../ui/widgets/StatusPill.h"
#include "../ui/widgets/TogglePill.h"
#include "../ui/widgets/VUMeterWidget.h"

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include <exception>
#include <iterator>

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
        return "READY TO RECORD";
    case UiRecordingState::Blocked:
        return "BLOCKED";
    case UiRecordingState::Preparing:
        return "PREPARING";
    case UiRecordingState::Recording:
        return "RECORDING";
    case UiRecordingState::Stopping:
        return "STOPPING";
    case UiRecordingState::Completed:
        return "COMPLETED";
    case UiRecordingState::Failed:
    default:
        return "FAILED";
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

QString targetLabelFor(const recorder_core::CaptureTarget& target) {
    switch (target.kind) {
    case recorder_core::CaptureTarget::Kind::Monitor:
        return "Monitor";
    case recorder_core::CaptureTarget::Kind::Window:
        return "Window";
    default:
        return "Target";
    }
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

} // namespace

RecordPage::RecordPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl,
                               ui::theme::ExoSnapMetrics::kSpaceXl, ui::theme::ExoSnapMetrics::kSpaceXl);
    layout->setSpacing(ui::theme::ExoSnapMetrics::kSpaceLg);

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
    control_layout->setContentsMargins(14, 12, 14, 12);
    control_layout->setSpacing(10);

    auto* control_head = new QHBoxLayout();
    control_head->setContentsMargins(0, 0, 0, 0);
    control_head->setSpacing(8);
    control_state_label_ = makeLabel("READY TO RECORD", "recordControlState", control_panel);
    auto* hotkey_label = makeLabel("ALT+F9", "recordHotkeyBadge", control_panel);
    hotkey_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    control_head->addWidget(control_state_label_);
    control_head->addStretch(1);
    control_head->addWidget(hotkey_label);
    control_layout->addLayout(control_head);

    start_btn_ = new QPushButton("START RECORDING", control_panel);
    start_btn_->setProperty("role", "primaryRecordStart");
    start_btn_->setMinimumHeight(46);
    control_layout->addWidget(start_btn_);

    stop_btn_ = new QPushButton("■ STOP", control_panel);
    stop_btn_->setProperty("role", "recordStop");
    stop_btn_->setMinimumHeight(46);
    stop_btn_->hide();
    control_layout->addWidget(stop_btn_);

    timer_label_ = makeLabel("00:00:00", "recordTimer", control_panel);
    timer_label_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    control_layout->addWidget(timer_label_);

    auto* size_grid = new QGridLayout();
    size_grid->setHorizontalSpacing(10);
    size_grid->setVerticalSpacing(2);
    size_grid->addWidget(makeLabel("SIZE", "recordMetricKey", control_panel), 0, 0);
    size_value_label_ = makeLabel("0 KB", "recordMetricValue", control_panel);
    size_value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    size_grid->addWidget(size_value_label_, 0, 1);
    size_grid->addWidget(makeLabel("EST", "recordMetricKey", control_panel), 1, 0);
    est_value_label_ = makeLabel("~-- GB/h", "recordMetricValue", control_panel);
    est_value_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    size_grid->addWidget(est_value_label_, 1, 1);
    control_layout->addLayout(size_grid);

    rail_layout->addWidget(control_panel);

    auto* toggles_panel = makePanel(rail_column);
    auto* toggles_layout = new QVBoxLayout(toggles_panel);
    toggles_layout->setContentsMargins(14, 12, 14, 12);
    toggles_layout->setSpacing(6);
    toggles_layout->addWidget(makeLabel("QUICK TOGGLES", "recordQuickTogglesHead", toggles_panel));
    const struct QuickToggleRow {
        const char* label;
        bool on_by_default;
    } quick_toggles[] = {
        {"Capture cursor", true},
        {"Hardware encode", true},
        {"Auto-split @ 4 h", false},
    };
    for (const auto& toggle : quick_toggles) {
        auto* row = new QWidget(toggles_panel);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 1, 0, 1);
        row_layout->setSpacing(8);
        row_layout->addWidget(makeLabel(QString::fromUtf8(toggle.label), "recordToggleLabel", row), 1);

        auto* pill = new ui::widgets::TogglePill(row);
        pill->setOn(toggle.on_by_default);
        row_layout->addWidget(pill, 0, Qt::AlignRight | Qt::AlignVCenter);
        toggles_layout->addWidget(row);
    }
    quick_toggle_note_label_ = makeLabel("Placeholder controls: wiring follows a later integration pass.",
                                         "recordPlaceholderNote", toggles_panel);
    quick_toggle_note_label_->setWordWrap(true);
    toggles_layout->addWidget(quick_toggle_note_label_);
    rail_layout->addWidget(toggles_panel);
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
    region_card_ = new ui::widgets::CaptureTargetCard(cards_row);
    region_card_->setTitle("Region");
    cards_layout->addWidget(monitor_card_, 1);
    cards_layout->addWidget(window_card_, 1);
    cards_layout->addWidget(region_card_, 1);
    monitor_card_->setAccessibleName("Monitor target");
    window_card_->setAccessibleName("Window target");
    region_card_->setAccessibleName("Region target");
    QWidget::setTabOrder(monitor_card_, window_card_);
    QWidget::setTabOrder(window_card_, region_card_);
    layout->addWidget(cards_row);

    target_combo_ = new QComboBox(content);
    target_combo_->setVisible(false);
    layout->addWidget(target_combo_);

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
    app_audio_check_ = new QCheckBox("Record Application Audio", audio_settings_panel);
    sys_audio_check_ = new QCheckBox("Record System Audio", audio_settings_panel);
    separate_tracks_check_ = new QCheckBox("Separate output tracks", audio_settings_panel);
    audio_settings_layout->addWidget(app_audio_check_);
    audio_settings_layout->addWidget(sys_audio_check_);
    audio_settings_layout->addWidget(separate_tracks_check_);

    audio_settings_layout->addSpacing(6);
    audio_settings_layout->addWidget(makeLabel("Audio Input", "audioSettingsGroupTitle", audio_settings_panel));
    mic_check_ = new QCheckBox("Record Microphone", audio_settings_panel);
    audio_settings_layout->addWidget(mic_check_);

    mic_device_row_ = new QWidget(audio_settings_panel);
    auto* mic_device_row_layout = new QHBoxLayout(mic_device_row_);
    mic_device_row_layout->setContentsMargins(0, 0, 0, 0);
    mic_device_row_layout->setSpacing(10);
    mic_device_row_layout->addWidget(makeLabel("Input Device", "audioSettingsRowLabel", mic_device_row_));
    mic_device_combo_ = new QComboBox(mic_device_row_);
    mic_device_combo_->addItem("Default Microphone");
    mic_device_row_layout->addWidget(mic_device_combo_, 1);
    audio_settings_layout->addWidget(mic_device_row_);

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
    audio_header_->setMeta("PLACEHOLDER · NOT LIVE ENGINE METERS");
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

        auto* db_label = makeLabel("-- dB", "audioDb", row);
        db_label->setFixedWidth(118);
        db_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row_layout->addWidget(db_label);

        audio_layout->addWidget(row);

        *meter_out = meter;
        *db_label_out = db_label;
    };

    addAudioRow("APP", "Selected application audio", &app_meter_, &app_db_label_);
    addAudioRow("MIC", "Microphone (placeholder source)", &mic_meter_, &mic_db_label_);
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
    output_meta_label_ =
        makeLabel("Path is resolved by coordinator after capability checks.", "destinationMeta", dest_left);
    output_meta_label_->setWordWrap(true);
    dest_left_layout->addWidget(output_path_label_);
    dest_left_layout->addWidget(output_meta_label_);

    auto* dest_buttons = new QWidget(destination_panel);
    auto* dest_buttons_layout = new QHBoxLayout(dest_buttons);
    dest_buttons_layout->setContentsMargins(0, 0, 0, 0);
    dest_buttons_layout->setSpacing(6);
    auto* reveal_btn = new QPushButton("Reveal (placeholder)", dest_buttons);
    reveal_btn->setProperty("role", "ghost");
    reveal_btn->setEnabled(false);
    auto* change_btn = new QPushButton("Change (placeholder)", dest_buttons);
    change_btn->setProperty("role", "ghost");
    change_btn->setEnabled(false);
    dest_buttons_layout->addWidget(reveal_btn);
    dest_buttons_layout->addWidget(change_btn);

    destination_layout->addWidget(dest_left, 1);
    destination_layout->addWidget(dest_buttons, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(destination_panel);

    result_panel_ = makePanel(content, "resultPanel");
    auto* result_layout = new QVBoxLayout(result_panel_);
    result_layout->setContentsMargins(14, 10, 14, 10);
    result_layout->setSpacing(4);
    result_status_label_ = makeLabel("", "resultStatus", result_panel_);
    result_path_label_ = makeLabel("", "resultDetail", result_panel_);
    result_phase_label_ = makeLabel("", "resultDetail", result_panel_);
    result_hresult_label_ = makeLabel("", "resultDetail", result_panel_);
    result_detail_label_ = makeLabel("", "resultDetail", result_panel_);
    result_path_label_->setWordWrap(true);
    result_phase_label_->setWordWrap(true);
    result_hresult_label_->setWordWrap(true);
    result_detail_label_->setWordWrap(true);
    result_layout->addWidget(result_status_label_);
    result_layout->addWidget(result_path_label_);
    result_layout->addWidget(result_phase_label_);
    result_layout->addWidget(result_hresult_label_);
    result_layout->addWidget(result_detail_label_);
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
    connect(region_card_, &ui::widgets::CaptureTargetCard::clicked, this, &RecordPage::onSelectRegionTarget);
    connect(app_audio_check_, &QCheckBox::toggled, this, &RecordPage::onAppAudioToggled);
    connect(sys_audio_check_, &QCheckBox::toggled, this, &RecordPage::onSysAudioToggled);
    connect(separate_tracks_check_, &QCheckBox::toggled, this, &RecordPage::onSeparateTracksToggled);
    connect(mic_check_, &QCheckBox::toggled, this, &RecordPage::onMicToggled);
    connect(mic_channel_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &RecordPage::onMicChannelChanged);

    initCoordinator();
}

void RecordPage::initCoordinator() {
    coordinator_ = std::make_unique<RecordingCoordinator>();
    view_model_.ApplyTargetKind(capability::CaptureTargetKind::Display);

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
        refresh();
    });
    coordinator_->SetStatsUpdatedCallback([this](const recorder_core::SessionStats& stats) {
        view_model_.UpdateStats(stats);
        updateStatsDisplay();
    });
    coordinator_->SetResultReadyCallback([this](const UiRecordingResult& result) {
        view_model_.SetResult(result);
        refresh();
    });

    view_model_.targets = coordinator_->EnumerateTargets();
    for (const auto& target : view_model_.targets) {
        const bool is_monitor = (target.kind == recorder_core::CaptureTarget::Kind::Monitor);
        const std::wstring prefix = is_monitor ? L"[Monitor] " : L"[Window] ";
        const std::wstring display = prefix + QString::fromStdString(target.description).toStdWString();
        view_model_.target_display_names.push_back(display);
        target_combo_->addItem(QString::fromStdWString(display));
    }

    view_model_.selected_target_index = -1;
    for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(i)];
        if (target.kind == recorder_core::CaptureTarget::Kind::Monitor && monitor_target_index_ < 0)
            monitor_target_index_ = i;
        if (target.kind == recorder_core::CaptureTarget::Kind::Window && window_target_index_ < 0)
            window_target_index_ = i;
    }

    if (monitor_target_index_ >= 0) {
        syncTargetSelectionToCombo(monitor_target_index_);
    } else if (!view_model_.targets.empty()) {
        syncTargetSelectionToCombo(0);
    }

    view_model_.SetState(coordinator_->State());
    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
    refresh();
}

void RecordPage::syncTargetSelectionToCombo(int target_index) {
    if (target_index < 0 || target_index >= static_cast<int>(view_model_.targets.size()))
        return;
    view_model_.selected_target_index = target_index;
    target_combo_->setCurrentIndex(target_index);

    const auto& target = view_model_.targets[static_cast<std::size_t>(target_index)];
    if (target.kind == recorder_core::CaptureTarget::Kind::Window) {
        view_model_.ApplyTargetKind(capability::CaptureTargetKind::Window);
    } else if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
        view_model_.ApplyTargetKind(capability::CaptureTargetKind::Display);
    } else {
        qWarning() << "Unsupported capture target kind for audio mapping";
    }

    updateTargetCards();
}

void RecordPage::onStart() {
    const int idx = target_combo_->currentIndex();
    view_model_.selected_target_index = idx;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
        return;

    view_model_.ResetStats();
    coordinator_->StartRecording(view_model_.targets[static_cast<std::size_t>(idx)], view_model_.audio_ui_state);
}

void RecordPage::onStop() {
    coordinator_->StopRecording();
}

void RecordPage::onSelectMonitorTarget() {
    syncTargetSelectionToCombo(monitor_target_index_);
    refresh();
}

void RecordPage::onSelectWindowTarget() {
    syncTargetSelectionToCombo(window_target_index_);
    refresh();
}

void RecordPage::onSelectRegionTarget() {
    if (region_target_index_ >= 0) {
        syncTargetSelectionToCombo(region_target_index_);
        refresh();
    }
}

void RecordPage::onAppAudioToggled(bool checked) {
    view_model_.audio_ui_state.record_application_audio = checked;

    if (!view_model_.audio_ui_state.record_application_audio || !view_model_.audio_ui_state.record_system_audio) {
        view_model_.audio_ui_state.separate_output_tracks = false;
    }

    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
}

void RecordPage::onSysAudioToggled(bool checked) {
    view_model_.audio_ui_state.record_system_audio = checked;

    if (!view_model_.audio_ui_state.record_application_audio || !view_model_.audio_ui_state.record_system_audio) {
        view_model_.audio_ui_state.separate_output_tracks = false;
    }

    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
}

void RecordPage::onSeparateTracksToggled(bool checked) {
    view_model_.audio_ui_state.separate_output_tracks = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
}

void RecordPage::onMicToggled(bool checked) {
    view_model_.audio_ui_state.record_microphone = checked;
    view_model_.RebuildAudioPlan();
    updateAudioControls();
    updateAudioTrackPreview();
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
}

void RecordPage::updateAudioControls() {
    if (!app_audio_check_ || !sys_audio_check_ || !separate_tracks_check_ || !mic_check_ || !mic_channel_combo_) {
        return;
    }

    QSignalBlocker b1(app_audio_check_);
    QSignalBlocker b2(sys_audio_check_);
    QSignalBlocker b3(separate_tracks_check_);
    QSignalBlocker b4(mic_check_);
    QSignalBlocker b5(mic_channel_combo_);

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
    separate_tracks_check_->setVisible(is_window && app && sys);

    app_audio_check_->setEnabled(!busy);
    sys_audio_check_->setEnabled(!busy);
    separate_tracks_check_->setEnabled(!busy);
    mic_check_->setEnabled(!busy);

    mic_device_row_->setVisible(mic);
    mic_channel_row_->setVisible(mic);

    mic_device_combo_->setEnabled(!busy);
    mic_channel_combo_->setEnabled(!busy);
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
    setStyledStringProperty(control_state_label_, "stateRole",
                            blocked ? "blocked" : (recording ? "recording" : "ready"));

    output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));

    preview_surface_->setRecording(recording);
    preview_surface_->setStatusText(recording ? QStringLiteral("REC")
                                              : (blocked ? QStringLiteral("BLOCKED") : QStringLiteral("READY")));
    preview_surface_->statusPill()->setTone(
        recording ? ui::widgets::StatusPill::Tone::Recording
                  : (blocked ? ui::widgets::StatusPill::Tone::Blocked : ui::widgets::StatusPill::Tone::Ready));
    preview_surface_->setCenterSubtitle(recording ? QStringLiteral("CAPTURING")
                                                  : QStringLiteral("IDLE · DDA TEX SHARED"));
    preview_surface_->setBottomRightText("AV1 · CQ 24");

    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        const QString target_desc = QString::fromStdString(target.description);
        capture_header_->setMeta(QString("%1 · 60 fps").arg(target_desc.isEmpty() ? "Target" : target_desc));
        preview_surface_->setTopMetaText(QString("%1 · 60 fps").arg(target_desc.isEmpty() ? "TARGET" : target_desc));
    } else {
        capture_header_->setMeta("NO TARGET");
        preview_surface_->setTopMetaText("NO TARGET");
    }

    updateTargetCards();
    updateReadinessRows();
    updateAudioControls();
    updateAudioTrackPreview();
    updateAudioPlaceholders();
    updateStatsDisplay();

    result_panel_->setVisible(view_model_.HasResult());
    if (view_model_.HasResult())
        updateResultDisplay();

    if (view_model_.selected_target_index >= 0 &&
        view_model_.selected_target_index < static_cast<int>(view_model_.targets.size())) {
        const auto& target = view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)];
        emit chromeStateChanged(recording, recording ? QStringLiteral("REC") : QStringLiteral("READY"),
                                QString("%1 · 60 fps · AV1")
                                    .arg(QString::fromStdString(target.description).isEmpty()
                                             ? targetLabelFor(target)
                                             : QString::fromStdString(target.description)));
    } else {
        emit chromeStateChanged(recording, recording ? QStringLiteral("REC") : QStringLiteral("READY"),
                                QString("NO TARGET · 60 fps · AV1"));
    }
}

void RecordPage::updateStatsDisplay() {
    timer_label_->setText(toClock(view_model_.elapsed_text));
    size_value_label_->setText(QString::fromStdWString(view_model_.output_size_text));
    est_value_label_->setText("~-- GB/h");
    preview_surface_->setBottomLeftText(QString("FRAMES %1 · VPKT %2 · DROP %3")
                                            .arg(view_model_.frames_captured)
                                            .arg(view_model_.video_packets)
                                            .arg(view_model_.dropped_frames));
}

void RecordPage::updateResultDisplay() {
    setStyledStringProperty(result_status_label_, "labelRole",
                            view_model_.last_succeeded ? "resultStatusOk" : "resultStatusErr");
    result_status_label_->setText(QString::fromStdWString(view_model_.result_status_text));

    const QString output_path = QString::fromStdWString(view_model_.result_output_path).trimmed();
    const QString error_phase = QString::fromStdWString(view_model_.result_error_phase).trimmed();
    const QString hresult = QString::fromStdWString(view_model_.result_hresult_text).trimmed();
    const QString detail = QString::fromStdWString(view_model_.result_error_detail).trimmed();

    result_path_label_->setText("Output: " + output_path);
    result_path_label_->setVisible(!output_path.isEmpty());
    result_phase_label_->setText("Phase: " + error_phase);
    result_phase_label_->setVisible(!error_phase.isEmpty());
    result_hresult_label_->setText("HRESULT: " + hresult);
    result_hresult_label_->setVisible(!hresult.isEmpty());
    result_detail_label_->setText("Detail: " + detail);
    result_detail_label_->setVisible(!detail.isEmpty());
}

void RecordPage::updateTargetCards() {
    const int current = target_combo_->currentIndex();
    const bool monitor_selected = (current == monitor_target_index_);
    const bool window_selected = (current == window_target_index_);
    const bool region_selected = (current == region_target_index_ && region_target_index_ >= 0);

    monitor_card_->setSelected(monitor_selected);
    window_card_->setSelected(window_selected);
    region_card_->setSelected(region_selected);
    region_card_->setEnabled(region_target_index_ >= 0);

    if (monitor_target_index_ >= 0 && monitor_target_index_ < static_cast<int>(view_model_.targets.size())) {
        monitor_card_->setSubtitle(
            QString::fromStdString(view_model_.targets[static_cast<std::size_t>(monitor_target_index_)].description));
    } else {
        monitor_card_->setSubtitle("No monitor target detected");
    }

    if (window_target_index_ >= 0 && window_target_index_ < static_cast<int>(view_model_.targets.size())) {
        window_card_->setSubtitle(
            QString::fromStdString(view_model_.targets[static_cast<std::size_t>(window_target_index_)].description));
    } else {
        window_card_->setSubtitle("No window target detected");
    }

    if (region_target_index_ >= 0) {
        region_card_->setSubtitle("Region target available");
        if (!region_selected)
            region_card_->setStatusText("○");
    } else {
        region_card_->setSubtitle("Not available in this version");
        region_card_->setStatusText("○");
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
            ? QString::fromStdString(
                  view_model_.targets[static_cast<std::size_t>(view_model_.selected_target_index)].description)
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
        {true, false, QString("WASAPI loopback path available (no live meter binding in this pass)")},
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

void RecordPage::updateAudioPlaceholders() {
    const bool recording =
        (view_model_.state == UiRecordingState::Recording || view_model_.state == UiRecordingState::Stopping);

    app_meter_->setActive(true);
    mic_meter_->setActive(true);
    sys_meter_->setActive(true);
    if (recording) {
        app_meter_->setLevel(0.58F);
        mic_meter_->setLevel(0.26F);
        sys_meter_->setLevel(0.34F);
        app_db_label_->setText("-19 dB · placeholder");
        mic_db_label_->setText("-31 dB · placeholder");
        sys_db_label_->setText("-28 dB · placeholder");
    } else {
        app_meter_->setLevel(0.36F);
        mic_meter_->setLevel(0.18F);
        sys_meter_->setLevel(0.22F);
        app_db_label_->setText("-24 dB · placeholder");
        mic_db_label_->setText("-35 dB · placeholder");
        sys_db_label_->setText("-32 dB · placeholder");
    }
}

} // namespace exosnap
