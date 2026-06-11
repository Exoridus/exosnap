#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include "../theme/ExoSnapMetrics.h"

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QPushButton;

namespace exosnap::ui::brand {
class BrandMarkWidget;
}

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

    // A top-navigation entry. A non-negative page_index selects a QStackedWidget page; a
    // negative page_index marks an action item (e.g. About) that opens a dialog instead of
    // switching the routed page.
    struct NavItem {
        QString label;
        int page_index = -1;
    };

    explicit OperationalTitleBar(QWidget* parent = nullptr);

    static constexpr int kHeight = ui::theme::ExoSnapMetrics::kTitlebarHeight;

    // Builds the top-navigation tabs. Page items become checkable tabs in an exclusive group;
    // action items become plain buttons that emit aboutRequested().
    void setNavItems(const QVector<NavItem>& items);
    // Highlights the tab bound to page_index (no-op when no tab maps to it).
    void setActivePage(int page_index);

    void setRecordingActive(bool recording);
    bool isRecordingActive() const noexcept;

    void setStatusLabel(const QString& status_text);
    // DF-11: updates dropped-frame counter shown in the Recording pill (no-op outside Recording state).
    void setRecordingDropCount(int drops);

    void setMaximizedState(bool maximized);

    bool isInDragArea(const QPoint& local_pos) const;
    WindowButtonHit hitTestWindowButton(const QPoint& local_pos) const;
    void resetDragCursor();
    QRect maximizeButtonRectInWindow() const;

  signals:
    void navPageRequested(int page_index);
    void aboutRequested();
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
    ui::brand::BrandMarkWidget* brand_mark_ = nullptr;
    QHBoxLayout* nav_layout_ = nullptr;
    QButtonGroup* nav_group_ = nullptr;
    ui::widgets::StatusPill* status_pill_ = nullptr;
    QPushButton* minimize_btn_ = nullptr;
    QPushButton* maximize_btn_ = nullptr;
    QPushButton* close_btn_ = nullptr;
    QPoint drag_press_global_pos_;
    bool tracking_drag_from_max_ = false;
    bool move_cursor_active_ = false;

    bool recording_active_ = false;
    QString status_label_ = QStringLiteral("READY");
    int recording_drop_count_ = 0; // DF-11: dropped frames shown in Recording pill

    void refreshStatusChip();
};

} // namespace exosnap::ui::chrome
