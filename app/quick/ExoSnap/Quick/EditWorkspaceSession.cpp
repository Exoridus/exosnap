#include "EditSessionAdapter.h"

#include "models/EditCommandTypes.h"
#include "models/EditWorkspace.h"

#include <QElapsedTimer>
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

#include <QByteArray>
#include <QPointer>
#include <QVariantMap>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_types.h>

namespace exosnap::quick {

namespace {
std::optional<edit::Time> AddTimelineTime(edit::Time value, edit::Time delta) {
    if ((delta > 0 && value > (std::numeric_limits<edit::Time>::max)() - delta) ||
        (delta < 0 && value < (std::numeric_limits<edit::Time>::min)() - delta))
        return std::nullopt;
    return value + delta;
}

// The workspace asset an import of `path` produces from its probe.
edit::Asset AssetFromProbe(const QString& path, const engine::EditMediaMetadata& metadata) {
    edit::Asset asset;
    asset.path = path.toStdWString();
    asset.name = QFileInfo(path).fileName().toStdString();
    asset.duration = metadata.duration_us;
    asset.width = metadata.width;
    asset.height = metadata.height;
    asset.fps = metadata.fps;
    asset.fps_num = metadata.fps_num;
    asset.fps_den = metadata.fps_den;
    asset.audio = metadata.has_audio;
    auto imported = edit::DescribeSourceImport(metadata);
    asset.source_descriptor = std::move(imported.descriptor);
    asset.input_mix_selection = std::move(imported.input_mix);
    asset.mix_decision_required = imported.mix_decision_required;
    return asset;
}
} // namespace

bool EditSessionAdapter::FitsMilliseconds(qint64 value) {
    return value >= (std::numeric_limits<edit::Time>::min)() / 1000 &&
           value <= (std::numeric_limits<edit::Time>::max)() / 1000;
}

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
                                                     .arg(is_video ? ++video : ++audio)},
                        {QStringLiteral("video"), is_video}});
    }
    return rows;
}

QVariantList EditSessionAdapter::visibleClips(qint64 from_ms, qint64 to_ms) const {
    if (!FitsMilliseconds(from_ms) || !FitsMilliseconds(to_ms))
        return {};
    return visibleClipsUs(from_ms * 1000, to_ms * 1000);
}

QVariantList EditSessionAdapter::visibleClipsUs(qint64 from_us, qint64 to_us) const {
    QVariantList rows;
    if (from_us > to_us)
        return rows;
    const auto* selected = workspace_.clip(workspace_.selection());
    for (const auto& clip : workspace_.clips()) {
        if (clip.end() < from_us || clip.start > to_us)
            continue;
        const auto* asset = workspace_.asset(clip.asset);
        const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                        [&clip](const auto& t) { return t.id == clip.track; });
        auto cache_stream_identity = clip.stream ? clip.stream->stream_identity : engine::AudioTrackId{};
        // Foreign track UUIDs bind one open, so persistent caches use only verified recording identities.
        if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio && asset->source_descriptor &&
            !asset->source_descriptor->audio_identities_verified)
            cache_stream_identity = {};
        uint32_t source_channels = 0;
        if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio && asset->source_descriptor) {
            const auto& streams = asset->source_descriptor->streams;
            const auto source_stream = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
                return stream.ref.kind == clip.stream->kind &&
                       stream.ref.container_index == clip.stream->container_index &&
                       stream.ref.stream_identity == clip.stream->stream_identity;
            });
            if (source_stream != streams.end())
                source_channels = source_stream->codec.channels;
        }
        rows.push_back(QVariantMap{
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(clip.id)},
            {QStringLiteral("name"), QString::fromStdString(asset->name)},
            {QStringLiteral("trackIndex"), static_cast<int>(track - workspace_.tracks().begin())},
            {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(clip.track)},
            {QStringLiteral("video"), track->type == edit::TrackType::Video},
            {QStringLiteral("path"), QString::fromStdWString(asset->path)},
            {QStringLiteral("group"), QVariant::fromValue<qulonglong>(clip.group)},
            {QStringLiteral("startUs"), QVariant::fromValue<qint64>(clip.start)},
            {QStringLiteral("endUs"), QVariant::fromValue<qint64>(clip.end())},
            {QStringLiteral("durationUs"), QVariant::fromValue<qint64>(clip.duration())},
            {QStringLiteral("inUs"), QVariant::fromValue<qint64>(clip.source_in)},
            {QStringLiteral("outUs"), QVariant::fromValue<qint64>(clip.source_out)},
            {QStringLiteral("fadeInUs"), QVariant::fromValue<qint64>(clip.fade_in.duration_us)},
            {QStringLiteral("fadeOutUs"), QVariant::fromValue<qint64>(clip.fade_out.duration_us)},
            {QStringLiteral("startMs"), QVariant::fromValue<qint64>(clip.start / 1000)},
            {QStringLiteral("durationMs"), QVariant::fromValue<qint64>(clip.duration() / 1000)},
            {QStringLiteral("inMs"), QVariant::fromValue<qint64>(clip.source_in / 1000)},
            {QStringLiteral("outMs"), QVariant::fromValue<qint64>(clip.source_out / 1000)},
            {QStringLiteral("available"), asset->state == edit::AssetState::Available},
            {QStringLiteral("selected"), selected && (clip.id == selected->id || (linkedSelection && clip.group != 0 &&
                                                                                  clip.group == selected->group))},
            {QStringLiteral("fadeInMs"), clip.fade_in.duration_us / 1000},
            {QStringLiteral("fadeOutMs"), clip.fade_out.duration_us / 1000},
            {QStringLiteral("sourceInUs"), QVariant::fromValue<qint64>(clip.source_in)},
            {QStringLiteral("sourceOutUs"), QVariant::fromValue<qint64>(clip.source_out)},
            {QStringLiteral("sourceEndUs"), QVariant::fromValue<qint64>(asset->duration)},
            {QStringLiteral("sourceChannels"), source_channels},
            {QStringLiteral("width"), asset->width},
            {QStringLiteral("height"), asset->height},
            {QStringLiteral("fpsNum"), asset->fps_num},
            {QStringLiteral("fpsDen"), asset->fps_den},
            {QStringLiteral("containerIndex"), clip.stream ? static_cast<int>(clip.stream->container_index) : -1},
            {QStringLiteral("sourceIdentity"),
             asset->source_descriptor
                 ? QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(
                                                      asset->source_descriptor->asset_identity_fingerprint.data()),
                                                  32)
                                           .toHex())
                 : QString()},
            {QStringLiteral("streamIdentity"),
             clip.stream
                 ? QString::fromLatin1(
                       QByteArray(reinterpret_cast<const char*>(cache_stream_identity.bytes.data()), 16).toHex())
                 : QString()}});
    }
    return rows;
}

