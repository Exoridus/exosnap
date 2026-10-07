#include "EditSessionAdapter.h"

#include "models/EditWorkspace.h"

#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QtCore/Qt>
#include <QtCore/qcontainerfwd.h>
#include <QtCore/qtmetamacros.h>
#include <QtCore/qtypes.h>

#include <QPointer>
#include <QVariantMap>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include <exosnap/engine/edit_timeline_export.h>

namespace exosnap::quick {

QVariantList EditSessionAdapter::media() const {
    QVariantList rows;
    for (const auto& asset : workspace_.assets()) {
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(asset.id)},
                        {QStringLiteral("name"), QString::fromStdString(asset.name)},
                        {QStringLiteral("path"), QString::fromStdWString(asset.path)},
                        {QStringLiteral("durationMs"), QVariant::fromValue<qint64>(asset.duration / 1000)},
                        {QStringLiteral("available"), asset.state == edit::AssetState::Available},
                        {QStringLiteral("metadata"),
                         QStringLiteral("%1 x %2 / %3 fps").arg(asset.width).arg(asset.height).arg(asset.fps)}});
    }
    return rows;
}

QVariantList EditSessionAdapter::tracks() const {
    QVariantList rows;
    int video = 0;
    int audio = 0;
    for (const auto& track : workspace_.tracks()) {
        const bool is_video = track.type == edit::TrackType::Video;
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(track.id)},
                        {QStringLiteral("name"), QStringLiteral("%1%2")
                                                     .arg(is_video ? QStringLiteral("V") : QStringLiteral("A"))
                                                     .arg(is_video ? ++video : ++audio)}});
    }
    return rows;
}

QVariantList EditSessionAdapter::visibleClips(qint64 from_ms, qint64 to_ms) const {
    QVariantList rows;
    const auto* selected = workspace_.clip(workspace_.selection());
    for (const auto& clip : workspace_.clips()) {
        if (clip.end() < from_ms * 1000 || clip.start > to_ms * 1000)
            continue;
        const auto* asset = workspace_.asset(clip.asset);
        const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                        [&clip](const auto& t) { return t.id == clip.track; });
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(clip.id)},
                        {QStringLiteral("name"), QString::fromStdString(asset->name)},
                        {QStringLiteral("trackIndex"), static_cast<int>(track - workspace_.tracks().begin())},
                        {QStringLiteral("video"), track->type == edit::TrackType::Video},
                        {QStringLiteral("path"), QString::fromStdWString(asset->path)},
                        {QStringLiteral("group"), QVariant::fromValue<qulonglong>(clip.group)},
                        {QStringLiteral("startMs"), QVariant::fromValue<qint64>(clip.start / 1000)},
                        {QStringLiteral("durationMs"), QVariant::fromValue<qint64>(clip.duration() / 1000)},
                        {QStringLiteral("inMs"), QVariant::fromValue<qint64>(clip.source_in / 1000)},
                        {QStringLiteral("outMs"), QVariant::fromValue<qint64>(clip.source_out / 1000)},
                        {QStringLiteral("available"), asset->state == edit::AssetState::Available},
                        {QStringLiteral("selected"),
                         selected && (clip.id == selected->id || (clip.group != 0 && clip.group == selected->group))}});
    }
    return rows;
}

void EditSessionAdapter::publishWorkspace(bool changed) {
    workspace_error_ = changed ? QString() : tr("The edit would overlap another clip or exceed the source.");
    duration_ms_ = workspace_.duration() / 1000;
    position_ms_ = workspace_.playhead() / 1000;
    const bool open = !workspace_.clips().empty();
    if (open_ != open) {
        open_ = open;
        emit openChanged();
    }
    emit workspaceChanged();
    emit durationChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
}

void EditSessionAdapter::selectClip(qulonglong id) {
    workspace_.select(id);
    emit workspaceChanged();
}

void EditSessionAdapter::appendAsset(qulonglong id, qint64 at_ms) {
    publishWorkspace(workspace_.insert(id, at_ms < 0 ? -1 : at_ms * 1000));
}

void EditSessionAdapter::addHistory(const QString& path, qint64 at_ms) {
    emit historyRequested(path, at_ms);
}

