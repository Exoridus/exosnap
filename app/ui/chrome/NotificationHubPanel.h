#pragma once
#include <QFrame>
#include <QMap>
#include <QString>

class QLabel;
class QScrollArea;
class QVBoxLayout;

namespace exosnap::ui::widgets {
class AdvisoryItem;
}

namespace exosnap::ui::chrome {

// PS-PHASE-B: Notification hub panel — frameless popup anchored below the bell.
// Shows a scrollable list of AdvisoryItems, or an empty-state when there are none.
// Deep-link actions emit deepLinkRequested(target) so MainWindow can navigate.
class NotificationHubPanel : public QFrame {
    Q_OBJECT
  public:
    explicit NotificationHubPanel(QWidget* parent = nullptr);

    // Add a persistent advisory. Items are shown in insertion order.
    void addAdvisory(const QString& id, const QString& status, const QString& title, const QString& body,
                     const QString& time_label, bool unread, const QString& action_id, const QString& action_label,
                     bool is_deep_link);

    // Remove all advisories and revert to the empty state.
    void clearAdvisories();

    // PS-PHASE-E: Remove a specific advisory by id. No-op if id not found.
    void removeAdvisoryById(const QString& id);

    // PS-PHASE-E: Returns the current number of unread advisories.
    int unreadCount() const noexcept;

    // Populate with two demo advisories for harness/visual testing.
    // Passing false clears all advisories.
    void setDemoAdvisories(bool enabled);

    // Reposition so the panel's top-right corner aligns with globalPos.
    // Call after showing the panel.
    void anchorToPoint(const QPoint& globalPos);

  Q_SIGNALS:
    // Emitted when an advisory deep-link action is triggered.
    // target is an opaque id (e.g. "settings/audio"); MainWindow navigates to Settings.
    void deepLinkRequested(const QString& target);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    void refreshEmptyState();
    void addAdvisoryWidget(ui::widgets::AdvisoryItem* item, const QString& id, bool unread);

    QLabel* mark_all_read_label_ = nullptr;
    QWidget* list_container_ = nullptr;
    QVBoxLayout* list_layout_ = nullptr;
    QWidget* empty_state_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    int advisory_count_ = 0;
    int unread_count_ = 0;

    // PS-PHASE-E: advisory tracking by id for removeAdvisoryById.
    // Each entry stores the AdvisoryItem widget and its preceding divider (if any).
    struct AdvisoryEntry {
        ui::widgets::AdvisoryItem* item = nullptr;
        QFrame* divider_before = nullptr; // 1px divider widget prepended before this item; null for first
        bool unread = false;
    };
    QMap<QString, AdvisoryEntry> advisory_by_id_;
};

} // namespace exosnap::ui::chrome
