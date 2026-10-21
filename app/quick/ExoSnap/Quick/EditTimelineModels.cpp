#include "EditTimelineModels.h"

#include "EditSessionAdapter.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <set>

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

namespace {
constexpr std::size_t kMaxFolderEntries = 100000;
constexpr std::size_t kFolderBatchSize = 200;

bool IsMediaCandidate(const QFileInfo& info) {
    static const QSet<QString> extensions{QStringLiteral("mp4"),  QStringLiteral("mkv"), QStringLiteral("mov"),
                                          QStringLiteral("webm"), QStringLiteral("avi"), QStringLiteral("m4v"),
                                          QStringLiteral("mp3"),  QStringLiteral("wav"), QStringLiteral("flac"),
                                          QStringLiteral("aac"),  QStringLiteral("ogg"), QStringLiteral("m4a"),
                                          QStringLiteral("opus"), QStringLiteral("wma")};
    return info.isFile() && !info.isSymbolicLink() && !info.isJunction() &&
           extensions.contains(info.suffix().toCaseFolded());
}

struct FolderEntry {
    QString path;
    QString name;
    bool directory = false;
    bool linked = false;
    bool media = false;
};
struct FolderScan {
    std::vector<FolderEntry> entries;
    QString error;
};

FolderScan ScanFolder(const QString& path, const std::shared_ptr<std::atomic<bool>>& cancelled) {
    FolderScan result;
    const QFileInfo root(path);
    if (root.isSymbolicLink() || root.isJunction()) {
        result.error = EditFolderModel::tr("Linked directories are not expanded.");
        return result;
    }
    std::error_code error;
    std::filesystem::directory_iterator it(std::filesystem::path(path.toStdWString()), error), end;
    while (!error && it != end && !cancelled->load(std::memory_order_relaxed)) {
        if (result.entries.size() >= kMaxFolderEntries) {
            result.error =
                EditFolderModel::tr("This directory exceeds the browsing limit of %1 entries.").arg(kMaxFolderEntries);
            break;
        }
        const QFileInfo info(QString::fromStdWString(it->path().wstring()));
        const bool linked = info.isSymbolicLink() || info.isJunction();
        result.entries.push_back(
            {info.absoluteFilePath(), info.fileName(), info.isDir(), linked, IsMediaCandidate(info)});
        it.increment(error);
    }
    if (error)
        result.error =
            EditFolderModel::tr("The directory could not be read: %1").arg(QString::fromStdString(error.message()));
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& left, const auto& right) {
        const auto folded = QString::compare(left.name.toCaseFolded(), right.name.toCaseFolded(), Qt::CaseSensitive);
        return folded == 0 ? QString::compare(left.name, right.name, Qt::CaseSensitive) < 0 : folded < 0;
    });
    return result;
}
} // namespace

struct EditFolderModel::Impl {
    struct Node {
        qulonglong id = 0;
        qulonglong parent = 0;
        int depth = 0;
        FolderEntry entry;
        bool expanded = false;
        bool loading = false;
        QString error;
        std::vector<qulonglong> children;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    EditFolderModel& owner;
    QString settings_path;
    std::map<qulonglong, Node> nodes;
    std::map<QString, qulonglong> paths;
    std::vector<qulonglong> roots;
    std::vector<qulonglong> rows;
    qulonglong next_id = 0;
    std::shared_ptr<std::atomic<bool>> inspect_cancelled;
    QThreadPool pool;