void EditSessionAdapter::beginImport() {
    if (pending_imports_++ == 0)
        emit importPendingChanged();
}

void EditSessionAdapter::endImport() {
    if (pending_imports_ > 0 && --pending_imports_ == 0)
        emit importPendingChanged();
}

void EditSessionAdapter::refuseEdit(const QString& message) {
    ++edit_outcome_sequence_;
    last_edit_accepted_ = false;
    workspace_error_ = message;
    emit workspaceChanged();
}

void EditSessionAdapter::publishWorkspace(bool changed) {
    ++edit_outcome_sequence_;
    last_edit_accepted_ = changed;
    if (drag_snapshot_ && drag_snapshot_->revision != workspace_.revision())
        cancelClipDrag();
    if (changed)
        selected_gap_track_ = 0;
    workspace_error_ = changed ? QString() : tr("The edit would overlap another clip or exceed the source.");
    duration_ms_ = workspace_.duration() / 1000;
    position_ms_ = workspace_.playhead() / 1000;
    const bool open = workspace_.duration() > 0;
    if (open_ != open) {
        open_ = open;
        emit openChanged();
    }
    emit workspaceChanged();
    emit durationChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    emit dragChanged();
}

void EditSessionAdapter::selectClip(qulonglong id) {
    selected_spacer_ = 0;
    selected_gap_track_ = 0;
    selected_transition_ = 0;
    workspace_.select(id);
    emit workspaceChanged();
}

QVariantList EditSessionAdapter::transitions() const {
    QVariantList result;
    for (const auto& t : workspace_.transitions()) {
        const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                        [&t](const auto& value) { return value.id == t.track; });
        result.push_back(QVariantMap{
            {QStringLiteral("outgoing"), QVariant::fromValue<qulonglong>(t.outgoing)},
            {QStringLiteral("incoming"), QVariant::fromValue<qulonglong>(t.incoming)},
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(t.id)},
            {QStringLiteral("trackIndex"), static_cast<int>(track - workspace_.tracks().begin())},
            {QStringLiteral("video"), track != workspace_.tracks().end() && track->type == edit::TrackType::Video},
            {QStringLiteral("startMs"), t.in_us / 1000},
            {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(t.track)},
            {QStringLiteral("startUs"), t.in_us},
            {QStringLiteral("endUs"), t.out_us},
            {QStringLiteral("durationUs"), t.out_us - t.in_us},
            {QStringLiteral("cutUs"), t.cut_us},
            {QStringLiteral("endMs"), t.out_us / 1000},
            {QStringLiteral("cutMs"), t.cut_us / 1000},
            {QStringLiteral("curve"), static_cast<int>(t.curve.kind)},
            {QStringLiteral("bias"), t.curve.bias},
            {QStringLiteral("audioLaw"), static_cast<int>(t.audio_law)},
            {QStringLiteral("durationMs"), (t.out_us - t.in_us) / 1000},
            {QStringLiteral("kind"),
             t.kind == edit::TransitionKind::HardCut ? QStringLiteral("hardCut") : QStringLiteral("crossfade")},
            {QStringLiteral("selected"), selected_transition_ == t.outgoing}});
    }
    return result;
}

