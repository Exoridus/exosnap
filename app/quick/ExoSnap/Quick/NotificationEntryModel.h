#pragma once

#include "notifications/NotificationEvent.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// One model over the notification hub's history (product-spec §9: "the hub
// is the record: every notification lands there, persists until dismissed").
//
// This model owns the ONLY copy of that history in the Quick frontend — it is
// fed exclusively by NotificationsAdapter's listener on its
// notifications::NotificationManager, never pushed into from elsewhere, so
// the hub can never drift from what the manager actually recorded (mirrors
// the LogEntryModel note: one model over the one history that already
// exists).
//
// No QImage in any role: every value here is a plain string, int, bool or a
// QVariantList of {action:int, label:string} maps for the `actions` role.
class NotificationEntryModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("NotificationEntryModel is provided by NotificationsAdapter")

  public:
    enum Role {
        KeyRole = Qt::UserRole + 1,
        SequenceRole,
        TitleRole,
        BodyRole,
        ToneRole,          // "success" | "caution" | "error" | "info" (AdvisoryStatusForType)
        TimestampRole,     // qint64 ms since epoch, when this entry was (last) recorded
        TimestampTextRole, // locally formatted "HH:mm"
        UnreadRole,
        ActionsRole,            // QVariantList of {action:int, label:string}, 0-2 entries
        PrimaryActionRole,      // int(NotificationAction); 0 (None) when the entry has no action
        PrimaryActionLabelRole, // "" when PrimaryActionRole is None
        SyntheticRole,          // raised through the automation channel, not by a product condition
    };

    explicit NotificationEntryModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Records a manager event as a new entry, or — when
    // notifications::NotificationHubEntryKey() resolves to an id already
    // present — replaces that entry in place and moves it to the top: a
    // recurring standing condition (or a re-raised one-shot type) is product
    // news the user has not seen yet, so it reads as fresh rather than
    // staying stranded wherever it was first raised.
    void recordEvent(const notifications::NotificationEvent& event);

    [[nodiscard]] int unreadCount() const noexcept;
    // The worst tone among unread entries (notifications::AdvisoryStatusRank),
    // or an empty string when nothing is unread — see
    // NotificationsAdapter::worstUnreadTone().
    [[nodiscard]] QString worstUnreadTone() const;

    // Marks every entry read. No-op (and no dataChanged) for rows already read.
    void markAllRead();
    // Marks a single row read. Out-of-range is a no-op.
    void markRead(int row);

    // Removes a single row. Out-of-range is a no-op.
    void removeAt(int row);
    // Removes the entry with this hub key, if any recorded. Returns whether
    // one was found and removed — the host-only path UpdateAvailable needs
    // (see NotificationsAdapter::removeEntryByKey()).
    bool removeByKey(const QString& key);
    // Empties the whole history.
    void clear();

    // The full event backing a row, for NotificationsAdapter::triggerAction()
    // to validate and re-emit. Returns a default-constructed (action = None)
    // event when row is out of range, which triggerAction's None guard
    // already treats as a no-op.
    [[nodiscard]] const notifications::NotificationEvent& eventAt(int row) const;

  private:
    struct Entry {
        QString key;
        notifications::NotificationEvent event;
        qint64 received_at_ms = 0;
        bool unread = true;
    };

    [[nodiscard]] int indexOfKey(const QString& key) const;
    [[nodiscard]] static QVariantList actionsFor(const Entry& entry);

    QVector<Entry> entries_;
};

} // namespace exosnap::quick
