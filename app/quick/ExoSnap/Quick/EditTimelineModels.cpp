#include "EditTimelineModels.h"

#include "EditSessionAdapter.h"

#include <QMutexLocker>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstddef>

namespace exosnap::quick {

EditTimelineTileModel::EditTimelineTileModel(QObject* parent) : QAbstractListModel(parent) {
}

int EditTimelineTileModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tiles_.size());
}

QVariant EditTimelineTileModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(tiles_.size()))
        return {};
    const Tile& tile = tiles_.at(static_cast<size_t>(index.row()));
    switch (role) {
    case SourceRole:
        return QStringLiteral("image://%1/%2/%3").arg(QLatin1String(kEditTileProviderId)).arg(run_id_).arg(index.row());
    case TimeMsRole:
        return QVariant::fromValue(tile.time_ms);
    default:
        return {};
    }
}

QHash<int, QByteArray> EditTimelineTileModel::roleNames() const {
    return {
        {SourceRole, QByteArrayLiteral("tileSource")},
        {TimeMsRole, QByteArrayLiteral("timeMs")},
    };
}

void EditTimelineTileModel::beginRun(quint64 run_id) {
    beginResetModel();
    tiles_.clear();
    run_id_ = run_id;
    endResetModel();
}

void EditTimelineTileModel::appendTile(qint64 time_ms) {
    const int row = static_cast<int>(tiles_.size());
    beginInsertRows({}, row, row);
    tiles_.push_back(Tile{time_ms});
    endInsertRows();
}

void EditTimelineTileModel::clear() {
    if (tiles_.empty())
        return;
    beginResetModel();
    tiles_.clear();
    endResetModel();
}

quint64 EditTimelineTileModel::runId() const noexcept {
    return run_id_;
}

std::vector<RecordingMarker> VisibleTimelineMarkers(const std::vector<RecordingMarker>& markers, qint64 duration_ms,
                                                    int track_width) {
    std::vector<RecordingMarker> visible;
    if (duration_ms <= 0 || track_width <= 0 || markers.empty())
        return visible;

    std::vector<RecordingMarker> sorted = markers;
    std::sort(sorted.begin(), sorted.end(),
              [](const RecordingMarker& lhs, const RecordingMarker& rhs) { return lhs.time_ms < rhs.time_ms; });

    int last_column = -1;
    visible.reserve(std::min<size_t>(sorted.size(), kMaxRenderedMarkers));
    for (const auto& marker : sorted) {
        const auto time_ms = static_cast<qint64>(marker.time_ms);
        if (time_ms < 0 || time_ms > duration_ms)
            continue;
        const int column = static_cast<int>(time_ms * track_width / duration_ms);
        if (column == last_column)
            continue;
        last_column = column;
        visible.push_back(marker);
        if (visible.size() >= static_cast<size_t>(kMaxRenderedMarkers))
            break;
    }
    return visible;
}

EditTimelineClipModel::EditTimelineClipModel(QObject* parent) : QAbstractListModel(parent) {
}

int EditTimelineClipModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant EditTimelineClipModel::data(const QModelIndex& index, int role) const {
    if (role != RowRole || !index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(rows_.size()))
        return {};
    return rows_[static_cast<std::size_t>(index.row())].second;
}

QHash<int, QByteArray> EditTimelineClipModel::roleNames() const {
    return {{RowRole, QByteArrayLiteral("row")}};
}

void EditTimelineClipModel::sync(EditSessionAdapter* session, qint64 fromMs, qint64 toMs) {
    setRows(session != nullptr ? session->visibleClips(fromMs, toMs) : QVariantList{});
}