void EditSessionAdapter::selectTransition(qulonglong outgoing) {
    selected_spacer_ = 0;
    selected_gap_track_ = 0;
    const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                    [outgoing](const auto& t) { return t.outgoing == outgoing; });
    selected_transition_ = found == workspace_.transitions().end() ? 0 : outgoing;
    workspace_.select(0);
    emit workspaceChanged();
}

bool EditSessionAdapter::canCrossfade(qulonglong outgoing) const {
    auto copy = workspace_;
    const auto* a = copy.clip(outgoing ? outgoing : copy.selection());
    if (!a)
        return false;
    for (const auto& b : copy.clips())
        if (b.track == a->track && b.start > a->start)
            return copy.crossfade(a->id, b.id);
    return false;
}

bool EditSessionAdapter::applyCrossfade(qulonglong outgoing, qint64 duration_ms) {
    if (duration_ms <= 0 || !FitsMilliseconds(duration_ms)) {
        publishWorkspace(false);
        return false;
    }
    const auto* a = workspace_.clip(outgoing ? outgoing : workspace_.selection());
    bool changed = false;
    if (a) {
        const auto id = a->id;
        for (const auto& b : workspace_.clips())
            if (b.track == a->track && b.start > a->start) {
                changed = workspace_.crossfade(id, b.id, duration_ms * 1000);
                break;
            }
        if (changed)
            selected_transition_ = id;
    }
    publishWorkspace(changed);
    return changed;
}

bool EditSessionAdapter::dropCrossfade(qint64 at_ms, qint64 tolerance_ms) {
    if (at_ms < 0 || !FitsMilliseconds(at_ms) || tolerance_ms < 0 || tolerance_ms > 250)
        return false;
    return dropCrossfadeUs(at_ms * 1000, tolerance_ms * 1000);
}

bool EditSessionAdapter::dropCrossfadeUs(qint64 at_us, qint64 tolerance_us) {
    if (at_us < 0 || tolerance_us < 0 || tolerance_us > 250'000)
        return false;
    for (const auto& clip : workspace_.clips())
        if (std::abs(clip.end() - at_us) <= tolerance_us && canCrossfade(clip.id))
            return applyCrossfade(clip.id);
    refuseEdit(tr("Crossfade needs a valid cut between adjacent video clips."));
    return false;
}

void EditSessionAdapter::removeTransition() {
    const bool changed = workspace_.removeCrossfade(selected_transition_);
    if (changed)
        selected_transition_ = 0;
    publishWorkspace(changed);
}

void EditSessionAdapter::appendAsset(qulonglong id, qint64 at_ms) {
    if (!FitsMilliseconds(at_ms)) {
        publishWorkspace(false);
        return;
    }
    appendAssetUs(id, at_ms < 0 ? -1 : at_ms * 1000);
}

void EditSessionAdapter::appendAssetUs(qulonglong id, qint64 at_us) {
    edit::AddAssetGroup command{id, at_us < 0 ? workspace_.duration() : at_us};
    if (const auto* source = workspace_.asset(id))
        command.input_mix_selection = source->input_mix_selection;
    command.auto_crossfade = autoCrossfade;
    applyCommand(command);
}

void EditSessionAdapter::addHistory(const QString& path, qint64 at_ms) {
    if (at_ms >= 0 && !FitsMilliseconds(at_ms)) {
        refuseEdit(tr("The timeline position is out of range."));
        return;
    }
    addHistoryUs(path, at_ms < 0 ? -1 : at_ms * 1000);
}

void EditSessionAdapter::addHistoryUs(const QString& path, qint64 at_us) {
    emit historyRequestedUs(path, at_us < 0 ? -1 : at_us);
}

void EditSessionAdapter::importMedia(const QUrl& url, bool append, qint64 at_ms) {
    importMediaBatch({url}, append, at_ms);
}

void EditSessionAdapter::importMediaBatch(const QList<QUrl>& urls, bool append, qint64 at_ms) {
    if (!FitsMilliseconds(at_ms)) {
        publishWorkspace(false);
        return;
    }
    importMediaBatchUs(urls, append, at_ms < 0 ? -1 : at_ms * 1000);
}

void EditSessionAdapter::importMediaBatchUs(const QList<QUrl>& urls, bool append, qint64 at_us) {
    QPointer<EditSessionAdapter> self(this);
    beginImport();
    keyframe_pool_.start([self, urls, append, at_us]() {
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
            assets.push_back(AssetFromProbe(path, metadata));
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, assets = std::move(assets), error, append, at_us]() mutable {
                if (!self)
                    return;
                if (!error.isEmpty()) {
                    self->refuseEdit(EditSessionAdapter::tr("Cannot import media: %1").arg(error));
                    self->endImport();
                    return;
                }
                std::vector<edit::Id> ids;
                for (auto& asset : assets)
                    ids.push_back(self->workspace_.addAsset(std::move(asset)));
                self->workspace_error_.clear();
                if (append)
                    self->publishWorkspace(
                        self->workspace_.insertAssets(ids, at_us < 0 ? -1 : at_us, self->autoCrossfade));
                else
                    emit self->workspaceChanged();
                self->endImport();
            },
            Qt::QueuedConnection);
    });
}
edit::Id EditSessionAdapter::addProbedMediaForHarness(const QString& path, const engine::EditMediaMetadata& metadata,
                                                      bool append) {
    const auto id = workspace_.addAsset(AssetFromProbe(path, metadata));
    workspace_error_.clear();
    if (append)
        publishWorkspace(workspace_.insertAssets({id}, -1, autoCrossfade));
    else
        emit workspaceChanged();
    return id;
}

