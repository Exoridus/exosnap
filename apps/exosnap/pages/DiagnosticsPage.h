#pragma once
#include <QWidget>

namespace exosnap {
class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPage(QWidget* parent = nullptr);
};
} // namespace exosnap
