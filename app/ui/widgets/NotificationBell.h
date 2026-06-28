#pragma once
#include <QColor>
#include <QToolButton>

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Notification bell with optional unread badge.
// Lucide "bell" icon; badge uses kWarn color for count > 0.
// signal clicked() is inherited from QAbstractButton.
class NotificationBell : public QToolButton {
    Q_OBJECT
  public:
    explicit NotificationBell(QWidget* parent = nullptr);

    // 0 = resting (no badge), >0 = shows badge with count.
    void setUnreadCount(int count);
    int unreadCount() const {
        return unread_count_;
    }

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

    int unread_count_ = 0;
    bool hovered_ = false;
    bool hub_open_ = false;
};

} // namespace exosnap::ui::widgets