void EditSessionAdapter::moveClip(qulonglong id, qint64 at_ms, bool snap) {
    if (!FitsMilliseconds(at_ms)) {
        publishWorkspace(false);
        return;
    }
    moveClipUs(id, at_ms * 1000, snap);
}

void EditSessionAdapter::moveClipUs(qulonglong id, qint64 at_us, bool snap) {
    edit::Time at = std::max<qint64>(0, at_us);
    if (snap)
        at = workspace_.snap(at, id, 100000);
    const auto* clip = workspace_.clip(id);
    if (!clip)
        return;
    edit::Move command{{id}, at - clip->start};
    command.expand_linked = linkedSelection;
    command.auto_crossfade = autoCrossfade;
    command.ripple = ripple;
    applyCommand(command);
}

void EditSessionAdapter::trimClip(qulonglong id, qint64 in_ms, qint64 out_ms) {
    if (in_ms < 0 || out_ms < 0 || !FitsMilliseconds(in_ms) || !FitsMilliseconds(out_ms)) {
        publishWorkspace(false);
        return;
    }
    trimClipUs(id, in_ms * 1000, out_ms * 1000);
}

void EditSessionAdapter::trimClipUs(qulonglong id, qint64 in_us, qint64 out_us) {
    if (in_us < 0 || out_us < 0) {
        publishWorkspace(false);
        return;
    }
    const auto* clip = workspace_.clip(id);
    if (!clip)
        return;
    if (in_us != clip->source_in && out_us != clip->source_out) {
        if (ripple || !linkedSelection) {
            refuseEdit(tr("Adjust one trim edge at a time for ripple or independent edits."));
        } else {
            publishWorkspace(workspace_.trim(id, in_us, out_us, autoCrossfade));
        }
        return;
    }
    const bool left = in_us != clip->source_in;
    edit::Trim command{id, left ? edit::TrimEdge::Left : edit::TrimEdge::Right,
                       left ? in_us - clip->source_in : out_us - clip->source_out};
    command.mode = ripple ? edit::TrimMode::Ripple : edit::TrimMode::Fixed;
    command.expand_linked = linkedSelection;
    command.auto_crossfade = autoCrossfade;
    applyCommand(command);
}

void EditSessionAdapter::splitSelected() {
    auto id = workspace_.selection();
    const auto* selected = workspace_.clip(id);
    if (!selected || selected->start >= workspace_.playhead() || selected->end() <= workspace_.playhead())
        if (const auto* active = workspace_.active(workspace_.playhead()))
            id = active->id;
    applyCommand(edit::Split{{id}, workspace_.playhead(), linkedSelection});
}

void EditSessionAdapter::deleteSelected(bool ripple_delete) {
    if (selected_spacer_) {
        if (ripple_delete) {
            for (const auto& spacer : workspace_.snapshot()->spacers)
                if (spacer.id == selected_spacer_) {
                    selected_gap_ = {spacer.start_us, spacer.end()};
                    selected_gap_track_ = spacer.track;
                    previewCloseGap();
                    return;
                }
        } else {
            const auto id = std::exchange(selected_spacer_, 0);
            applyCommand(edit::RemoveSpacer{id});
        }
        return;
    }
    if (selected_gap_track_) {
        if (ripple_delete)
            previewCloseGap();
        return;
    }
    if (ripple_delete)
        applyCommand(edit::RippleRemoveClips{{workspace_.selection()}, linkedSelection});
    else
        applyCommand(edit::LiftClips{{workspace_.selection()}, linkedSelection});
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
    if (!FitsMilliseconds(delta_ms)) {
        publishWorkspace(false);
        return;
    }
    nudgeSelectedUs(delta_ms * 1000, edge);
}

void EditSessionAdapter::nudgeSelectedUs(qint64 delta, int edge) {
    const auto* clip = workspace_.clip(workspace_.selection());
    if (!clip)
        return;
    const auto boundary = edge == 1 ? clip->source_in : edge == 2 ? clip->source_out : clip->start;
    const auto requested = AddTimelineTime(boundary, delta);
    if (!requested) {
        publishWorkspace(false);
        return;
    }
    if (edge == 1)
        trimClipUs(clip->id, *requested, clip->source_out);
    else if (edge == 2)
        trimClipUs(clip->id, clip->source_in, *requested);
    else
        moveClipUs(clip->id, *requested, false);
}

bool EditSessionAdapter::applyCommand(const edit::EditCommand& command) {
    QElapsedTimer clock;
    clock.start();
    const auto result = workspace_.apply(command, workspace_.revision());
    command_model_ns_ = clock.nsecsElapsed();
    clock.restart();
    publishWorkspace(result.status != edit::CommandStatus::Rejected);
    command_publish_ns_ = clock.nsecsElapsed();
    return result.status != edit::CommandStatus::Rejected;
}

