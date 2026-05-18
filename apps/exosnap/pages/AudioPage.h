#pragma once
#include <QWidget>

class QCheckBox;
class QLabel;

namespace exosnap {

class AudioPage : public QWidget {
    Q_OBJECT
  public:
    explicit AudioPage(QWidget* parent = nullptr);

  private:
    void onSourceStateChanged();
    void updateMergeVisibility();
    void updateResultingTracks();

    QCheckBox* app_enable_ = nullptr;
    QCheckBox* mic_enable_ = nullptr;
    QCheckBox* mic_merge_ = nullptr;
    QCheckBox* sys_enable_ = nullptr;
    QCheckBox* sys_merge_ = nullptr;
    QLabel* resulting_tracks_label_ = nullptr;
};

} // namespace exosnap
