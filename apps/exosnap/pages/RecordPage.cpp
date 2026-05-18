#include "RecordPage.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace exosnap {

namespace {

QLabel* makeTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 22px; font-weight: 600; color: #E8EAED;");
    return l;
}

QLabel* makeSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("color: #8A9099; font-size: 13px;");
    l->setWordWrap(true);
    return l;
}

QLabel* makeSectionLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("font-size: 13px; font-weight: 600; color: #C0C4CC; margin-top: 4px;");
    return l;
}

QFrame* makePanel(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setStyleSheet("QFrame { background: #141A26; border-radius: 6px; padding: 2px; }");
    return f;
}

} // namespace

RecordPage::RecordPage(QWidget* parent) : QWidget(parent) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    content->setStyleSheet("QWidget { background: transparent; }");
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(14);

    layout->addWidget(makeTitle("Record", content));
    layout->addWidget(
        makeSubLabel("Operational view for target selection, readiness, and live runtime stats.", content));

    // Capability banner
    capability_label_ = new QLabel(content);
    capability_label_->setWordWrap(true);
    capability_label_->setStyleSheet("background: #1A2133; border-radius: 5px;"
                                     "padding: 10px 14px; color: #8A9099; font-size: 12px;");
    layout->addWidget(capability_label_);

    // Preview surface
    layout->addWidget(makeSectionLabel("Live Preview", content));
    auto* previewBox = new QFrame(content);
    previewBox->setFixedHeight(180);
    previewBox->setStyleSheet("QFrame { background: #0D1017; border-radius: 6px; border: 1px solid #1E2535; }");
    auto* previewLayout = new QVBoxLayout(previewBox);
    auto* previewHint = new QLabel("Preview surface placeholder", previewBox);
    previewHint->setAlignment(Qt::AlignCenter);
    previewHint->setStyleSheet("color: #3E4555; font-size: 12px;");
    previewLayout->addWidget(previewHint);
    layout->addWidget(previewBox);

    // Capture target
    layout->addWidget(makeSectionLabel("Capture Target", content));
    target_combo_ = new QComboBox(content);
    target_combo_->setMinimumWidth(300);
    target_combo_->setStyleSheet("QComboBox { background: #1A2133; border: 1px solid #2A3349;"
                                 " border-radius: 4px; padding: 6px 12px; color: #C8CBD0; }"
                                 "QComboBox::drop-down { border: none; }"
                                 "QComboBox QAbstractItemView { background: #1A2133; color: #C8CBD0;"
                                 " selection-background-color: #263050; border: 1px solid #2A3349; }");
    layout->addWidget(target_combo_);

    // Output path
    layout->addWidget(makeSectionLabel("Output Path", content));
    output_path_label_ = new QLabel("--", content);
    output_path_label_->setWordWrap(true);
    output_path_label_->setStyleSheet("color: #8A9099; font-size: 12px;");
    layout->addWidget(output_path_label_);

    // Readiness
    layout->addWidget(makeSectionLabel("Readiness", content));
    layout->addWidget(makeSubLabel("Start is blocked automatically while active blockers are present.", content));

    // Controls
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);

    start_btn_ = new QPushButton("Start Recording", content);
    start_btn_->setStyleSheet("QPushButton { background: #2468C0; border: none; border-radius: 4px;"
                              " padding: 8px 18px; color: #FFFFFF; font-weight: 600; }"
                              "QPushButton:hover { background: #3078D0; }"
                              "QPushButton:pressed { background: #1A58B0; }"
                              "QPushButton:disabled { background: #1E2535; color: #454C5E; }");

    stop_btn_ = new QPushButton("Stop Recording", content);
    stop_btn_->setStyleSheet("QPushButton { background: #252C3C; border: 1px solid #3A4254;"
                             " border-radius: 4px; padding: 8px 18px; color: #C0C4CC; }"
                             "QPushButton:hover { background: #2E3648; }"
                             "QPushButton:disabled { background: #1A2030; color: #454C5E; }");

    btnRow->addWidget(start_btn_);
    btnRow->addWidget(stop_btn_);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    state_label_ = new QLabel(content);
    state_label_->setStyleSheet("color: #8A9099; font-size: 12px;");
    layout->addWidget(state_label_);

    // Audio activity placeholder
    layout->addWidget(makeSectionLabel("Audio activity", content));
    layout->addWidget(makeSubLabel("APP ◂◅◇    MIC ◂◃    SYS ◂◅◇", content));

    // Live stats panel
    stats_panel_ = makePanel(content);
    auto* statsLayout = new QVBoxLayout(stats_panel_);
    statsLayout->setSpacing(4);
    statsLayout->setContentsMargins(14, 10, 14, 10);
    elapsed_label_ = new QLabel(stats_panel_);
    frames_label_ = new QLabel(stats_panel_);
    video_packets_label_ = new QLabel(stats_panel_);
    audio_packets_label_ = new QLabel(stats_panel_);
    dropped_label_ = new QLabel(stats_panel_);
    size_label_ = new QLabel(stats_panel_);
    for (auto* lbl :
         {elapsed_label_, frames_label_, video_packets_label_, audio_packets_label_, dropped_label_, size_label_}) {
        lbl->setStyleSheet("color: #C0C4CC; font-size: 12px;");
        statsLayout->addWidget(lbl);
    }
    stats_panel_->hide();
    layout->addWidget(stats_panel_);

    // Result panel
    result_panel_ = makePanel(content);
    auto* resultLayout = new QVBoxLayout(result_panel_);
    resultLayout->setSpacing(4);
    resultLayout->setContentsMargins(14, 10, 14, 10);
    result_status_label_ = new QLabel(result_panel_);
    result_path_label_ = new QLabel(result_panel_);
    result_phase_label_ = new QLabel(result_panel_);
    result_hresult_label_ = new QLabel(result_panel_);
    result_detail_label_ = new QLabel(result_panel_);
    for (auto* lbl :
         {result_status_label_, result_path_label_, result_phase_label_, result_hresult_label_, result_detail_label_}) {
        lbl->setWordWrap(true);
        lbl->setStyleSheet("color: #C0C4CC; font-size: 12px;");
        resultLayout->addWidget(lbl);
    }
    result_panel_->hide();
    layout->addWidget(result_panel_);

    layout->addStretch();
    scroll->setWidget(content);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->addWidget(scroll);

    connect(start_btn_, &QPushButton::clicked, this, &RecordPage::onStart);
    connect(stop_btn_, &QPushButton::clicked, this, &RecordPage::onStop);

    initCoordinator();
}