    Impl(EditFolderModel& model, QString settings) : owner(model), settings_path(std::move(settings)) {
        pool.setMaxThreadCount(1);
    }
    ~Impl() {
        for (auto& [id, node] : nodes)
            if (node.cancelled)
                node.cancelled->store(true, std::memory_order_relaxed);
        if (inspect_cancelled)
            inspect_cancelled->store(true, std::memory_order_relaxed);
        pool.clear();
        pool.waitForDone();
    }
    void persist() {
        if (settings_path.isEmpty())
            return;
        QStringList directories;
        for (const auto id : roots)
            directories.push_back(nodes.at(id).entry.path);
        QSettings(settings_path, QSettings::IniFormat).setValue(QStringLiteral("folders/roots"), directories);
    }
    void appendVisible(qulonglong id, std::vector<qulonglong>& target) const {
        const auto& node = nodes.at(id);
        target.push_back(id);
        if (node.expanded)
            for (const auto child : node.children)
                appendVisible(child, target);
    }
    void publish() {
        std::vector<qulonglong> target;
        for (const auto id : roots)
            appendVisible(id, target);
        const std::set<qulonglong> wanted(target.begin(), target.end());
        for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i) {
            if (wanted.contains(rows[static_cast<std::size_t>(i)]))
                continue;
            int first = i;
            while (first > 0 && !wanted.contains(rows[static_cast<std::size_t>(first - 1)]))
                --first;
            owner.beginRemoveRows({}, first, i);
            rows.erase(rows.begin() + first, rows.begin() + i + 1);
            owner.endRemoveRows();
            i = first;
        }
        for (std::size_t i = 0; i < target.size(); ++i) {
            if (i < rows.size() && rows[i] == target[i])
                continue;
            const auto found = std::find(rows.begin() + static_cast<std::ptrdiff_t>(i), rows.end(), target[i]);
            if (found != rows.end()) {
                const auto source = static_cast<int>(found - rows.begin());
                owner.beginMoveRows({}, source, source, {}, static_cast<int>(i));
                const auto id = *found;
                rows.erase(found);
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(i), id);
                owner.endMoveRows();
            } else {
                const auto existing = std::set<qulonglong>(rows.begin() + static_cast<std::ptrdiff_t>(i), rows.end());
                std::size_t count = 1;
                while (i + count < target.size() && count < kFolderBatchSize && !existing.contains(target[i + count]))
                    ++count;
                owner.beginInsertRows({}, static_cast<int>(i), static_cast<int>(i + count - 1));
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(i),
                            target.begin() + static_cast<std::ptrdiff_t>(i),
                            target.begin() + static_cast<std::ptrdiff_t>(i + count));
                owner.endInsertRows();
                i += count - 1;
            }
        }
    }
    void changed(qulonglong id) {
        const auto found = std::find(rows.begin(), rows.end(), id);
        if (found == rows.end())
            return;
        const auto index = owner.index(static_cast<int>(found - rows.begin()));
        emit owner.dataChanged(index, index, {EditTimelineClipModel::RowRole});
    }
    void eraseNode(qulonglong id) {
        auto found = nodes.find(id);
        if (found == nodes.end())
            return;
        if (found->second.cancelled)
            found->second.cancelled->store(true, std::memory_order_relaxed);
        for (const auto child : found->second.children)
            eraseNode(child);
        paths.erase(found->second.entry.path.toCaseFolded());
        nodes.erase(found);
    }
    void cancelBranch(qulonglong id) {
        auto& node = nodes.at(id);
        if (node.cancelled)
            node.cancelled->store(true, std::memory_order_relaxed);
        node.loading = false;
        node.expanded = false;
        for (const auto child : node.children)
            cancelBranch(child);
    }
    void apply(qulonglong id, std::shared_ptr<FolderScan> scan, std::shared_ptr<std::atomic<bool>> token,
               std::shared_ptr<std::vector<qulonglong>> accepted, std::size_t offset) {
        const auto found = nodes.find(id);
        if (found == nodes.end() || found->second.cancelled != token || token->load(std::memory_order_relaxed))
            return;
        auto& parent = found->second;
        const auto end = std::min(offset + kFolderBatchSize, scan->entries.size());
        bool limited = false;
        for (auto i = offset; i < end; ++i) {
            const auto& entry = scan->entries[i];
            const auto key = entry.path.toCaseFolded();
            auto existing = paths.find(key);
            qulonglong child = existing == paths.end() ? 0 : existing->second;
            if (!child) {
                if (nodes.size() >= kMaxFolderEntries) {
                    scan->error = EditFolderModel::tr(
                        "The folder browser has reached its entry limit. Remove unused roots and refresh.");
                    limited = true;
                    break;
                }
                child = ++next_id;
                Node node;
                node.id = child;
                node.parent = id;
                node.depth = parent.depth + 1;
                node.entry = entry;
                if (entry.linked)
                    node.error = EditFolderModel::tr("Linked entries are not followed.");
                nodes.emplace(child, std::move(node));
                paths.emplace(key, child);
            } else {
                auto& node = nodes.at(child);
                if (node.entry.directory && (!entry.directory || entry.linked)) {
                    for (const auto descendant : node.children)
                        eraseNode(descendant);
                    node.children.clear();
                    node.expanded = false;
                    if (node.cancelled)
                        node.cancelled->store(true, std::memory_order_relaxed);
                }
                node.entry = entry;
                node.error = entry.linked ? EditFolderModel::tr("Linked entries are not followed.") : QString();
                changed(child);
            }
            accepted->push_back(child);
        }
        auto children = *accepted;
        const std::set<qulonglong> seen(children.begin(), children.end());
        const bool finished = end == scan->entries.size() || limited;
        if (!finished) {
            for (const auto old : parent.children)
                if (!seen.contains(old))
                    children.push_back(old);
        } else {
            for (const auto old : parent.children)
                if (!seen.contains(old))
                    eraseNode(old);
            parent.loading = false;
            parent.error = scan->error;
        }
        parent.children = std::move(children);
        publish();
        changed(id);
        if (!finished) {
            QPointer<EditFolderModel> model(&owner);
            QTimer::singleShot(16, &owner, [model, id, scan, token, accepted, end] {
                if (model)
                    model->impl_->apply(id, scan, token, accepted, end);
            });
        }
    }
    void scan(qulonglong id) {
        const auto found = nodes.find(id);
        if (found == nodes.end() || !found->second.entry.directory)
            return;
        auto& node = found->second;
        if (node.cancelled)
            node.cancelled->store(true, std::memory_order_relaxed);
        node.cancelled = std::make_shared<std::atomic<bool>>(false);
        node.loading = true;
        node.error.clear();
        changed(id);
        const auto token = node.cancelled;
        const auto path = node.entry.path;
        QPointer<EditFolderModel> model(&owner);
        pool.start([model, token, path, id] {
            auto result = std::make_shared<FolderScan>(ScanFolder(path, token));
            if (!model || token->load(std::memory_order_relaxed))
                return;
            QMetaObject::invokeMethod(
                model,
                [model, token, result, id] {
                    if (model)
                        model->impl_->apply(id, result, token, std::make_shared<std::vector<qulonglong>>(), 0);
                },
                Qt::QueuedConnection);
        });
    }
};