void EditTimelineClipModel::setRows(const QVariantList& rows) {
    std::vector<std::pair<qulonglong, QVariantMap>> target;
    target.reserve(static_cast<std::size_t>(rows.size()));
    QSet<qulonglong> wanted;
    for (const auto& value : rows) {
        auto row = value.toMap();
        const auto id = row.value(QStringLiteral("id")).toULongLong();
        wanted.insert(id);
        target.emplace_back(id, std::move(row));
    }
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (wanted.contains(rows_[static_cast<std::size_t>(row)].first))
            continue;
        int first = row;
        while (first > 0 && !wanted.contains(rows_[static_cast<std::size_t>(first - 1)].first))
            --first;
        beginRemoveRows({}, first, row);
        rows_.erase(rows_.begin() + first, rows_.begin() + row + 1);
        endRemoveRows();
        row = first;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        auto& [id, row] = target[i];
        if (i >= rows_.size() || rows_[i].first != id) {
            const auto found = std::find_if(rows_.begin() + static_cast<std::ptrdiff_t>(i), rows_.end(),
                                            [id = id](const auto& entry) { return entry.first == id; });
            const int destination = static_cast<int>(i);
            if (found == rows_.end()) {
                beginInsertRows({}, destination, destination);
                rows_.emplace(rows_.begin() + destination, id, std::move(row));
                endInsertRows();
                continue;
            }
            const int source = static_cast<int>(found - rows_.begin());
            beginMoveRows({}, source, source, {}, destination);
            auto moved = std::move(*found);
            rows_.erase(found);
            rows_.emplace(rows_.begin() + destination, std::move(moved));
            endMoveRows();
        }
        if (rows_[i].second != row) {
            rows_[i].second = std::move(row);
            const auto changed = index(static_cast<int>(i));
            emit dataChanged(changed, changed, {RowRole});
        }
    }
}

EditMarkerModel::EditMarkerModel(QObject* parent) : QAbstractListModel(parent) {
}

int EditMarkerModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(visible_.size());
}

QVariant EditMarkerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(visible_.size()))
        return {};
    const RecordingMarker& marker = visible_.at(static_cast<size_t>(index.row()));
    switch (role) {
    case TimeMsRole:
        return QVariant::fromValue(static_cast<qint64>(marker.time_ms));
    case LabelRole:
        return marker.label.empty() ? QString::fromLatin1(RecordingMarkerTypeDefaultLabel(marker.type))
                                    : QString::fromStdString(marker.label);
    case TypeRole:
        return QString::fromLatin1(RecordingMarkerTypeToString(marker.type));
    default:
        return {};
    }
}

QHash<int, QByteArray> EditMarkerModel::roleNames() const {
    return {
        {TimeMsRole, QByteArrayLiteral("timeMs")},
        {LabelRole, QByteArrayLiteral("label")},
        {TypeRole, QByteArrayLiteral("markerType")},
    };
}

void EditMarkerModel::setMarkers(const std::vector<RecordingMarker>& markers, qint64 duration_ms, int track_width) {
    std::vector<RecordingMarker> next = VisibleTimelineMarkers(markers, duration_ms, track_width);
    if (next == visible_)
        return;
    beginResetModel();
    visible_ = std::move(next);
    endResetModel();
}

EditTimelineTileProvider::EditTimelineTileProvider() : QQuickImageProvider(QQuickImageProvider::Image) {
}

QImage EditTimelineTileProvider::requestImage(const QString& id, QSize* size, const QSize& requested_size) {
    Q_UNUSED(requested_size);
    const QStringList parts = id.split(QLatin1Char('/'));
    if (parts.size() != 2)
        return {};
    bool run_ok = false;
    bool index_ok = false;
    const quint64 run_id = parts.at(0).toULongLong(&run_ok);
    const int index = parts.at(1).toInt(&index_ok);
    if (!run_ok || !index_ok)
        return {};

    QMutexLocker lock(&mutex_);
    if (run_id != run_id_)
        return {};
    const auto it = tiles_.constFind(index);
    if (it == tiles_.constEnd())
        return {};
    if (size != nullptr)
        *size = it->size();
    return *it;
}

void EditTimelineTileProvider::submitTile(quint64 run_id, int index, QImage image) {
    QMutexLocker lock(&mutex_);
    if (run_id != run_id_) {
        if (run_id < run_id_)
            return;
        run_id_ = run_id;
        tiles_.clear();
    }
    tiles_.insert(index, std::move(image));
}

void EditTimelineTileProvider::clear() {
    QMutexLocker lock(&mutex_);
    tiles_.clear();
}

} // namespace exosnap::quick