void EditSessionAdapter::importMedia(const QUrl& url, bool append, qint64 at_ms) {
    importMediaBatch({url}, append, at_ms);
}

void EditSessionAdapter::importMediaBatch(const QList<QUrl>& urls, bool append, qint64 at_ms) {
    QPointer<EditSessionAdapter> self(this);
    keyframe_pool_.start([self, urls, append, at_ms]() {
        std::vector<edit::Asset> assets;
        QString error;
        for (const auto& url : urls) {
            if (!url.isLocalFile())
                continue;
            const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
            const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
            if (!metadata.error.empty() || metadata.duration_us <= 0) {
                error = QString::fromStdString(metadata.error);
                break;
            }
            edit::Asset asset;
            asset.path = path.toStdWString();
            asset.name = QFileInfo(path).fileName().toStdString();
            asset.duration = metadata.duration_us;
            asset.width = metadata.width;
            asset.height = metadata.height;
            asset.fps = metadata.fps;
            asset.audio = metadata.has_audio;
            assets.push_back(std::move(asset));
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, assets = std::move(assets), error, append, at_ms]() mutable {
                if (!self)
                    return;
                if (!error.isEmpty()) {
                    self->workspace_error_ = EditSessionAdapter::tr("Cannot import media: %1").arg(error);
                    emit self->workspaceChanged();
                    return;
                }
                std::vector<edit::Id> ids;
                for (auto& asset : assets)
                    ids.push_back(self->workspace_.addAsset(std::move(asset)));
                self->workspace_error_.clear();
                if (append)
                    self->publishWorkspace(self->workspace_.insertAssets(ids, at_ms < 0 ? -1 : at_ms * 1000));
                else
                    emit self->workspaceChanged();
            },
            Qt::QueuedConnection);
    });
}
void EditSessionAdapter::moveClip(qulonglong id, qint64 at_ms, bool snap) {
    edit::Time at = std::max<qint64>(0, at_ms) * 1000;
    if (snap)
        at = workspace_.snap(at, id, 100000);
    publishWorkspace(workspace_.move(id, at));
}

void EditSessionAdapter::trimClip(qulonglong id, qint64 in_ms, qint64 out_ms) {
    publishWorkspace(workspace_.trim(id, in_ms * 1000, out_ms * 1000));
}

void EditSessionAdapter::splitSelected() {
    auto id = workspace_.selection();
    const auto* selected = workspace_.clip(id);
    if (!selected || selected->start >= workspace_.playhead() || selected->end() <= workspace_.playhead())
        if (const auto* active = workspace_.active(workspace_.playhead()))
            id = active->id;
    publishWorkspace(workspace_.split(id, workspace_.playhead()));
}

void EditSessionAdapter::deleteSelected(bool ripple) {
    publishWorkspace(workspace_.remove(workspace_.selection(), ripple));
}
void EditSessionAdapter::undo() {
    if (workspace_.undo())
        publishWorkspace(true);
}
void EditSessionAdapter::redo() {
    if (workspace_.redo())
        publishWorkspace(true);
}

void EditSessionAdapter::selectAdjacentClip(int direction) {
    const auto& clips = workspace_.clips();
    if (clips.empty())
        return;
    auto current =
        std::find_if(clips.begin(), clips.end(), [this](const auto& c) { return c.id == workspace_.selection(); });
    auto index = current == clips.end() ? 0 : static_cast<int>(current - clips.begin());
    index = std::clamp(index + direction, 0, static_cast<int>(clips.size()) - 1);
    selectClip(clips[static_cast<size_t>(index)].id);
}

void EditSessionAdapter::nudgeSelected(qint64 delta_ms, int edge) {
    const auto* clip = workspace_.clip(workspace_.selection());
    if (!clip)
        return;
    const auto delta = delta_ms * 1000;
    if (edge == 1)
        publishWorkspace(workspace_.trim(clip->id, clip->source_in + delta, clip->source_out));
    else if (edge == 2)
        publishWorkspace(workspace_.trim(clip->id, clip->source_in, clip->source_out + delta));
    else
        publishWorkspace(workspace_.move(clip->id, clip->start + delta));
}

} // namespace exosnap::quick
