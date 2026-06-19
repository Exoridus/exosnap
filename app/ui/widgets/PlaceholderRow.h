#pragma once
#include <QString>
#include <QWidget>

class QLabel;
class QHBoxLayout;

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Disabled "coming in <ver>" placeholder row.
// Ausgegraute Optik (kText3), Versions-Tag als kleines Badge (kBg3, kLine2, dashed border).
class PlaceholderRow : public QWidget {
    Q_OBJECT
  public:
    explicit PlaceholderRow(QWidget* parent = nullptr);

    void setLabel(const QString& label);
    void setVersionTag(const QString& version); // e.g. "0.7"

  private:
    QLabel* label_{nullptr};
    QLabel* version_badge_{nullptr};
};

} // namespace exosnap::ui::widgets
