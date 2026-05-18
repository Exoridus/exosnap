#pragma once
#include <QWidget>

namespace exosnap {
class AudioPage : public QWidget {
    Q_OBJECT
  public:
    explicit AudioPage(QWidget* parent = nullptr);
};
} // namespace exosnap
