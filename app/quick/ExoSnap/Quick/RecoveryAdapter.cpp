#include "RecoveryAdapter.h"

#include "diagnostics/AppLog.h"

#include <QMetaObject>
#include <QRunnable>

#include <utility>

namespace exosnap::quick {
namespace {

QString FormatSize(qint64 bytes) {
    if (bytes <= 0)
        return QStringLiteral("unknown size");
    if (bytes < 1024LL * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024LL * 1024));
    return QStringLiteral("%1 GB").arg(bytes / (1024LL * 1024 * 1024));
}

QString DisplayName(const RecoveryManifestEntry& entry) {
    const QString& source = entry.final_output_path.isEmpty() ? entry.artefact_path : entry.final_output_path;
    return source.section(QLatin1Char('/'), -1).section(QLatin1Char('\\'), -1);
}

} // namespace

// ---------------------------------------------------------------------------
// RecoveryCandidateModel
// ---------------------------------------------------------------------------

RecoveryCandidateModel::RecoveryCandidateModel(QObject* parent) : QAbstractListModel(parent) {
}

int RecoveryCandidateModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(candidates_.size());
}

QVariant RecoveryCandidateModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= candidates_.size())
        return {};
    const RecoveryCandidate& candidate = candidates_.at(index.row());
    const RowState& state = states_.at(index.row());
    switch (role) {
    case DisplayNameRole:
        return DisplayName(candidate.entry);
    case MetaRole:
        return QStringLiteral("%1 \xc2\xb7 %2 \xc2\xb7 %3")
            .arg(FormatSize(candidate.artefact_size_bytes), candidate.entry.started_at.left(10),
                 candidate.entry.intended_container.toUpper());
    case CanContinueRole:
        return !candidate.entry.finalized;
    case StatusRole:
        return state.status;
    case StatusIsErrorRole:
        return state.status_is_error;
    case BusyRole:
        return state.busy;
    case ProgressRole:
        return state.progress;
    case ArmedDiscardRole:
        return state.armed_discard;
    default:
        return {};
    }
}

QHash<int, QByteArray> RecoveryCandidateModel::roleNames() const {
    return {
        {DisplayNameRole, "displayName"},     {MetaRole, "meta"},
        {CanContinueRole, "canContinue"},     {StatusRole, "status"},
        {StatusIsErrorRole, "statusIsError"}, {BusyRole, "busy"},
        {ProgressRole, "progress"},           {ArmedDiscardRole, "armedDiscard"},
    };
}

void RecoveryCandidateModel::setCandidates(QVector<RecoveryCandidate> candidates) {
    beginResetModel();
    candidates_ = std::move(candidates);
    states_.assign(candidates_.size(), RowState{});
    endResetModel();
}

void RecoveryCandidateModel::removeAt(int index) {
    if (!contains(index))
        return;
    beginRemoveRows({}, index, index);
    candidates_.remove(index);
    states_.remove(index);
    endRemoveRows();
}

const QVector<RecoveryCandidate>& RecoveryCandidateModel::candidates() const noexcept {
    return candidates_;
}

bool RecoveryCandidateModel::contains(int index) const noexcept {
    return index >= 0 && index < candidates_.size();
}

void RecoveryCandidateModel::emitRowChanged(int index, const QList<int>& roles) {
    const QModelIndex model_index = this->index(index, 0);
    emit dataChanged(model_index, model_index, roles);
}

void RecoveryCandidateModel::setStatus(int index, const QString& text, bool is_error) {
    if (!contains(index))
        return;
    states_[index].status = text;
    states_[index].status_is_error = is_error;
    emitRowChanged(index, {StatusRole, StatusIsErrorRole});
}

void RecoveryCandidateModel::setBusy(int index, bool busy) {
    if (!contains(index))
        return;
    states_[index].busy = busy;
    if (!busy)
        states_[index].progress = 0.0;
    emitRowChanged(index, {BusyRole, ProgressRole});
}

void RecoveryCandidateModel::setProgress(int index, double progress) {
    if (!contains(index))
        return;
    states_[index].progress = progress;
    emitRowChanged(index, {ProgressRole});
}

void RecoveryCandidateModel::setArmedDiscard(int index, bool armed) {
    if (!contains(index))
        return;
    states_[index].armed_discard = armed;
    emitRowChanged(index, {ArmedDiscardRole});
}

void RecoveryCandidateModel::clearArmedDiscard() {
    for (int i = 0; i < states_.size(); ++i) {
        if (states_.at(i).armed_discard)
            setArmedDiscard(i, false);
    }
}

// ---------------------------------------------------------------------------
// RecoveryAdapter
// ---------------------------------------------------------------------------

RecoveryAdapter::RecoveryAdapter(QObject* parent) : QObject(parent) {
    // One repair remux at a time. The cap is the policy, not a tuning knob:
    // two concurrent remuxes write into the same output folder.
    finish_pool_.setMaxThreadCount(1);
}

RecoveryAdapter::~RecoveryAdapter() {
    // Ask a running Finish to stop at its next progress callback, then let
    // finish_pool_'s destructor join it. Without the flag the join would wait
    // for a whole remux to complete on the way out.
    cancel_requested_.store(true);
}

void RecoveryAdapter::setService(RecoveryService* service) {
    service_ = service;
}

QAbstractItemModel* RecoveryAdapter::model() noexcept {
    return &model_;
}

bool RecoveryAdapter::hasCandidates() const noexcept {
    return !model_.candidates().isEmpty();
}

int RecoveryAdapter::candidateCount() const noexcept {
    return static_cast<int>(model_.candidates().size());
}