void EditSessionAdapter::beginClipDrag(qulonglong id, int edge) {
    cancelClipDrag();
    if (!workspace_.clip(id))
        return;
    selected_gap_track_ = 0;
    drag_snapshot_ = workspace_.snapshot();
    drag_clip_ = id;
    drag_playhead_ = workspace_.playhead();
    drag_edge_ = edge;
    updateClipDrag(0);
}

void EditSessionAdapter::beginJunctionDrag(qulonglong outgoing, int edge) {
    cancelClipDrag();
    const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                    [outgoing](const auto& junction) { return junction.outgoing == outgoing; });
    if (found == workspace_.transitions().end())
        return;
    selected_gap_track_ = 0;
    drag_snapshot_ = workspace_.snapshot();
    drag_junction_ = found->id;
    drag_edge_ = edge;
    updateClipDrag(0);
}

void EditSessionAdapter::beginSpacerDrag(qulonglong id, int edge) {
    cancelClipDrag();
    const auto snapshot = workspace_.snapshot();
    if (std::none_of(snapshot->spacers.begin(), snapshot->spacers.end(),
                     [id](const auto& value) { return value.id == id; }))
        return;
    selected_gap_track_ = 0;
    drag_snapshot_ = snapshot;
    drag_spacer_ = id;
    drag_edge_ = edge;
    updateClipDrag(0);
}

void EditSessionAdapter::updateClipDrag(qint64 delta_ms, qulonglong track, int new_track, bool insert,
                                        qint64 tolerance_ms) {
    if (!FitsMilliseconds(delta_ms)) {
        drag_command_.reset();
        drag_preview_.valid = false;
        emit dragChanged();
        return;
    }
    updateClipDragUs(delta_ms * 1000, track, new_track, insert, std::clamp<qint64>(tolerance_ms, 0, 250) * 1000);
}

