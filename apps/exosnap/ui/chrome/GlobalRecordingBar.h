#pragma once

#include <QWidget>

#include "../theme/ExoSnapMetrics.h"

namespace exosnap::ui::widgets {
class StatusPill;
}

namespace exosnap::ui::chrome {

class GlobalRecordingBar : public QWidget {
    Q_OBJECT
  public:
    explicit GlobalRecordingBar(QWidget* parent = nullptr);

    static constexpr int kHeight = 40;

    void setStatusLabel(const QString& status_text);
    const QString& statusLabel() const;

  private:
    void refreshVisualState();
    void refreshStatusChip();

    static QString normalizeStatusLabel(const QString& status_text);

    ui::widgets::StatusPill* status_pill_ = nullptr;

    QString status_label_ = QStringLiteral("READY");
};

} // namespace exosnap::ui::chrome
