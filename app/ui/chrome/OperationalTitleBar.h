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
class NotificationBell;
class StatusPill;
} // namespace exosnap::ui::widgets

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

    void setMaximizedState(bool maximized);

    // Forces one window control button ("minimize" / "maximize" / "close") to paint
    // its hover state. Any other value (including empty) clears hover from all three.
    // A real mouse hover is unaffected and keeps using the normal :hover QSS path.
    //
    // Two callers:
    //   - the visual-test harness, so a static screenshot can show a hover state
    //     without a real OS mouse move;
    //   - MainWindow's WM_NCMOUSEMOVE handling. The maximize cell answers
    //     WM_NCHITTEST with HTMAXBUTTON to get the Snap Layouts flyout, which puts
    //     it in the non-client area — Qt stops delivering enter/leave events for it,
    //     so its hover has to be driven from the native message stream instead.
    void setForcedWindowButtonHover(const QString& which);

    // PS-PHASE-B: notification bell API. Takes the worst unread advisory status
    // ("error" / "caution" / "info" / "success"); empty means nothing unread.
    void setBellUnreadStatus(const QString& status);
    ui::widgets::NotificationBell* bellWidget() const {
        return bell_;
    }

    // Re-bakes the two-tone wordmark rich-text from ActiveTheme(). Call after a theme switch
    // so the "exo" colour is correct for the current (possibly persisted) theme.
    void refreshBrand();

    // True where the bar may be dragged, i.e. everywhere no interactive child sits.
    // MainWindow's WM_NCHITTEST returns HTCAPTION for exactly this area, so anything
    // wrongly reported as draggable stops receiving mouse events altogether.
    bool isInDragArea(const QPoint& local_pos) const;
    WindowButtonHit hitTestWindowButton(const QPoint& local_pos) const;
    // Toggles the window between maximized and normal. Driven from WM_NCLBUTTONUP:
    // the maximize button is HTMAXBUTTON and so never sees a Qt click.
    void triggerMaximizeRestore();

  signals:
    void navPageRequested(int page_index);
    void aboutRequested();
    void bellClicked(); // PS-PHASE-B: emitted when the notification bell is clicked.
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

  protected:
    // No mouse handlers: dragging, restore-on-drag and double-click-to-maximize are
    // Windows' own, reached through HTCAPTION in MainWindow's WM_NCHITTEST.
    void paintEvent(QPaintEvent* event) override;

  private:
    ui::brand::BrandMarkWidget* brand_mark_ = nullptr;
    QLabel* wordmark_ = nullptr; // stored to allow refreshBrand() to re-bake rich-text
    QHBoxLayout* nav_layout_ = nullptr;
    QButtonGroup* nav_group_ = nullptr;
    ui::widgets::StatusPill* status_pill_ = nullptr;
    ui::widgets::NotificationBell* bell_ = nullptr; // PS-PHASE-B
    QPushButton* minimize_btn_ = nullptr;
    QPushButton* maximize_btn_ = nullptr;
    QPushButton* close_btn_ = nullptr;

    bool recording_active_ = false;
    QString status_label_ = QStringLiteral("READY");

    void refreshStatusChip();
};

} // namespace exosnap::ui::chrome
