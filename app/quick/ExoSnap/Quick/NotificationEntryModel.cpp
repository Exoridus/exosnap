#include "NotificationEntryModel.h"

#include "NotificationHubPolicy.h"

#include <QDateTime>
#include <QVariantMap>

namespace exosnap::quick {
namespace {

using notifications::NotificationAction;
using notifications::NotificationEvent;

const NotificationEvent& FallbackEvent() {
    static const NotificationEvent event{};
    return event;
}

} // namespace

NotificationEntryModel::NotificationEntryModel(QObject* parent) : QAbstractListModel(parent) {
}

int NotificationEntryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant NotificationEntryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= entries_.size())
        return {};
    const Entry& entry = entries_.at(index.row());
    switch (role) {
    case KeyRole:
        return entry.key;
    case SequenceRole:
        return QVariant::fromValue<qint64>(static_cast<qint64>(entry.event.sequence));
    case TitleRole:
        return entry.event.title;
    case BodyRole:
        return entry.event.body;
    case ToneRole:
        return notifications::AdvisoryStatusForType(entry.event.type);
    case TimestampRole:
        return entry.received_at_ms;
    case TimestampTextRole:
        return QDateTime::fromMSecsSinceEpoch(entry.received_at_ms).toString(QStringLiteral("HH:mm"));
    case UnreadRole:
        return entry.unread;
    case ActionsRole:
        return actionsFor(entry);
    case PrimaryActionRole:
        return static_cast<int>(entry.event.action);
    case PrimaryActionLabelRole:
        return notifications::NotificationActionLabel(entry.event.action);
    default:
        break;
    }
    return {};
}

QHash<int, QByteArray> NotificationEntryModel::roleNames() const {
    return {
        {KeyRole, "key"},
        {SequenceRole, "sequence"},
        {TitleRole, "title"},
        {BodyRole, "body"},
        {ToneRole, "tone"},
        {TimestampRole, "timestampMs"},
        {TimestampTextRole, "timestampText"},
        {UnreadRole, "unread"},
        {ActionsRole, "actions"},
        {PrimaryActionRole, "primaryAction"},
        {PrimaryActionLabelRole, "primaryActionLabel"},
    };
}

void NotificationEntryModel::recordEvent(const NotificationEvent& event) {
    const QString key = notifications::NotificationHubEntryKey(event);

    const int existing = indexOfKey(key);
    if (existing >= 0) {
        beginRemoveRows({}, existing, existing);
        entries_.remove(existing);
        endRemoveRows();
    }

    Entry entry;
    entry.key = key;
    entry.event = event;
    entry.received_at_ms = QDateTime::currentMSecsSinceEpoch();
    entry.unread = true;

    // Newest-first: a fresh or re-raised entry is always the first thing the
    // user sees when they next open the hub.
    beginInsertRows({}, 0, 0);
    entries_.prepend(entry);
    endInsertRows();
}

int NotificationEntryModel::unreadCount() const noexcept {
    int count = 0;
    for (const Entry& entry : entries_) {
        if (entry.unread)
            ++count;
    }
    return count;
}

QString NotificationEntryModel::worstUnreadTone() const {
    QString worst;
    int worst_rank = 0;
    for (const Entry& entry : entries_) {
        if (!entry.unread)
            continue;
        const QString tone = notifications::AdvisoryStatusForType(entry.event.type);
        const int rank = notifications::AdvisoryStatusRank(tone);
        if (rank > worst_rank) {
            worst_rank = rank;
            worst = tone;
        }
    }
    return worst;
}

void NotificationEntryModel::markAllRead() {
    for (int i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].unread)
            continue;
        entries_[i].unread = false;
        const QModelIndex idx = index(i);
        emit dataChanged(idx, idx, {UnreadRole});
    }
}

void NotificationEntryModel::markRead(int row) {
    if (row < 0 || row >= entries_.size() || !entries_[row].unread)
        return;
    entries_[row].unread = false;
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, {UnreadRole});
}

void NotificationEntryModel::removeAt(int row) {
    if (row < 0 || row >= entries_.size())
        return;
    beginRemoveRows({}, row, row);
    entries_.remove(row);
    endRemoveRows();
}

bool NotificationEntryModel::removeByKey(const QString& key) {
    const int row = indexOfKey(key);
    if (row < 0)
        return false;
    removeAt(row);
    return true;
}

void NotificationEntryModel::clear() {
    if (entries_.isEmpty())
        return;
    beginResetModel();
    entries_.clear();
    endResetModel();
}

const NotificationEvent& NotificationEntryModel::eventAt(int row) const {
    if (row < 0 || row >= entries_.size())
        return FallbackEvent();
    return entries_.at(row).event;
}

int NotificationEntryModel::indexOfKey(const QString& key) const {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).key == key)
            return i;
    }
    return -1;
}

QVariantList NotificationEntryModel::actionsFor(const Entry& entry) {
    QVariantList list;
    const auto append = [&list](NotificationAction action) {
        if (action == NotificationAction::None)
            return;
        QVariantMap row;
        row.insert(QStringLiteral("action"), static_cast<int>(action));
        row.insert(QStringLiteral("label"), notifications::NotificationActionLabel(action));
        list.append(row);
    };
    append(entry.event.action);
    append(entry.event.secondary_action);
    return list;
}

} // namespace exosnap::quick
