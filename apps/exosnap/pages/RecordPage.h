#pragma once
#include <QWidget>
#include <memory>

#include "../services/RecordingCoordinator.h"
#include "../viewmodels/RecordViewModel.h"

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;

namespace exosnap {

class RecordPage : public QWidget {
    Q_OBJECT
  public:
    explicit RecordPage(QWidget* parent = nullptr);

  private slots:
    void onStart();
    void onStop();

  private:
    void initCoordinator();
    void refresh();
    void updateStatsDisplay();
    void updateResultDisplay();

    RecordViewModel view_model_;
    std::unique_ptr<RecordingCoordinator> coordinator_;

    QLabel* capability_label_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    QLabel* output_path_label_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QLabel* state_label_ = nullptr;
    QFrame* stats_panel_ = nullptr;
    QLabel* elapsed_label_ = nullptr;
    QLabel* frames_label_ = nullptr;
    QLabel* video_packets_label_ = nullptr;
    QLabel* audio_packets_label_ = nullptr;
    QLabel* dropped_label_ = nullptr;
    QLabel* size_label_ = nullptr;
    QFrame* result_panel_ = nullptr;
    QLabel* result_status_label_ = nullptr;
    QLabel* result_path_label_ = nullptr;
    QLabel* result_phase_label_ = nullptr;
    QLabel* result_hresult_label_ = nullptr;
    QLabel* result_detail_label_ = nullptr;
};

} // namespace exosnap
