#pragma once
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QHBoxLayout;
class QVBoxLayout;
class QPushButton;

namespace exosnap::ui::widgets {

// PS-FOUNDATIONS-R1: Notification-hub advisory item.
// Layout: [StatusIcon | VBox[Title, Body] | VBox[Time, UnreadDot]] + action buttons.
class AdvisoryItem : public QWidget {
    Q_OBJECT
  public:
    explicit AdvisoryItem(QWidget* parent = nullptr);

    // "success" | "caution" | "error" | "info"
    void setStatus(const QString& status);
    void setTitle(const QString& title);
    void setBody(const QString& body);
    void setTimeLabel(const QString& time);
    void setUnread(bool unread);
    // isDeepLink = true: action shows a chevron-right indicator and emits deepLinkRequested()
    void addAction(const QString& id, const QString& label, bool isDeepLink = false);

  Q_SIGNALS:
    void actionTriggered(const QString& id);
    void deepLinkRequested();

  private:
    void updateStatusIcon();
    void updateUnreadDot();

    QString status_{"info"};
    bool unread_{false};

    QLabel* status_icon_label_{nullptr};
    QLabel* title_label_{nullptr};
    QLabel* body_label_{nullptr};
    QLabel* time_label_{nullptr};
    QWidget* unread_dot_{nullptr};
    QWidget* actions_container_{nullptr};
    QHBoxLayout* actions_layout_{nullptr};

    struct ActionEntry {
        QString id;
        QString label;
        bool is_deep_link;
    };
    QVector<ActionEntry> actions_;
};

} // namespace exosnap::ui::widgets