bool RecoveryAdapter::surfaceOpen() const noexcept {
    return surface_open_;
}

bool RecoveryAdapter::busy() const noexcept {
    return busy_index_ >= 0;
}

int RecoveryAdapter::scan() {
    if (service_ == nullptr)
        return 0;
    model_.setCandidates(service_->Scan());
    armed_discard_index_ = -1;
    emit candidatesChanged();
    return candidateCount();
}

void RecoveryAdapter::seedCandidatesForVisualHarness(QVector<RecoveryCandidate> candidates) {
    model_.setCandidates(std::move(candidates));
    armed_discard_index_ = -1;
    emit candidatesChanged();
}

void RecoveryAdapter::openSurface() {
    if (!hasCandidates())
        return;
    setSurfaceOpen(true);
}

void RecoveryAdapter::dismiss() {
    // A running Finish keeps the surface up: lowering it would hide a progress
    // bar the user is waiting on, and there is no other place that reports it.
    if (busy())
        return;
    disarmDiscard();
    setSurfaceOpen(false);
}

void RecoveryAdapter::finish(int index) {
    if (service_ == nullptr || busy() || !model_.contains(index))
        return;
    disarmDiscard();

    const RecoveryManifestEntry entry = model_.candidates().at(index).entry;
    setBusyIndex(index);
    model_.setBusy(index, true);
    model_.setStatus(index, {}, false);
    cancel_requested_.store(false);

    RecoveryService* service = service_;
    // Queued back onto this object's thread: both callbacks touch the model.
    finish_pool_.start([this, service, entry, index]() {
        const RecoveryActionResult result = service->Finish(entry, [this, index](float progress) -> bool {
            QMetaObject::invokeMethod(
                this,
                [this, index, progress]() {
                    model_.setProgress(index, static_cast<double>(progress));
                    emit actionProgressChanged(static_cast<double>(progress));
                },
                Qt::QueuedConnection);
            return !cancel_requested_.load();
        });
        QMetaObject::invokeMethod(
            this,
            [this, index, success = result.success, message = QString::fromStdString(result.message)]() {
                onFinishComplete(index, success, message);
            },
            Qt::QueuedConnection);
    });
}

void RecoveryAdapter::onFinishComplete(int index, bool success, const QString& message) {
    model_.setBusy(index, false);
    setBusyIndex(-1);
    emit actionFinished(success, cancel_requested_.load());

    if (success) {
        diagnostics::AppLog::info(QStringLiteral("recovery"), QStringLiteral("Recovery finish succeeded"));
        model_.removeAt(index);
        emit candidatesChanged();
        if (!hasCandidates())
            setSurfaceOpen(false);
        return;
    }

    const QString text = message.isEmpty()
                             ? (cancel_requested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Failed"))
                             : message;
    // Cancel is a user decision, not a failure — it must not be tinted as one.
    model_.setStatus(index, text, !cancel_requested_.load());
    diagnostics::AppLog::warning(QStringLiteral("recovery"),
                                 QStringLiteral("Recovery finish did not complete: %1").arg(text));
}

void RecoveryAdapter::armDiscard(int index) {
    if (busy() || !model_.contains(index))
        return;
    model_.clearArmedDiscard();
    armed_discard_index_ = index;
    model_.setArmedDiscard(index, true);
}

void RecoveryAdapter::disarmDiscard() {
    if (armed_discard_index_ < 0)
        return;
    armed_discard_index_ = -1;
    model_.clearArmedDiscard();
}

void RecoveryAdapter::discard(int index) {
    if (service_ == nullptr || busy() || !model_.contains(index))
        return;
    // The confirm is a safety property of the action, so it is enforced where
    // the action lives. A delegate that lost track of its armed state cannot
    // delete a recording by calling straight through.
    if (armed_discard_index_ != index)
        return;

    const RecoveryManifestEntry entry = model_.candidates().at(index).entry;
    const RecoveryActionResult result = service_->Discard(entry);
    disarmDiscard();

    if (!result.success) {
        model_.setStatus(index, QString::fromStdString(result.message), /*is_error=*/true);
        diagnostics::AppLog::warning(
            QStringLiteral("recovery"),
            QStringLiteral("Recovery discard failed: %1").arg(QString::fromStdString(result.message)));
        return;
    }

    diagnostics::AppLog::info(QStringLiteral("recovery"), QStringLiteral("Recovery candidate discarded"));
    model_.removeAt(index);
    emit candidatesChanged();
    if (!hasCandidates())
        setSurfaceOpen(false);
}

void RecoveryAdapter::continueSession(int index) {
    if (busy() || !model_.contains(index))
        return;
    const RecoveryCandidate& candidate = model_.candidates().at(index);
    if (candidate.entry.finalized)
        return;

    const RecoveryManifestEntry entry = candidate.entry;
    disarmDiscard();
    // The surface closes on the request, not on its completion: arming the
    // coordinator lands on the Record page, and leaving a scrim over it would
    // hide the very session the user just resumed.
    setSurfaceOpen(false);
    emit continueRequested(entry);
}

void RecoveryAdapter::cancelAction() {
    if (!busy())
        return;
    cancel_requested_.store(true);
}

void RecoveryAdapter::setSurfaceOpen(bool open) {
    if (surface_open_ == open)
        return;
    surface_open_ = open;
    emit surfaceOpenChanged();
}

void RecoveryAdapter::setBusyIndex(int index) {
    const bool was_busy = busy();
    busy_index_ = index;
    if (was_busy != busy())
        emit busyChanged();
}

} // namespace exosnap::quick