void EditSessionAdapter::updateClipDragUs(qint64 delta, qulonglong track, int new_track, bool insert,
                                          qint64 tolerance_us) {
    if (!drag_snapshot_)
        return;
    const auto reject = [this]() {
        drag_command_.reset();
        drag_preview_.valid = false;
        emit dragChanged();
    };
    if (delta == (std::numeric_limits<edit::Time>::min)()) {
        reject();
        return;
    }
    if (drag_spacer_) {
        if (drag_edge_ < 0)
            drag_command_ = edit::MoveSpacer{drag_spacer_, delta};
        else
            drag_command_ =
                edit::TrimSpacer{drag_spacer_, drag_edge_ == 0 ? edit::TrimEdge::Left : edit::TrimEdge::Right, delta};
        drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
        emit dragChanged();
        return;
    }
    if (drag_junction_) {
        const auto junction = std::find_if(drag_snapshot_->junctions.begin(), drag_snapshot_->junctions.end(),
                                           [this](const auto& value) { return value.id == drag_junction_; });
        if (junction == drag_snapshot_->junctions.end())
            return;
        const auto start = AddTimelineTime(junction->in_us, drag_edge_ == 0 ? delta : 0);
        const auto end = AddTimelineTime(junction->out_us, drag_edge_ == 1 ? delta : 0);
        if (!start || !end) {
            reject();
            return;
        }
        drag_command_ = edit::SetJunctionWindow{junction->id, {*start, *end}};
        drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
        emit dragChanged();
        return;
    }
    const auto lead = std::find_if(drag_snapshot_->clips.begin(), drag_snapshot_->clips.end(),
                                   [this](const auto& clip) { return clip.id == drag_clip_; });
    if (lead == drag_snapshot_->clips.end())
        return;
    if (drag_edge_ >= 2) {
        const bool incoming = drag_edge_ == 2;
        const auto duration = AddTimelineTime(incoming ? lead->fade_in.duration_us : lead->fade_out.duration_us,
                                              incoming ? delta : -delta);
        if (!duration) {
            reject();
            return;
        }
        drag_command_ = edit::SetFade{
            lead->id, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, {std::max<edit::Time>(0, *duration)}};
        drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
        emit dragChanged();
        return;
    }
    if (snapping) {
        const auto boundary = drag_edge_ == 1 ? lead->end() : lead->start;
        const auto requested = AddTimelineTime(boundary, delta);
        if (!requested) {
            reject();
            return;
        }
        auto best = static_cast<uint64_t>(std::clamp<qint64>(tolerance_us, 0, 250000) + 1);
        auto snapped = delta;
        const auto consider = [&](edit::Time marker) {
            if (marker < 0)
                return;
            const auto distance = marker >= *requested
                                      ? static_cast<uint64_t>(marker) - static_cast<uint64_t>(*requested)
                                      : static_cast<uint64_t>(*requested) - static_cast<uint64_t>(marker);
            if (distance < best) {
                best = distance;
                snapped = marker - boundary;
            }
        };
        consider(0);
        consider(drag_snapshot_->result_range.in_us);
        if (drag_snapshot_->result_range.out_mode == edit::ResultOutMode::Fixed)
            consider(drag_snapshot_->result_range.out_us);
        consider(drag_playhead_);
        if (time_selection_in_us_ >= 0)
            consider(time_selection_in_us_);
        if (time_selection_out_us_ >= 0)
            consider(time_selection_out_us_);
        for (const auto& source : drag_snapshot_->assets)
            if (source->id == lead->asset)
                for (const auto& marker : source->markers) {
                    if (marker.time_ms > static_cast<uint64_t>((std::numeric_limits<edit::Time>::max)() / 1000))
                        continue;
                    const auto relative = static_cast<edit::Time>(marker.time_ms) * 1000 - lead->source_in;
                    if (const auto position = AddTimelineTime(lead->start, relative))
                        consider(*position);
                }
        for (const auto& clip : drag_snapshot_->clips)
            if (clip.id != lead->id && (!linkedSelection || !lead->group || clip.group != lead->group)) {
                consider(clip.start);
                consider(clip.end());
            }
        delta = snapped;
    }
    if (drag_edge_ >= 0) {
        edit::Trim command{drag_clip_, drag_edge_ == 0 ? edit::TrimEdge::Left : edit::TrimEdge::Right, delta};
        command.mode = ripple ? edit::TrimMode::Ripple : edit::TrimMode::Fixed;
        command.expand_linked = linkedSelection;
        command.auto_crossfade = autoCrossfade;
        drag_command_ = command;
    } else {
        edit::Move command{{drag_clip_}, delta};
        command.expand_linked = linkedSelection;
        command.auto_crossfade = autoCrossfade;
        command.ripple = ripple || insert;
        if (new_track >= 0) {
            const auto kind = new_track == 2 ? edit::TrackType::Audio : edit::TrackType::Video;
            track = drag_snapshot_->id_high_watermark + 1;
            command.new_tracks.push_back({track, kind});
            command.new_video_front = new_track == 0;
            if (linkedSelection && lead->group) {
                std::vector<std::pair<edit::Id, edit::Id>> destinations{{lead->track, track}};
                for (const auto& clip : drag_snapshot_->clips) {
                    if (clip.id == lead->id || clip.group != lead->group)
                        continue;
                    auto destination = std::find_if(destinations.begin(), destinations.end(),
                                                    [&clip](const auto& value) { return value.first == clip.track; });
                    if (destination == destinations.end()) {
                        const auto source_track =
                            std::find_if(drag_snapshot_->tracks.begin(), drag_snapshot_->tracks.end(),
                                         [&clip](const auto& value) { return value.id == clip.track; });
                        const auto temporary = drag_snapshot_->id_high_watermark + command.new_tracks.size() + 1;
                        command.new_tracks.push_back({temporary, source_track->type});
                        destinations.emplace_back(clip.track, temporary);
                        destination = destinations.end() - 1;
                    }
                    command.target_tracks.emplace_back(clip.id, destination->second);
                }
            }
        }
        if (track)
            command.target_tracks.emplace_back(drag_clip_, track);
        drag_command_ = command;
    }
    drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
    emit dragChanged();
}

bool EditSessionAdapter::commitClipDrag() {
    if (!drag_snapshot_ || !drag_command_ || !dragValid()) {
        cancelClipDrag();
        refuseEdit(tr("The drag is unavailable or the timeline changed. Try again."));
        return false;
    }
    const auto result = workspace_.apply(*drag_command_, drag_snapshot_->revision);
    cancelClipDrag();
    publishWorkspace(result.status != edit::CommandStatus::Rejected);
    return result.status != edit::CommandStatus::Rejected;
}

void EditSessionAdapter::cancelClipDrag() {
    drag_snapshot_.reset();
    drag_command_.reset();
    drag_preview_ = {};
    drag_junction_ = 0;
    drag_spacer_ = 0;
    emit dragChanged();
}

QVariantList EditSessionAdapter::dragTracks() const {
    QVariantList rows;
    for (const auto& track : drag_preview_.tracks)
        rows.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(track.id)},
                                   {QStringLiteral("video"), track.type == edit::TrackType::Video}});
    return rows;
}

QVariantList EditSessionAdapter::dragClips() const {
    QVariantList rows;
    if (!drag_snapshot_)
        return rows;
    for (const auto& clip : drag_preview_.clips) {
        if (std::find(drag_snapshot_->clips.begin(), drag_snapshot_->clips.end(), clip) != drag_snapshot_->clips.end())
            continue;
        const auto track = std::find_if(drag_preview_.tracks.begin(), drag_preview_.tracks.end(),
                                        [&clip](const auto& value) { return value.id == clip.track; });
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(clip.id)},
                        {QStringLiteral("trackIndex"), static_cast<int>(track - drag_preview_.tracks.begin())},
                        {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(clip.track)},
                        {QStringLiteral("startMs"), clip.start / 1000.0},
                        {QStringLiteral("startUs"), clip.start},
                        {QStringLiteral("endUs"), clip.end()},
                        {QStringLiteral("durationUs"), clip.duration()},
                        {QStringLiteral("fadeInUs"), clip.fade_in.duration_us},
                        {QStringLiteral("fadeOutUs"), clip.fade_out.duration_us},
                        {QStringLiteral("durationMs"), clip.duration() / 1000.0},
                        {QStringLiteral("fadeInMs"), clip.fade_in.duration_us / 1000.0},
                        {QStringLiteral("fadeOutMs"), clip.fade_out.duration_us / 1000.0}});
    }
    return rows;
}

