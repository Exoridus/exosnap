#pragma once

#include <QWidget>

#include "../theme/ExoSnapMetrics.h"

class QLabel;
class QPushButton;
class QFrame;
class QResizeEvent;

namespace exosnap::ui::widgets {
class StatusPill;
}

namespace exosnap::ui::chrome {

class GlobalRecordingBar : public QWidget {
    Q_OBJECT
  public:
    explicit GlobalRecordingBar(QWidget* parent = nullptr);

    static constexpr int kHeight = 50;

    void setStatusLabel(const QString& status_text);
    const QString& statusLabel() const;

    void setProfileSummary(const QString& summary_text);
    void setTargetSummary(const QString& summary_text);
    void setOutputSummary(const QString& summary_text);
    void setRuntimeSummary(const QString& summary_text);

  signals:
    void primaryActionRequested();
    void pauseActionRequested();
    void micActionRequested();
    void markerActionRequested();
    void overlayActionRequested();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void refreshVisualState();
    void refreshStatusChip();
    void refreshActionLabels();
    void refreshSummaryLabels();
    void applyCompactLayout();

    void setSummaryLabel(QLabel* label, const QString& summary_text, int max_chars);

    static QString normalizeStatusLabel(const QString& status_text);
    static QString normalizeSummaryText(const QString& summary_text);
    static QString clipSummaryText(const QString& summary_text, int max_chars);

    ui::widgets::StatusPill* status_pill_ = nullptr;
    QPushButton* primary_action_button_ = nullptr;
    QPushButton* pause_action_button_ = nullptr;
    QPushButton* mic_action_button_ = nullptr;
    QPushButton* marker_action_button_ = nullptr;
    QPushButton* overlay_action_button_ = nullptr;

    QWidget* output_summary_slot_ = nullptr;
    QWidget* runtime_summary_slot_ = nullptr;
    QFrame* output_separator_ = nullptr;
    QFrame* runtime_separator_ = nullptr;

    QLabel* profile_summary_value_ = nullptr;
    QLabel* target_summary_value_ = nullptr;
    QLabel* output_summary_value_ = nullptr;
    QLabel* runtime_summary_value_ = nullptr;

    QString status_label_ = QStringLiteral("READY");
    QString profile_summary_text_ = QStringLiteral("-");
    QString target_summary_text_ = QStringLiteral("-");
    QString output_summary_text_ = QStringLiteral("-");
    QString runtime_summary_text_ = QStringLiteral("DUR --:--:-- · SIZE -");
};

} // namespace exosnap::ui::chrome
