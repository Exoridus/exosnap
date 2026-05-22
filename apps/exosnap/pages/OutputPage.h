#pragma once
#include <QWidget>

#include "../models/OutputSettingsModel.h"

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace exosnap {

class OutputPage : public QWidget {
    Q_OBJECT
  public:
    explicit OutputPage(const OutputSettingsModel& initial_settings, QWidget* parent = nullptr);

  signals:
    void outputSettingsChanged(const OutputSettingsModel& settings);

  private:
    void onContainerChanged(int id);
    void onBrowse();
    void applySettingsToUi();
    void emitCurrentSettings();
    void updateAudioCodecChoices();
    void updateEffectiveOutputPreview();

    OutputSettingsModel settings_;
    QButtonGroup* container_group_ = nullptr;
    QRadioButton* mkv_radio_ = nullptr;
    QRadioButton* mp4_radio_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QLineEdit* destination_edit_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QLabel* mp4_info_label_ = nullptr;
    QLabel* effective_output_path_label_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
};

} // namespace exosnap