QVariantList EditSessionAdapter::dragTransitions() const {
    QVariantList rows;
    for (const auto& junction : drag_preview_.junctions) {
        const auto track = std::find_if(drag_preview_.tracks.begin(), drag_preview_.tracks.end(),
                                        [&junction](const auto& value) { return value.id == junction.track; });
        rows.push_back(
            QVariantMap{{QStringLiteral("startMs"), junction.in_us / 1000.0},
                        {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(junction.track)},
                        {QStringLiteral("startUs"), junction.in_us},
                        {QStringLiteral("endUs"), junction.out_us},
                        {QStringLiteral("durationUs"), junction.out_us - junction.in_us},
                        {QStringLiteral("durationMs"), (junction.out_us - junction.in_us) / 1000.0},
                        {QStringLiteral("trackIndex"), static_cast<int>(track - drag_preview_.tracks.begin())}});
    }
    return rows;
}

void EditSessionAdapter::requestFrameStep(int direction) {
    const auto fps = workspace_.snapshot()->output_fps;
    const auto next = direction < 0 ? fps.previousFrame(workspace_.playhead()) : fps.nextFrame(workspace_.playhead());
    if (next) {
        emit inspectionRequested();
        requestSeekUs(*next);
    }
}

void EditSessionAdapter::setSelectionBoundary(bool out) {
    if (out)
        time_selection_out_us_ = workspace_.playhead();
    else
        time_selection_in_us_ = workspace_.playhead();
    emit timeSelectionChanged();
}

void EditSessionAdapter::setJunctionWindow(qint64 start_ms, qint64 end_ms) {
    if (!FitsMilliseconds(start_ms) || !FitsMilliseconds(end_ms)) {
        publishWorkspace(false);
        return;
    }
    for (const auto& junction : workspace_.transitions())
        if (junction.outgoing == selected_transition_) {
            applyCommand(edit::SetJunctionWindow{junction.id, {start_ms * 1000, end_ms * 1000}});
            return;
        }
}
void EditSessionAdapter::setJunctionCurve(int kind, double bias) {
    for (const auto& junction : workspace_.transitions())
        if (junction.outgoing == selected_transition_) {
            applyCommand(edit::SetJunctionCurve{junction.id, {static_cast<engine::CurveKind>(kind), bias}});
            return;
        }
}
void EditSessionAdapter::setJunctionAudioLaw(int law) {
    for (const auto& junction : workspace_.transitions())
        if (junction.outgoing == selected_transition_) {
            applyCommand(edit::SetAudioLaw{junction.id, static_cast<edit::AudioGainLaw>(law)});
            return;
        }
}
void EditSessionAdapter::setJunctionCut(qint64 at_ms) {
    if (!FitsMilliseconds(at_ms)) {
        publishWorkspace(false);
        return;
    }
    for (const auto& junction : workspace_.transitions())
        if (junction.outgoing == selected_transition_) {
            applyCommand(edit::SetHardCut{junction.id, at_ms * 1000});
            return;
        }
}
void EditSessionAdapter::setClipFade(qulonglong id, bool incoming, qint64 duration_ms) {
    if (!FitsMilliseconds(duration_ms)) {
        publishWorkspace(false);
        return;
    }
    applyCommand(edit::SetFade{id, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, {duration_ms * 1000}});
}

QVariantMap EditSessionAdapter::selectedClipDetails() const {
    for (const auto& value : visibleClipsUs(0, workspace_.duration()))
        if (value.toMap().value(QStringLiteral("id")).toULongLong() == workspace_.selection())
            return value.toMap();
    return {};
}

QVariantMap EditSessionAdapter::selectedJunctionDetails() const {
    for (const auto& value : transitions())
        if (value.toMap().value(QStringLiteral("selected")).toBool())
            return value.toMap();
    return {};
}

bool EditSessionAdapter::deleteTrack(qulonglong id, bool confirmed) {
    if (!confirmed) {
        pending_track_delete_ = id;
        pending_track_delete_revision_ = workspace_.revision();
        return applyCommand(edit::DeleteTrack{id});
    }
    if (id != pending_track_delete_)
        return false;
    const auto result =
        workspace_.apply(edit::DeleteTrack{id, pending_track_delete_revision_}, pending_track_delete_revision_);
    pending_track_delete_ = 0;
    publishWorkspace(result.status != edit::CommandStatus::Rejected);
    return result.status != edit::CommandStatus::Rejected;
}

