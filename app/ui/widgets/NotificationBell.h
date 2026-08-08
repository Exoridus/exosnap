#pragma once
#include <QColor>
#include <QToolButton>

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Notification bell with an optional unread dot.
// Lucide "bell" icon; the dot is coloured by the worst unread severity.
// signal clicked() is inherited from QAbstractButton.
//
// The dot replaced a counted badge: a 9 px digit inside a 14x13 px field put the
// glyph at roughly 5 px tall, below what IBM Plex Mono rasterises cleanly. The
// exact count was never actionable from the title bar anyway — it lives in the
// hub, one click away — so the dot carries only what the bar can say legibly:
// that something is unread, and how urgent the worst of it is.
class NotificationBell : public QToolButton {
    Q_OBJECT
  public:
    explicit NotificationBell(QWidget* parent = nullptr);

    // Sized for the 40 px title bar: the bell needs vertical clearance on both
    // sides of the bar, so it cannot simply grow with the row.
    static constexpr int kSize = 28;
    static constexpr int kIconSize = 16;
    static constexpr int kDotDiameter = 8;

    // Advisory status key of the worst unread entry: "error", "caution",
    // "info" or "success". Empty means nothing is unread and no dot is drawn.
    // Values are produced by notifications::AdvisoryStatusForType(); an
    // unrecognised non-empty string still draws a dot, in the neutral colour.
    void setUnreadStatus(const QString& status);
    QString unreadStatus() const {
        return unread_status_;
    }
    bool hasUnread() const {
        return !unread_status_.isEmpty();
    }

    // Centre of the unread dot, in widget coordinates. Anchored to the bell
    // glyph's top-right corner rather than to the widget edge: the icon is
    // centred inside a larger button, so an edge-anchored dot floats away from
    // the glyph instead of reading as attached to it.
    QPoint dotCenter() const;

    // VG-2: open/closed hub state — drives the QSS [hubOpen="true"] rule.
    // Call this from wherever the hub panel is shown/hidden (e.g. MainWindow::toggleNotificationHub).
    void setHubOpen(bool open);
    bool hubOpen() const {
        return hub_open_;
    }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void updateIcon();
    // Resolves unread_status_ to the dot fill from the active theme.
    QColor dotColor() const;

    QString unread_status_;
    bool hovered_ = false;
    bool hub_open_ = false;
};

} // namespace exosnap::ui::widgets
