#pragma once
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace exosnap {

class OutputPage : public QWidget {
    Q_OBJECT
  public:
    explicit OutputPage(QWidget* parent = nullptr);

  private:
    void onContainerChanged(int id);
    void onBrowse();
    void updateAudioCodecChoices();

    QButtonGroup* container_group_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QLineEdit* destination_edit_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QLabel* mp4_info_label_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
};

} // namespace exosnap