namespace {
QVariantList SpacerRows(const std::vector<edit::Spacer>& spacers, const std::vector<edit::Track>& tracks) {
    QVariantList rows;
    for (const auto& spacer : spacers) {
        const auto track = std::find_if(tracks.begin(), tracks.end(),
                                        [&spacer](const auto& value) { return value.id == spacer.track; });
        rows.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(spacer.id)},
                                   {QStringLiteral("trackIndex"), static_cast<int>(track - tracks.begin())},
                                   {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(spacer.track)},
                                   {QStringLiteral("startMs"), spacer.start_us / 1000.0},
                                   {QStringLiteral("startUs"), spacer.start_us},
                                   {QStringLiteral("endUs"), spacer.end()},
                                   {QStringLiteral("durationUs"), spacer.duration_us},
                                   {QStringLiteral("durationMs"), spacer.duration_us / 1000.0}});
    }
    return rows;
}
} // namespace

QVariantList EditSessionAdapter::spacers() const {
    const auto snapshot = workspace_.snapshot();
    return SpacerRows(snapshot->spacers, snapshot->tracks);
}

QVariantList EditSessionAdapter::dragSpacers() const {
    return SpacerRows(drag_preview_.spacers, drag_preview_.tracks);
}

void EditSessionAdapter::selectSpacer(qulonglong id) {
    const auto snapshot = workspace_.snapshot();
    if (std::none_of(snapshot->spacers.begin(), snapshot->spacers.end(),
                     [id](const auto& value) { return value.id == id; }))
        return;
    selected_spacer_ = id;
    selected_gap_track_ = 0;
    selected_transition_ = 0;
    workspace_.select(0);
    emit workspaceChanged();
}

QVariantMap EditSessionAdapter::selectedGap() const {
    if (!selected_gap_track_)
        return {};
    const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                    [this](const auto& value) { return value.id == selected_gap_track_; });
    return {{QStringLiteral("startMs"), selected_gap_.in_us / 1000},
            {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(selected_gap_track_)},
            {QStringLiteral("startUs"), selected_gap_.in_us},
            {QStringLiteral("endUs"), selected_gap_.out_us},
            {QStringLiteral("endMs"), selected_gap_.out_us / 1000},
            {QStringLiteral("trackIndex"), static_cast<int>(track - workspace_.tracks().begin())}};
}

void EditSessionAdapter::selectGap(qulonglong track, qint64 at_ms) {
    if (!FitsMilliseconds(at_ms))
        return;
    selectGapUs(track, at_ms * 1000);
}

void EditSessionAdapter::selectGapUs(qulonglong track, qint64 at) {
    if (std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(),
                     [track](const auto& value) { return value.id == track; }))
        return;
    edit::Span span{0, workspace_.duration()};
    const auto inspect = [&](edit::Time start, edit::Time end) {
        if (start <= at && at < end)
            span = {0, 0};
        else if (end <= at)
            span.in_us = std::max(span.in_us, end);
        else if (start > at)
            span.out_us = std::min(span.out_us, start);
    };
    for (const auto& clip : workspace_.clips())
        if (clip.track == track)
            inspect(clip.start, clip.end());
    for (const auto& spacer : workspace_.snapshot()->spacers)
        if (spacer.track == track)
            inspect(spacer.start_us, spacer.end());
    if (span.out_us <= span.in_us || at < span.in_us || at >= span.out_us)
        return;
    selected_gap_ = span;
    selected_gap_track_ = track;
    selected_spacer_ = 0;
    selected_transition_ = 0;
    workspace_.select(0);
    emit workspaceChanged();
}

void EditSessionAdapter::copyBlankDuration() {
    if (selected_spacer_)
        for (const auto& spacer : workspace_.snapshot()->spacers)
            if (spacer.id == selected_spacer_) {
                copied_blank_duration_ = spacer.duration_us;
                copied_blank_track_ = spacer.track;
                return;
            }
    if (!selected_gap_track_)
        return;
    copied_blank_duration_ = selected_gap_.out_us - selected_gap_.in_us;
    copied_blank_track_ = selected_gap_track_;
}

void EditSessionAdapter::pasteSpacer() {
    if (copied_blank_duration_ > 0)
        applyCommand(edit::AddSpacer{selected_gap_track_ ? selected_gap_track_ : copied_blank_track_,
                                     workspace_.playhead(), copied_blank_duration_});
}

void EditSessionAdapter::previewCloseGap() {
    if (!selected_gap_track_)
        return;
    cancelClipDrag();
    drag_snapshot_ = workspace_.snapshot();
    drag_command_ = edit::CloseGap{selected_gap_};
    drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
    emit dragChanged();
}

void EditSessionAdapter::setFixedTimelineEnd(bool fixed, qint64 time_ms) {
    if (!FitsMilliseconds(time_ms)) {
        publishWorkspace(false);
        return;
    }
    applyCommand(
        edit::SetTimelineEnd{{fixed ? edit::TimelineEndMode::FixedEnd : edit::TimelineEndMode::FollowContentEnd,
                              fixed ? time_ms * 1000 : 0}});
}

} // namespace exosnap::quick
