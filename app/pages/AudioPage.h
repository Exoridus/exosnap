#pragma once
#include <QWidget>

namespace exosnap::ui::widgets {
class AudioSourceRow;
}

namespace exosnap {

class AudioPage : public QWidget {
    Q_OBJECT
  public:
    explicit AudioPage(QWidget* parent = nullptr);

  private:
    ui::widgets::AudioSourceRow* app_row_ = nullptr;
    ui::widgets::AudioSourceRow* mic_row_ = nullptr;
    ui::widgets::AudioSourceRow* sys_row_ = nullptr;
};

} // namespace exosnap
