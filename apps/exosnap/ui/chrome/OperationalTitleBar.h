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

    void setMaximizedState(bool maximized);

    bool isInDragArea(const QPoint& local_pos) const;
    WindowButtonHit hitTestWindowButton(const QPoint& local_pos) const;
    QRect maximizeButtonRectInWindow() const;

  signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QWidget* brand_block_ = nullptr;
    QLabel* page_code_label_ = nullptr;
    ui::widgets::StatusPill* status_pill_ = nullptr;
    QLabel* context_label_ = nullptr;
    QLabel* cpu_label_ = nullptr;
    QLabel* gpu_label_ = nullptr;
    QLabel* ram_label_ = nullptr;
    QPushButton* minimize_btn_ = nullptr;
    QPushButton* maximize_btn_ = nullptr;
    QPushButton* close_btn_ = nullptr;
    bool recording_active_ = false;
};

} // namespace exosnap::ui::chrome