EditFolderModel::EditFolderModel(QString settings_path, QObject* parent)
    : QAbstractListModel(parent), impl_(std::make_unique<Impl>(*this, std::move(settings_path))) {
    if (!impl_->settings_path.isEmpty()) {
        const auto roots =
            QSettings(impl_->settings_path, QSettings::IniFormat).value(QStringLiteral("folders/roots")).toStringList();
        for (const auto& path : roots)
            addRoot(QUrl::fromLocalFile(path));
    }
}
EditFolderModel::~EditFolderModel() = default;
int EditFolderModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(impl_->rows.size());
}
QHash<int, QByteArray> EditFolderModel::roleNames() const {
    return {{EditTimelineClipModel::RowRole, QByteArrayLiteral("row")}};
}
QVariant EditFolderModel::data(const QModelIndex& index, int role) const {
    if (role != EditTimelineClipModel::RowRole || !index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto found = impl_->nodes.find(impl_->rows[static_cast<std::size_t>(index.row())]);
    if (found == impl_->nodes.end())
        return {};
    const auto& node = found->second;
    return QVariantMap{{QStringLiteral("id"), node.id},
                       {QStringLiteral("entryId"), node.id},
                       {QStringLiteral("name"), node.entry.name},
                       {QStringLiteral("url"), QUrl::fromLocalFile(node.entry.path)},
                       {QStringLiteral("depth"), node.depth},
                       {QStringLiteral("directory"), node.entry.directory},
                       {QStringLiteral("expanded"), node.expanded},
                       {QStringLiteral("loading"), node.loading},
                       {QStringLiteral("error"), node.error},
                       {QStringLiteral("mediaCandidate"), node.entry.media},
                       {QStringLiteral("root"), node.parent == 0}};
}
void EditFolderModel::addRoot(const QUrl& directory) {
    if (!directory.isLocalFile() || impl_->roots.size() >= 64)
        return;
    const QFileInfo info(QDir::cleanPath(QFileInfo(directory.toLocalFile()).absoluteFilePath()));
    const auto key = info.absoluteFilePath().toCaseFolded();
    if (impl_->paths.contains(key))
        return;
    const auto prior_roots = impl_->roots;
    for (const auto id : prior_roots) {
        const auto prior = impl_->nodes.at(id).entry.path.toCaseFolded();
        if (key.startsWith(prior + QLatin1Char('/')))
            return;
        if (prior.startsWith(key + QLatin1Char('/')))
            removeRoot(id);
    }
    Impl::Node node;
    node.id = ++impl_->next_id;
    node.entry = {info.absoluteFilePath(), info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName(), true,
                  info.isSymbolicLink() || info.isJunction(), false};
    const auto id = node.id;
    impl_->nodes.emplace(id, std::move(node));
    impl_->paths.emplace(key, id);
    impl_->roots.push_back(id);
    impl_->publish();
    impl_->persist();
}
void EditFolderModel::toggle(qulonglong entry_id) {
    const auto found = impl_->nodes.find(entry_id);
    if (found == impl_->nodes.end() || !found->second.entry.directory)
        return;
    auto& node = found->second;
    node.expanded = !node.expanded;
    if (!node.expanded)
        impl_->cancelBranch(entry_id);
    impl_->publish();
    impl_->changed(entry_id);
    if (node.expanded)
        impl_->scan(entry_id);
}
void EditFolderModel::refresh(qulonglong entry_id) {
    const auto found = impl_->nodes.find(entry_id);
    if (found != impl_->nodes.end() && found->second.expanded)
        impl_->scan(entry_id);
}
void EditFolderModel::removeRoot(qulonglong entry_id) {
    const auto found = std::find(impl_->roots.begin(), impl_->roots.end(), entry_id);
    if (found == impl_->roots.end())
        return;
    impl_->roots.erase(found);
    impl_->eraseNode(entry_id);
    impl_->publish();
    impl_->persist();
}
void EditFolderModel::inspectDirectory(const QUrl& directory) {
    if (impl_->inspect_cancelled)
        impl_->inspect_cancelled->store(true, std::memory_order_relaxed);
    impl_->inspect_cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto token = impl_->inspect_cancelled;
    QPointer<EditFolderModel> model(this);
    impl_->pool.start([model, directory, token] {
        auto scan = ScanFolder(directory.toLocalFile(), token);
        QVariantList files;
        if (scan.error.isEmpty() && directory.isLocalFile())
            for (const auto& entry : scan.entries)
                if (entry.media)
                    files.push_back(QUrl::fromLocalFile(entry.path));
        if (!directory.isLocalFile())
            scan.error = EditFolderModel::tr("Only local directories are supported.");
        if (!model || token->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(
            model,
            [model, directory, token, files = std::move(files), error = std::move(scan.error)] {
                if (model && !token->load(std::memory_order_relaxed))
                    emit model->candidatesReady(directory, files, error);
            },
            Qt::QueuedConnection);
    });
}

QList<QUrl> EditFolderModel::DirectoryCandidates(const QUrl& directory,
                                                 const std::shared_ptr<std::atomic<bool>>& cancelled, QString& reason) {
    auto scan = ScanFolder(directory.toLocalFile(), cancelled);
    reason = scan.error;
    QList<QUrl> candidates;
    if (reason.isEmpty() && directory.isLocalFile())
        for (const auto& entry : scan.entries)
            if (entry.media)
                candidates.push_back(QUrl::fromLocalFile(entry.path));
    if (!directory.isLocalFile())
        reason = EditFolderModel::tr("Only local directories are supported.");
    return candidates;
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

void EditTimelineClipModel::syncUs(EditSessionAdapter* session, qint64 fromUs, qint64 toUs) {
    ++sync_count_;
    setRows(session != nullptr ? session->visibleClipsUs(fromUs, toUs) : QVariantList{});
}

void EditTimelineClipModel::syncViewportUs(EditSessionAdapter* session, qint64 fromUs, qint64 toUs,
                                           const QVariantList& visibleTrackIds, const QVariantList& retainedClipIds) {
    ++sync_count_;
    setRows(session != nullptr ? session->visibleClipsOnTracksUs(fromUs, toUs, visibleTrackIds, retainedClipIds)
                               : QVariantList{});
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