void RecordPage::initCoordinator() {
    coordinator_ = std::make_unique<RecordingCoordinator>();

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
    for (const auto& t : view_model_.targets) {
        bool isMonitor = (t.kind == recorder_core::CaptureTarget::Kind::Monitor);
        std::wstring prefix = isMonitor ? L"[Monitor] " : L"[Window] ";
        view_model_.target_display_names.push_back(prefix + t.description);
        target_combo_->addItem(QString::fromStdWString(prefix + t.description));
    }

    view_model_.selected_target_index = -1;
    for (int i = 0; i < static_cast<int>(view_model_.targets.size()); ++i) {
        if (view_model_.targets[i].kind == recorder_core::CaptureTarget::Kind::Monitor) {
            view_model_.selected_target_index = i;
            target_combo_->setCurrentIndex(i);
            break;
        }
    }
    if (view_model_.selected_target_index < 0 && !view_model_.targets.empty()) {
        view_model_.selected_target_index = 0;
        target_combo_->setCurrentIndex(0);
    }

    view_model_.SetState(coordinator_->State());
    view_model_.capability_status_text = coordinator_->CapabilityStatusText();
    refresh();
}

void RecordPage::onStart() {
    int idx = target_combo_->currentIndex();
    view_model_.selected_target_index = idx;
    if (idx < 0 || idx >= static_cast<int>(view_model_.targets.size()))
        return;
    view_model_.ResetStats();
    coordinator_->StartRecording(view_model_.targets[idx]);
}

void RecordPage::onStop() {
    coordinator_->StopRecording();
}

void RecordPage::refresh() {
    capability_label_->setText(QString::fromStdWString(view_model_.capability_status_text));
    target_combo_->setEnabled(view_model_.CanStart());
    output_path_label_->setText(QString::fromStdWString(view_model_.output_path_display));
    start_btn_->setEnabled(view_model_.CanStart());
    stop_btn_->setEnabled(view_model_.CanStop());
    state_label_->setText(QString::fromStdWString(view_model_.state_text));
    stats_panel_->setVisible(view_model_.ShouldShowStats());
    if (view_model_.ShouldShowStats())
        updateStatsDisplay();
    result_panel_->setVisible(view_model_.HasResult());
    if (view_model_.HasResult())
        updateResultDisplay();
}

void RecordPage::updateStatsDisplay() {
    elapsed_label_->setText("Elapsed: " + QString::fromStdWString(view_model_.elapsed_text));
    frames_label_->setText("Frames: " + QString::number(view_model_.frames_captured));
    video_packets_label_->setText("Video packets: " + QString::number(view_model_.video_packets));
    audio_packets_label_->setText("Audio packets: " + QString::number(view_model_.audio_packets));
    dropped_label_->setText("Dropped frames: " + QString::number(view_model_.dropped_frames));
    size_label_->setText("Size: " + QString::fromStdWString(view_model_.output_size_text));
}

void RecordPage::updateResultDisplay() {
    result_status_label_->setText(QString::fromStdWString(view_model_.result_status_text));
    result_path_label_->setText(QString::fromStdWString(view_model_.result_output_path));
    result_phase_label_->setText(QString::fromStdWString(view_model_.result_error_phase));
    result_hresult_label_->setText(QString::fromStdWString(view_model_.result_hresult_text));
    result_detail_label_->setText(QString::fromStdWString(view_model_.result_error_detail));
}

} // namespace exosnap
