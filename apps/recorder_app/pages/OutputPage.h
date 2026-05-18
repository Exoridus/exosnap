#pragma once
#include <QWidget>

namespace exosnap {
class OutputPage : public QWidget {
    Q_OBJECT
public:
    explicit OutputPage(QWidget* parent = nullptr);
};
} // namespace exosnap
