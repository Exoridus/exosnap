#pragma once

#include "notifications/NotificationEvent.h"

#include <QAbstractListModel>
#include <QTimer>
#include <QVector>

namespace exosnap::notifications {
class NotificationManager;
}

namespace exosnap::quick {

// The transient glance, as rows.
//
// The hub is the record and NotificationEntryModel owns it; this is the other,
// much shorter list: exactly what NotificationManager::VisibleEvents() currently
// holds. It never decides what is visible, how long a toast lives or which one
// replaces which — it mirrors, so the out-of-window toast stack and the manager
// can never disagree about what is on screen.
//
// The countdown ticker runs only while a timed toast is visible. A standing
// toast reports a condition that still holds and has nothing to count down.
class NotificationToastModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role {
        SequenceRole = Qt::UserRole + 1,
        TitleRole,
        BodyRole,
        ToneRole,     // "success" | "caution" | "error" | "info"
        StandingRole, // never auto-dismisses; no countdown hairline
        RemainingFractionRole,
        ActionCountRole, // 0, 1 or 2 — one means the whole card is the action
        PrimaryLabelRole,
        PrimaryActionRole, // int(NotificationAction)
        SecondaryLabelRole,
        SecondaryActionRole,
    };

    explicit NotificationToastModel(QObject* parent = nullptr);

    // `manager` must outlive this model.
    void attach(notifications::NotificationManager* manager);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // The event behind a sequence number, or nullptr when it is no longer
    // visible — a click on a card the manager has just retired must not replay
    // an action that is gone.
    [[nodiscard]] const notifications::NotificationEvent* eventForSequence(quint64 sequence) const;

  private:
    void refresh();
    void updateCountdown();

    notifications::NotificationManager* manager_ = nullptr;
    QVector<notifications::NotificationEvent> visible_;
    QTimer countdown_;
};

} // namespace exosnap::quick
