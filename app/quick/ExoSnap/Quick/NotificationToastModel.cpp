#include "NotificationToastModel.h"

#include "NotificationHubPolicy.h"
#include "notifications/NotificationManager.h"

#include <QDateTime>

namespace exosnap::quick {
namespace {

// 100 ms is fine for a bar that runs for five to eight seconds: the hairline
// moves at most a few pixels per tick at the widest toast, and a faster timer
// would wake the GUI thread for nothing.
constexpr int kCountdownIntervalMs = 100;

} // namespace

NotificationToastModel::NotificationToastModel(QObject* parent) : QAbstractListModel(parent) {
    countdown_.setInterval(kCountdownIntervalMs);
    // Qt's default timer type allows up to 5 % drift and, on Windows, snaps to
    // the ~15.6 ms system tick -- at a 100 ms interval that lands the updates
    // unevenly, and a bar moving in uneven steps reads as stuttering however
    // small the steps are. The countdown overlay's timer is precise for the
    // same reason.
    countdown_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&countdown_, &QTimer::timeout, this, [this]() { updateCountdown(); });
}

void NotificationToastModel::attach(notifications::NotificationManager* manager) {
    manager_ = manager;
    if (manager_ == nullptr)
        return;
    QObject::connect(manager_, &notifications::NotificationManager::visibleSetChanged, this, [this]() { refresh(); });
    refresh();
}

int NotificationToastModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

QVariant NotificationToastModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= visible_.size())
        return {};
    const notifications::NotificationEvent& event = visible_.at(index.row());

    switch (role) {
    case SequenceRole:
        return static_cast<qint64>(event.sequence);
    case TitleRole:
        return event.title;
    case BodyRole:
        return event.body;
    case ToneRole:
        return notifications::AdvisoryStatusForType(event.type);
    case StandingRole:
        return notifications::NotificationManager::IsStanding(event.type);
    case RemainingFractionRole: {
        const int interval = notifications::NotificationManager::DismissIntervalMs(event.type);
        if (interval <= 0 || manager_ == nullptr)
            return 0.0;
        const qint64 shown_at = manager_->ShownAtMs(event.sequence);
        if (shown_at < 0)
            return 0.0;
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - shown_at;
        const double remaining = 1.0 - static_cast<double>(elapsed) / static_cast<double>(interval);
        return std::clamp(remaining, 0.0, 1.0);
    }
    case ActionCountRole: {
        int count = 0;
        if (event.action != notifications::NotificationAction::None)
            ++count;
        if (event.secondary_action != notifications::NotificationAction::None)
            ++count;
        return count;
    }
    case PrimaryLabelRole:
        return notifications::NotificationActionLabel(event.action);
    case PrimaryActionRole:
        return static_cast<int>(event.action);
    case SecondaryLabelRole:
        return notifications::NotificationActionLabel(event.secondary_action);
    case SecondaryActionRole:
        return static_cast<int>(event.secondary_action);
    default:
        return {};
    }
}

QHash<int, QByteArray> NotificationToastModel::roleNames() const {
    return {
        {SequenceRole, "sequence"},
        {TitleRole, "title"},
        {BodyRole, "body"},
        {ToneRole, "tone"},
        {StandingRole, "standing"},
        {RemainingFractionRole, "remainingFraction"},
        {ActionCountRole, "actionCount"},
        {PrimaryLabelRole, "primaryLabel"},
        {PrimaryActionRole, "primaryAction"},
        {SecondaryLabelRole, "secondaryLabel"},
        {SecondaryActionRole, "secondaryAction"},
    };
}

const notifications::NotificationEvent* NotificationToastModel::eventForSequence(quint64 sequence) const {
    for (const notifications::NotificationEvent& event : visible_) {
        if (event.sequence == sequence)
            return &event;
    }
    return nullptr;
}

void NotificationToastModel::refresh() {
    // A full reset rather than a diff: the visible set is at most a handful of
    // rows and the manager reorders it (the timed toast is always last), so a
    // row-wise diff would be more code for no measurable gain.
    beginResetModel();
    visible_ = manager_ != nullptr ? manager_->VisibleEvents() : QVector<notifications::NotificationEvent>{};
    endResetModel();

    const bool has_timed =
        std::any_of(visible_.cbegin(), visible_.cend(), [](const notifications::NotificationEvent& event) {
            return !notifications::NotificationManager::IsStanding(event.type);
        });
    if (has_timed && !countdown_.isActive())
        countdown_.start();
    else if (!has_timed && countdown_.isActive())
        countdown_.stop();
}

void NotificationToastModel::updateCountdown() {
    if (visible_.isEmpty())
        return;
    emit dataChanged(index(0, 0), index(static_cast<int>(visible_.size()) - 1, 0), {RemainingFractionRole});
}

} // namespace exosnap::quick
