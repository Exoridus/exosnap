#pragma once

#include <QWidget>

#include "../theme/ExoSnapMetrics.h"

class QLabel;
class QPushButton;

namespace exosnap::ui::widgets {
class StatusPill;
}

namespace exosnap::ui::chrome {

class OperationalTitleBar : public QWidget {
    Q_OBJECT
  public:
    enum class WindowButtonHit {
        None,
        Minimize,
        MaximizeRestore,
        Close,
    };

    explicit OperationalTitleBar(QWidget* parent = nullptr);

    static constexpr int kHeight = ui::theme::ExoSnapMetrics::kTitlebarHeight;

    void setPageContext(const QString& page_code, const QString& context_text);
    void setRecordingActive(bool recording);
    bool isRecordingActive() const noexcept;

    void setStatusLabel(const QString& status_text);
    void setRuntimeMeta(const QString& cpu_text, const QString& gpu_text, const QString& ram_text);
    void setRecordingRuntime(const QString& elapsed_text, const QString& bitrate_text, const QString& drop_text);

    void setMaximizedState(bool maximized);

    bool isInDragArea(const QPoint& local_pos) const;
    WindowButtonHit hitTestWindowButton(const QPoint& local_pos) const;
    void resetDragCursor();
    QRect maximizeButtonRectInWindow() const;

  signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

  private:
    QLabel* page_code_label_ = nullptr;
    ui::widgets::StatusPill* status_pill_ = nullptr;
    QLabel* context_label_ = nullptr;
    QLabel* metrics_label_ = nullptr;
    QPushButton* minimize_btn_ = nullptr;
    QPushButton* maximize_btn_ = nullptr;
    QPushButton* close_btn_ = nullptr;
    QPoint drag_press_global_pos_;
    bool tracking_drag_from_max_ = false;
    bool move_cursor_active_ = false;

    bool recording_active_ = false;
    QString status_label_ = QStringLiteral("READY");
    QString idle_cpu_text_ = QStringLiteral("–");
    QString idle_gpu_text_ = QStringLiteral("–");
    QString idle_ram_text_ = QStringLiteral("–");
    QString rec_elapsed_text_ = QStringLiteral("--:--:--");
    QString rec_bitrate_text_ = QStringLiteral("–");
    QString rec_drop_text_ = QStringLiteral("–");

    void refreshStatusChip();
    void refreshMetricsLabel();
};

} // namespace exosnap::ui::chrome
