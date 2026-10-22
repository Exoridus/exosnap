#include "EditRenderText.h"
#include "EditSessionAdapter.h"
#include "EditTimelineModels.h"

#include "models/EditCommandTypes.h"
#include "models/EditPointerGeometry.h"
#include "models/EditTime.h"
#include "models/EditTimelineModel.h"
#include "models/EditWorkspace.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QtCore/Qt>
#include <QtCore/qcontainerfwd.h>
#include <QtCore/qtmetamacros.h>
#include <QtCore/qtypes.h>

#include <QByteArray>
#include <QPointer>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QVariantMap>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/media_cache_store.h>

namespace exosnap::edit {

SourceImport DescribeSourceImport(const engine::EditMediaMetadata& metadata, Time duration,
                                  const std::shared_ptr<const engine::MediaReadCapability>& access) {
    const bool bound = access && metadata.file_identity == access->Identity().file_identity &&
                       metadata.file_size == access->Identity().file_size;
    return source_import_detail::Describe(metadata, duration, bound);
}

} // namespace exosnap::edit

namespace exosnap::quick {

namespace {
std::optional<edit::Time> AddTimelineTime(edit::Time value, edit::Time delta) {
    if ((delta > 0 && value > (std::numeric_limits<edit::Time>::max)() - delta) ||
        (delta < 0 && value < (std::numeric_limits<edit::Time>::min)() - delta))
        return std::nullopt;
    return value + delta;
}

QVariantMap JunctionEnvelope(const edit::Transition& junction, const edit::Clip& outgoing, const edit::Clip& incoming,
                             bool audio, qreal from, qreal to, int count) {
    if (!std::isfinite(from) || !std::isfinite(to) || from < 0 || to > 1 || from >= to || count < 1 || count > 4096)
        return {};
    const auto start = std::max(outgoing.start, incoming.start);
    const auto end = std::min(outgoing.end(), incoming.end());
    if (end <= start)
        return {};
    const auto span = end - start;
    const engine::TimelineJunction projected{junction.id,    junction.track,     junction.outgoing, junction.incoming,
                                             junction.kind,  junction.in_us,     junction.out_us,   junction.cut_us,
                                             junction.curve, junction.audio_law, junction.anchor};
    QVariantList outgoing_points, incoming_points;
    outgoing_points.reserve(count + 1);
    incoming_points.reserve(count + 1);
    for (int index = 0; index <= count; ++index) {
        const auto progress = from + (to - from) * index / count;
        const auto offset = static_cast<double>(span) * progress;
        const auto time = start + (offset >= static_cast<double>(span) ? span : static_cast<edit::Time>(offset));
        const auto out = engine::timeline_detail::JunctionGain(projected, false, time, audio);
        const auto in = engine::timeline_detail::JunctionGain(projected, true, time, audio);
        if (!out || !in)
            return {};
        outgoing_points.push_back(*out);
        incoming_points.push_back(*in);
    }
    return {{QStringLiteral("outgoing"), outgoing_points}, {QStringLiteral("incoming"), incoming_points}};
}

std::optional<edit::CrossfadeAtCut> AdjacentCutCommand(const edit::EditSnapshot& snapshot, edit::Id outgoing,
                                                       edit::Time duration, bool linked) {
    const auto a = std::find_if(snapshot.clips.begin(), snapshot.clips.end(),
                                [outgoing](const auto& clip) { return clip.id == outgoing; });
    if (a == snapshot.clips.end())
        return std::nullopt;
    const auto b = std::find_if(snapshot.clips.begin(), snapshot.clips.end(), [&](const auto& clip) {
        return clip.id != a->id && clip.track == a->track && clip.start == a->end();
    });
    if (b == snapshot.clips.end())
        return std::nullopt;
    return edit::CrossfadeAtCut{a->id, b->id, duration, linked};
}

template <typename Snapshot>
bool FinalizeCapturedSourceDefault(Snapshot& snapshot, edit::Id id, uint64_t version,
                                   const edit::InputMixSelection& mix,
                                   const std::shared_ptr<const edit::ValidatedSourceDescriptor>& descriptor) {
    const auto source = std::find_if(snapshot.assets.begin(), snapshot.assets.end(),
                                     [id](const auto& asset) { return asset && asset->id == id; });
    if (source == snapshot.assets.end() || (*source)->source_version != version || !(*source)->mix_decision_required ||
        !(*source)->source_descriptor || (*source)->source_descriptor->file_identity != descriptor->file_identity)
        return false;
    const auto& expected = (*source)->source_descriptor->asset_identity_fingerprint;
    if (std::any_of(expected.begin(), expected.end(), [](auto byte) { return byte != 0; }) &&
        expected != mix.asset_identity_fingerprint)
        return false;
    auto finalized = std::make_shared<edit::Asset>(**source);
    finalized->source_descriptor = descriptor;
    finalized->input_mix_selection = mix;
    finalized->mix_decision_required = false;
    *source = std::move(finalized);
    edit::RebaseImportedAudioDefaults(snapshot.clips, id, mix);
    return true;
}
// The workspace asset an import of `path` produces from its probe.
edit::Asset AssetFromProbe(const QString& path, const engine::EditMediaMetadata& metadata,
                           const std::shared_ptr<const engine::MediaReadCapability>& access = {}) {
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
    auto imported = edit::DescribeSourceImport(metadata, 0, access);
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

void EditSessionAdapter::finishPointerSeek() {
    const auto target =
        edit::BoundedGridTime(workspace_.playhead(), 0, workspace_.duration(), workspace_.snapshot()->output_fps,
                              timeValueUsesSamples(0, QStringLiteral("playhead")));
    if (target && *target != workspace_.playhead())
        requestSeekUs(*target);
}

QVariantList EditSessionAdapter::selectedClipIds() const {
    QVariantList ids;
    for (const auto id : selection_.clip_ids)
        ids.push_back(QVariant::fromValue<qulonglong>(id));
    return ids;
}

QVariantList EditSessionAdapter::soloTrackIds() const {
    QVariantList ids;
    for (const auto id : solo_tracks_)
        ids.push_back(QVariant::fromValue<qulonglong>(id));
    return ids;
}

QVariantMap EditSessionAdapter::parseTimeValue(const QString& text, qint64 current, bool samples, int format) const {
    if (format < -1 || format > 2)
        return {{QStringLiteral("valid"), false},
                {QStringLiteral("valueUs"), current},
                {QStringLiteral("error"), tr("Choose a valid time format.")},
                {QStringLiteral("gridLabel"), QString()}};
    const auto fps = workspace_.snapshot()->output_fps;
    const auto bytes = text.toUtf8();
    const auto active_format = format < 0 ? (samples ? edit::TimeFieldFormat::Samples : edit::TimeFieldFormat::Seconds)
                                          : static_cast<edit::TimeFieldFormat>(format);
    const auto parsed = edit::ParseTime(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())),
                                        {fps, current, active_format});
    QString error;
    switch (parsed.error) {
    case edit::TimeParseError::None:
        break;
    case edit::TimeParseError::InvalidRate:
        error = tr("The output frame rate is unavailable.");
        break;
    case edit::TimeParseError::InvalidFrame:
        error = tr("The frame number is outside the output timecode rate.");
        break;
    case edit::TimeParseError::NegativeTime:
        error = tr("Time cannot be negative.");
        break;
    case edit::TimeParseError::Overflow:
        error = tr("The time is too large.");
        break;
    case edit::TimeParseError::Precision:
        error = tr("This time cannot be represented in whole microseconds.");
        break;
    case edit::TimeParseError::InvalidSyntax:
        error = tr("Enter a valid time, for example 1.25s, 120ms or 10f.");
        break;
    }
    const auto grid = samples        ? tr("48 kHz samples")
                      : !fps.valid() ? tr("Frames (rate unavailable)")
                      : fps.den == 1 ? tr("%1 fps").arg(fps.num)
                                     : tr("%1/%2 fps").arg(fps.num).arg(fps.den);
    return {{QStringLiteral("valid"), static_cast<bool>(parsed)},
            {QStringLiteral("valueUs"), parsed ? parsed.value_us : current},
            {QStringLiteral("error"), error},
            {QStringLiteral("gridLabel"), grid}};
}

int EditSessionAdapter::defaultTimeFormat() const {
    return static_cast<int>(workspace_.snapshot()->output_fps.valid() ? edit::TimeFieldFormat::Timecode
                                                                      : edit::TimeFieldFormat::Seconds);
}

QString EditSessionAdapter::formatTimeValue(qint64 value, int format) const {
    if (format < 0 || format > 2)
        return {};
    const auto text = edit::FormatTime(value, workspace_.snapshot()->output_fps,
                                       static_cast<edit::TimeFieldFormat>(format == 0 ? defaultTimeFormat() : format));
    return text ? QString::fromStdString(*text) : QString();
}

QVariantMap EditSessionAdapter::parseGainValue(const QString& text, bool track) const {
    static const QRegularExpression syntax(QStringLiteral(R"(^[+-]?(?:[0-9]+(?:[.,][0-9]?)?|[.,][0-9])(?:\s*dB)?$)"),
                                           QRegularExpression::CaseInsensitiveOption);
    auto number = text.trimmed();
    const auto invalid = [this, track] {
        return QVariantMap{{QStringLiteral("valid"), false},
                           {QStringLiteral("value"), 0.0},
                           {QStringLiteral("error"), track
                                                         ? tr("Enter a gain from -60.0 to +12.0 dB in 0.1 dB steps.")
                                                         : tr("Enter a gain from -60.0 to +24.0 dB in 0.1 dB steps.")}};
    };
    if (!syntax.match(number).hasMatch())
        return invalid();
    if (number.endsWith(QStringLiteral("dB"), Qt::CaseInsensitive))
        number.chop(2);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool parsed = false;
    const auto value = number.trimmed().toDouble(&parsed);
    if (!parsed || !std::isfinite(value) || value < -60 || value > (track ? 12 : 24))
        return invalid();
    return {{QStringLiteral("valid"), true}, {QStringLiteral("value"), value}, {QStringLiteral("error"), QString()}};
}

QString EditSessionAdapter::formatGainValue(double value) const {
    return std::isfinite(value) ? QString::number(value == 0 ? 0 : value, 'f', 1) : QString();
}

void EditSessionAdapter::setPendingDraftOwner(QObject* owner, qulonglong target) {
    if (owner && owner->thread() == thread()) {
        pending_draft_owner_ = owner;
        pending_draft_target_ = target;
    }
}

void EditSessionAdapter::clearPendingDraftOwner(QObject* owner) {
    if (pending_draft_owner_ == owner) {
        pending_draft_owner_.clear();
        pending_draft_target_ = 0;
    }
}

bool EditSessionAdapter::preflightPendingDraft() {
    if (range_scope_review_ && !range_scope_action_) {
        refuseEdit(tr("Apply or cancel the track scope review before continuing."));
        return false;
    }
    if (committing_pending_draft_ || !pending_draft_owner_)
        return true;
    const auto owner = pending_draft_owner_;
    bool accepted = false;
    committing_pending_draft_ = true;
    const bool invoked =
        QMetaObject::invokeMethod(owner, "commitPendingDraft", Qt::DirectConnection, Q_RETURN_ARG(bool, accepted));
    committing_pending_draft_ = false;
    if (invoked && accepted) {
        if (owner)
            clearPendingDraftOwner(owner);
        return true;
    }
    refuseEdit(tr("Correct or cancel the pending value before continuing."));
    return false;
}

void EditSessionAdapter::setInteractionSetting(bool& setting, bool value) {
    if (setting == value)
        return;
    if (preflightPendingDraft())
        setting = value;
    // A refused write must restore the bound value of a checkable control.
    emit interactionSettingsChanged();
}

void EditSessionAdapter::setSnapping(bool value) {
    setInteractionSetting(snapping, value);
}

void EditSessionAdapter::setRipple(bool value) {
    setInteractionSetting(ripple, value);
}

void EditSessionAdapter::setAutoCrossfade(bool value) {
    setInteractionSetting(autoCrossfade, value);
}

void EditSessionAdapter::setLinkedSelection(bool value) {
    setInteractionSetting(linkedSelection, value);
}

void EditSessionAdapter::cancelPendingDraft(const QString& reason) {
    const auto owner = pending_draft_owner_;
    pending_draft_owner_.clear();
    pending_draft_target_ = 0;
    if (owner)
        QMetaObject::invokeMethod(owner, "cancelPendingDraft", Qt::DirectConnection, Q_ARG(QString, reason));
}

bool EditSessionAdapter::applyTimeValue(qulonglong target, const QString& field, qint64 value) {
    if (!preflightPendingDraft())
        return false;
    const auto refuse = [this](const QString& reason) {
        refuseEdit(reason);
        return false;
    };
    if (value < 0)
        return refuse(tr("Time cannot be negative."));
    if (field == QStringLiteral("playhead")) {
        if (value > workspace_.duration())
            return refuse(tr("The playhead must be within the timeline."));
        requestSeekUs(value);
        return true;
    }
    if (field == QStringLiteral("timelineEnd"))
        return applyCommand(edit::SetTimelineEnd{{edit::TimelineEndMode::FixedEnd, value}});
    if (field.startsWith(QStringLiteral("range"))) {
        if (selection_.mode != edit::SelectionMode::Range || !selection_.range)
            return refuse(tr("Select a range before editing its time."));
        const auto range = *selection_.range;
        QVariantList tracks;
        for (const auto id : range.track_ids)
            tracks.push_back(QVariant::fromValue<qulonglong>(id));
        if (field == QStringLiteral("rangeStart"))
            return setEditRangeUs(value, range.end_us, tracks, range.end_us);
        if (field == QStringLiteral("rangeEnd"))
            return setEditRangeUs(range.start_us, value, tracks, range.start_us);
        if (field == QStringLiteral("rangeDuration") && value > 0) {
            const auto end = AddTimelineTime(range.start_us, value);
            if (end)
                return setEditRangeUs(range.start_us, *end, tracks, range.start_us);
        }
        return refuse(tr("The range time is outside the valid editing range."));
    }
    if (field.startsWith(QStringLiteral("junction"))) {
        const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                        [target](const auto& junction) { return junction.id == target; });
        if (found == workspace_.transitions().end())
            return refuse(tr("The edited transition is no longer available."));
        if (field == QStringLiteral("junctionCut"))
            return applyCommand(edit::SetHardCut{target, value});
        edit::Span span{found->in_us, found->out_us};
        if (field == QStringLiteral("junctionStart"))
            span.in_us = value;
        else if (field == QStringLiteral("junctionEnd"))
            span.out_us = value;
        else if (field == QStringLiteral("junctionDuration") && value > 0) {
            const auto end = AddTimelineTime(span.in_us, value);
            if (!end)
                return refuse(tr("The transition duration is too large."));
            span.out_us = *end;
        } else {
            return refuse(tr("The transition time is outside the valid editing range."));
        }
        return applyCommand(edit::SetJunctionWindow{target, span});
    }
    const auto* clip = workspace_.clip(target);
    if (!clip)
        return refuse(tr("The edited clip is no longer available."));
    if (field == QStringLiteral("clipStart")) {
        edit::Move command{{target}, value - clip->start};
        command.expand_linked = linkedSelection;
        command.auto_crossfade = autoCrossfade;
        return applyCommand(command);
    }
    if (field == QStringLiteral("fadeIn") || field == QStringLiteral("fadeOut")) {
        const bool incoming = field == QStringLiteral("fadeIn");
        const auto other = incoming ? clip->fade_out.duration_us : clip->fade_in.duration_us;
        if (value > clip->duration() - other)
            return refuse(tr("The fade cannot exceed the available clip duration."));
        auto fade = incoming ? clip->fade_in : clip->fade_out;
        fade.duration_us = value;
        return applyCommand(edit::SetFade{target, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, fade});
    }
    edit::Trim command;
    command.lead_id = target;
    command.targets = {target};
    command.expand_linked = linkedSelection;
    command.auto_crossfade = autoCrossfade;
    command.clamp_to_bounds = false;
    if (field == QStringLiteral("sourceIn")) {
        if (value >= clip->source_out)
            return refuse(tr("The source start must be before the source end."));
        command.edge = edit::TrimEdge::Left;
        command.delta_us = value - clip->source_in;
    } else if (field == QStringLiteral("sourceOut")) {
        if (value <= clip->source_in)
            return refuse(tr("The source end must be after the source start."));
        command.edge = edit::TrimEdge::Right;
        command.delta_us = value - clip->source_out;
    } else if (field == QStringLiteral("duration") && value > 0) {
        const auto end = AddTimelineTime(clip->source_in, value);
        if (!end)
            return refuse(tr("The clip duration is too large."));
        command.edge = edit::TrimEdge::Right;
        command.delta_us = *end - clip->source_out;
    } else {
        return refuse(tr("The clip time is outside the valid editing range."));
    }
    return applyCommand(command);
}

bool EditSessionAdapter::timeValueUsesSamples(qulonglong target, const QString& field) const {
    const auto audio_track = [this](edit::Id id) {
        const auto found = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                        [id](const auto& track) { return track.id == id; });
        return found != workspace_.tracks().end() && found->type == edit::TrackType::Audio;
    };
    if (field.startsWith(QStringLiteral("range")) || field == QStringLiteral("playhead")) {
        if (selection_.mode == edit::SelectionMode::Range && selection_.range)
            return !selection_.range->track_ids.empty() &&
                   std::all_of(selection_.range->track_ids.begin(), selection_.range->track_ids.end(), audio_track);
        if (field != QStringLiteral("playhead") || selection_.mode != edit::SelectionMode::Clips ||
            selection_.clip_ids.empty())
            return false;
        for (const auto id : selection_.clip_ids) {
            const auto* clip = workspace_.clip(id);
            if (!clip || !audio_track(clip->track))
                return false;
            if (clip->group &&
                std::any_of(workspace_.clips().begin(), workspace_.clips().end(), [&](const auto& member) {
                    return member.group == clip->group &&
                           std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), member.id) ==
                               selection_.clip_ids.end();
                }))
                return false;
        }
        return true;
    }
    if (field.startsWith(QStringLiteral("junction"))) {
        const auto junction = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                           [target](const auto& value) { return value.id == target; });
        return junction != workspace_.transitions().end() && audio_track(junction->track);
    }
    const auto* clip = workspace_.clip(target);
    if (!clip || !audio_track(clip->track))
        return false;
    if (field == QStringLiteral("fadeIn") || field == QStringLiteral("fadeOut"))
        return true;
    if (field != QStringLiteral("clipStart") && field != QStringLiteral("sourceIn") &&
        field != QStringLiteral("sourceOut") && field != QStringLiteral("duration"))
        return false;
    return !linkedSelection || !clip->group ||
           std::none_of(workspace_.clips().begin(), workspace_.clips().end(),
                        [&](const auto& member) { return member.group == clip->group && !audio_track(member.track); });
}

bool EditSessionAdapter::copyClips() {
    if (!preflightPendingDraft())
        return false;
    if (selection_.mode != edit::SelectionMode::Clips || selection_.clip_ids.empty()) {
        refuseEdit(tr("Select clips to copy/cut."));
        return false;
    }
    const auto copied = edit::CopyClips(*workspace_.snapshot(), selection_.clip_ids);
    if (!copied) {
        refuseEdit(tr("The selected clips are unavailable for copying."));
        return false;
    }
    clip_clipboard_ = copied;
    workspace_error_.clear();
    emit clipboardChanged();
    emit workspaceChanged();
    return true;
}

bool EditSessionAdapter::cutClips() {
    if (!copyClips())
        return false;
    return applyCommand(edit::LiftClips{selection_.clip_ids, false});
}

bool EditSessionAdapter::previewPasteClipsUs(qint64 at, qulonglong track, bool insert) {
    if (!preflightPendingDraft())
        return false;
    cancelClipDrag();
    cancelRangeGesture();
    if (!clip_clipboard_) {
        refuseEdit(tr("Copy clips before pasting."));
        return false;
    }
    for (const auto& asset : clip_clipboard_->assets)
        if (asset && sourceVerificationFailed(asset->id)) {
            refuseEdit(tr("The clipboard source is unavailable or changed. Relink it before pasting."));
            return false;
        }
    drag_snapshot_ = workspace_.snapshot();
    drag_selection_ =
        GestureSelection{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_};
    drag_command_ =
        edit::PasteClips{clip_clipboard_, at, track ? track : selection_.active_track, insert, autoCrossfade};
    drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
    emit dragChanged();
    if (!drag_preview_.valid) {
        const auto reason = dragReason();
        cancelClipDrag();
        refuseEdit(reason);
        return false;
    }
    return true;
}

bool EditSessionAdapter::pasteClips(bool insert) {
    return previewPasteClipsUs(workspace_.playhead(), selection_.active_track, insert) && commitClipDrag();
}

bool EditSessionAdapter::setTrackSolo(qulonglong id, bool solo) {
    const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                    [id](const auto& value) { return value.id == id; });
    if (track == workspace_.tracks().end() || track->type != edit::TrackType::Audio)
        return false;
    const bool changed = solo ? solo_tracks_.insert(id).second : solo_tracks_.erase(id) != 0;
    if (changed) {
        emit auditionChanged();
        emit workspaceChanged();
    }
    return true;
}

bool EditSessionAdapter::beginRangeGestureUs(qint64 anchor, const QVariantList& track_ids, bool extend_scope,
                                             bool scope_only) {
    if (!preflightPendingDraft())
        return false;
    cancelRangeGesture();
    const auto snapshot = workspace_.snapshot();
    if (anchor < 0 || anchor > edit::SnapshotDuration(*snapshot) || track_ids.isEmpty())
        return false;
    std::set<edit::Id> seen;
    std::vector<edit::Id> tracks;
    for (const auto& value : track_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid || !seen.insert(id).second ||
            std::none_of(snapshot->tracks.begin(), snapshot->tracks.end(),
                         [id](const auto& track) { return track.id == id; }))
            return false;
        tracks.push_back(id);
    }
    cancelClipDrag();
    range_gesture_snapshot_ = snapshot;
    range_selection_ = pointer_selection_ ? pointer_selection_->original
                                          : GestureSelection{selection_, selected_transition_, selected_spacer_,
                                                             selected_gap_track_, selected_gap_};
    pointer_selection_.reset();
    range_gesture_tracks_ = std::move(tracks);
    range_gesture_first_track_ = range_gesture_tracks_.front();
    range_gesture_base_ = selection_.range;
    range_gesture_extend_scope_ = extend_scope && bool(range_gesture_base_);
    range_gesture_scope_only_ = scope_only && bool(range_gesture_base_);
    range_gesture_anchor_ =
        (range_gesture_extend_scope_ || range_gesture_scope_only_) ? range_gesture_base_->anchor_us : anchor;
    updateRangeGestureUs(range_gesture_scope_only_ ? range_gesture_base_->end_us : anchor);
    return true;
}

void EditSessionAdapter::setRangeGestureCandidate(edit::Time start, edit::Time end, const std::vector<edit::Id>& tracks,
                                                  edit::Time anchor) {
    range_gesture_preview_ = edit::PreviewEditRange(*range_gesture_snapshot_, start, end, tracks, anchor);
    range_gesture_candidate_ =
        range_gesture_preview_->valid() ? std::optional<edit::EditRange>{range_gesture_preview_->range} : std::nullopt;
}

bool EditSessionAdapter::previewRangeScope(const QVariantList& track_ids) {
    QScopedValueRollback scope_action(range_scope_action_, true);
    if (!preflightPendingDraft() || !selection_.range)
        return false;
    const auto base = *selection_.range;
    std::vector<edit::Id> tracks;
    for (const auto& value : track_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid) {
            refuseEdit(tr("Choose existing tracks for the range."));
            return false;
        }
        tracks.push_back(id);
    }
    cancelRangeGesture();
    cancelClipDrag();
    range_gesture_snapshot_ = workspace_.snapshot();
    range_selection_ =
        GestureSelection{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_};
    range_gesture_base_ = base;
    range_gesture_tracks_ = tracks;
    range_gesture_anchor_ = base.anchor_us;
    range_scope_review_ = true;
    setRangeGestureCandidate(base.start_us, base.end_us, tracks, base.anchor_us);
    emit rangeGestureChanged();
    return true;
}

bool EditSessionAdapter::includeLinkedRangeTracks() {
    if (!selection_.range)
        return false;
    const auto& range = *selection_.range;
    const auto snapshot = workspace_.snapshot();
    const auto& initial =
        range_scope_review_ && range_gesture_preview_ ? range_gesture_preview_->range.track_ids : range.track_ids;
    std::set<edit::Id> tracks(initial.begin(), initial.end());
    std::set<edit::Id> groups;
    bool changed;
    do {
        changed = false;
        for (const auto& clip : snapshot->clips)
            if (clip.group && tracks.contains(clip.track) && clip.start < range.end_us && clip.end() > range.start_us)
                groups.insert(clip.group);
        for (const auto& clip : snapshot->clips)
            if (clip.group && groups.contains(clip.group))
                changed = tracks.insert(clip.track).second || changed;
    } while (changed);
    QVariantList scope;
    for (const auto& track : snapshot->tracks)
        if (tracks.contains(track.id))
            scope.push_back(QVariant::fromValue<qulonglong>(track.id));
    return previewRangeScope(scope);
}

QVariantList EditSessionAdapter::rangeTrackSpan(qulonglong first, qulonglong last) const {
    const auto& tracks = workspace_.tracks();
    const auto from =
        std::find_if(tracks.begin(), tracks.end(), [first](const auto& track) { return track.id == first; });
    const auto to = std::find_if(tracks.begin(), tracks.end(), [last](const auto& track) { return track.id == last; });
    if (from == tracks.end() || to == tracks.end())
        return {};
    QVariantList result;
    for (auto track = std::min(from, to); track != std::max(from, to) + 1; ++track)
        result.push_back(QVariant::fromValue<qulonglong>(track->id));
    return result;
}

void EditSessionAdapter::updateRangeGestureToTrackUs(qint64 endpoint, qulonglong current) {
    const auto tracks = rangeTrackSpan(range_gesture_first_track_, current);
    if (!tracks.isEmpty())
        updateRangeGestureUs(endpoint, tracks);
}

void EditSessionAdapter::updateRangeGestureUs(qint64 endpoint, const QVariantList& current_span) {
    if (!range_gesture_snapshot_)
        return;
    range_gesture_candidate_.reset();
    range_gesture_preview_.reset();
    if (range_gesture_snapshot_->revision == workspace_.revision()) {
        auto tracks = range_gesture_tracks_;
        if (!current_span.isEmpty()) {
            tracks.clear();
            for (const auto& value : current_span) {
                bool valid = false;
                const auto id = value.toULongLong(&valid);
                if (!valid) {
                    emit rangeGestureChanged();
                    return;
                }
                tracks.push_back(id);
            }
        }
        if (range_gesture_extend_scope_ || range_gesture_scope_only_)
            for (const auto id : range_gesture_base_->track_ids)
                if (std::find(tracks.begin(), tracks.end(), id) == tracks.end())
                    tracks.push_back(id);
        const auto start = range_gesture_scope_only_ ? range_gesture_base_->start_us : range_gesture_anchor_;
        const auto end = range_gesture_scope_only_ ? range_gesture_base_->end_us : endpoint;
        setRangeGestureCandidate(start, end, tracks, range_gesture_anchor_);
    }
    emit rangeGestureChanged();
}

bool EditSessionAdapter::commitRangeGesture() {
    QScopedValueRollback scope_action(range_scope_action_, true);
    if (!preflightPendingDraft()) {
        cancelRangeGesture();
        return false;
    }
    if (!rangeGestureValid()) {
        const auto reason = rangeGestureReason();
        cancelRangeGesture();
        if (!reason.isEmpty())
            refuseEdit(reason);
        return false;
    }
    const auto range = *range_gesture_candidate_;
    QVariantList tracks;
    for (const auto id : range.track_ids)
        tracks.push_back(QVariant::fromValue<qulonglong>(id));
    range_selection_.reset();
    cancelRangeGesture();
    return setEditRangeUs(range.start_us, range.end_us, tracks, range.anchor_us);
}

void EditSessionAdapter::cancelRangeGesture() {
    const bool active = bool(range_gesture_snapshot_);
    range_gesture_snapshot_.reset();
    range_gesture_candidate_.reset();
    range_gesture_preview_.reset();
    range_scope_review_ = false;
    range_gesture_tracks_.clear();
    range_gesture_base_.reset();
    range_gesture_first_track_ = 0;
    range_band_target_ = 0;
    if (range_selection_) {
        auto original = std::move(*range_selection_);
        range_selection_.reset();
        restoreGestureSelection(std::move(original));
    }
    if (active)
        emit rangeGestureChanged();
}

bool EditSessionAdapter::rangeGestureValid() const {
    return range_gesture_snapshot_ && range_gesture_candidate_ &&
           range_gesture_snapshot_->revision == workspace_.revision();
}

QVariantMap EditSessionAdapter::rangeGestureCandidate() const {
    if (!range_gesture_preview_)
        return {};
    const auto& range = range_gesture_preview_->range;
    QVariantList tracks;
    for (const auto id : range.track_ids)
        tracks.push_back(QVariant::fromValue<qulonglong>(id));
    return {{QStringLiteral("startUs"), range.start_us},
            {QStringLiteral("endUs"), range.end_us},
            {QStringLiteral("durationUs"), range.end_us - range.start_us},
            {QStringLiteral("trackIds"), tracks},
            {QStringLiteral("anchorUs"), range.anchor_us},
            {QStringLiteral("valid"), rangeGestureValid()}};
}

QString EditSessionAdapter::rangeGestureReason() const {
    if (!range_gesture_snapshot_)
        return {};
    if (range_gesture_snapshot_->revision != workspace_.revision())
        return TranslateEditError({edit::EditErrorCode::StaleRevision, {}, {}});
    if (!range_gesture_preview_)
        return tr("The range needs distinct valid boundaries and existing tracks.");
    switch (range_gesture_preview_->validity) {
    case edit::RangeValidity::Valid:
        return {};
    case edit::RangeValidity::EmptyScope:
        return tr("Choose at least one track for the range.");
    case edit::RangeValidity::InvalidTrack:
        return tr("Choose existing tracks for the range.");
    case edit::RangeValidity::OutOfBounds:
        return tr("The range boundaries must remain inside the timeline.");
    case edit::RangeValidity::Collapsed:
        return tr("Both boundaries fall on the same grid position. Choose a longer range or a different track scope.");
    }
    return {};
}

bool EditSessionAdapter::setClipGain(qulonglong id, double gain_db) {
    return applyCommand(edit::SetGain{edit::GainTarget::Clip, id, gain_db});
}

bool EditSessionAdapter::setClipGains(const QVariantList& ids, double gain) {
    std::vector<edit::Id> targets;
    std::set<edit::Id> seen;
    for (const auto& value : ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid || !id || !seen.insert(id).second)
            return false;
        targets.push_back(id);
    }
    return !targets.empty() && applyCommand(edit::SetGain{edit::GainTarget::Clip, 0, gain, std::move(targets)});
}
bool EditSessionAdapter::setTrackGain(qulonglong id, double gain_db) {
    return applyCommand(edit::SetGain{edit::GainTarget::Track, id, gain_db});
}
bool EditSessionAdapter::setTrackEnabled(qulonglong id, bool enabled) {
    return applyCommand(edit::SetTrackEnabled{id, enabled});
}
bool EditSessionAdapter::setTrackName(qulonglong id, const QString& name) {
    return applyCommand(edit::RenameTrack{id, name.trimmed().toStdString()});
}

bool EditSessionAdapter::reorderTrack(qulonglong id, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= workspace_.tracks().size())
        return false;
    return applyCommand(edit::ReorderTrack{id, static_cast<std::size_t>(index)});
}

QVariantMap EditSessionAdapter::editRange() const {
    if (!selection_.range)
        return {};
    QVariantList tracks;
    for (const auto id : selection_.range->track_ids)
        tracks.push_back(QVariant::fromValue<qulonglong>(id));
    return {{QStringLiteral("startUs"), selection_.range->start_us},
            {QStringLiteral("endUs"), selection_.range->end_us},
            {QStringLiteral("durationUs"), selection_.range->end_us - selection_.range->start_us},
            {QStringLiteral("trackIds"), tracks},
            {QStringLiteral("anchorUs"), selection_.range->anchor_us}};
}

void EditSessionAdapter::syncSelection() {
    const auto selected = workspace_.selection();
    const auto snapshot = workspace_.snapshot();
    if (range_gesture_snapshot_ && range_gesture_snapshot_->revision != snapshot->revision)
        cancelRangeGesture();
    if (pending_draft_owner_ && pending_draft_target_) {
        const auto target = pending_draft_target_;
        const bool exists = std::any_of(snapshot->clips.begin(), snapshot->clips.end(),
                                        [target](const auto& value) { return value.id == target; }) ||
                            std::any_of(snapshot->tracks.begin(), snapshot->tracks.end(),
                                        [target](const auto& value) { return value.id == target; }) ||
                            std::any_of(snapshot->junctions.begin(), snapshot->junctions.end(),
                                        [target](const auto& value) { return value.id == target; }) ||
                            std::any_of(snapshot->spacers.begin(), snapshot->spacers.end(),
                                        [target](const auto& value) { return value.id == target; });
        if (!exists)
            cancelPendingDraft(tr("The edited item is no longer available."));
    }
    selection_.prune(*snapshot);
    const auto removed_solos = std::erase_if(solo_tracks_, [&](const auto id) {
        return std::none_of(snapshot->tracks.begin(), snapshot->tracks.end(),
                            [id](const auto& track) { return track.id == id && track.type == edit::TrackType::Audio; });
    });
    if (removed_solos)
        emit auditionChanged();
    if ((selection_.mode == edit::SelectionMode::Spacer &&
         std::none_of(snapshot->spacers.begin(), snapshot->spacers.end(),
                      [this](const auto& spacer) { return spacer.id == selected_spacer_; })) ||
        (selection_.mode == edit::SelectionMode::Junction &&
         std::none_of(snapshot->junctions.begin(), snapshot->junctions.end(),
                      [this](const auto& junction) { return junction.outgoing == selected_transition_; })) ||
        (selection_.mode == edit::SelectionMode::Gap && !selected_gap_track_))
        selection_.clear();
    if (selected && workspace_.clip(selected) &&
        (selection_.mode == edit::SelectionMode::None ||
         (selection_.mode == edit::SelectionMode::Clips &&
          std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), selected) == selection_.clip_ids.end())))
        selection_.selectClip(*snapshot, selected, false, false, linkedSelection);
    workspace_.select(selection_.mode == edit::SelectionMode::Clips ? selection_.primary_clip : 0);
    if (selection_.mode != edit::SelectionMode::Spacer)
        selected_spacer_ = 0;
    if (selection_.mode != edit::SelectionMode::Gap)
        selected_gap_track_ = 0;
    if (selection_.mode != edit::SelectionMode::Junction)
        selected_transition_ = 0;
    const auto start = selection_.range ? selection_.range->start_us : -1;
    const auto end = selection_.range ? selection_.range->end_us : -1;
    if (start != time_selection_in_us_ || end != time_selection_out_us_) {
        time_selection_in_us_ = start;
        time_selection_out_us_ = end;
        emit timeSelectionChanged();
    }
    emit selectionChanged();
}

void EditSessionAdapter::publishSelection() {
    syncSelection();
    emit workspaceChanged();
}

void EditSessionAdapter::selectClipWithModifiers(qulonglong id, bool toggle, bool extend) {
    if (!preflightPendingDraft())
        return;
    cancelRangeGesture();
    if (!selection_.selectClip(*workspace_.snapshot(), id, toggle, extend, linkedSelection))
        return;
    workspace_.select(selection_.primary_clip);
    publishSelection();
}

bool EditSessionAdapter::beginClipPointerSelection(qulonglong id, bool toggle, bool extend, bool preserve_selection) {
    if (!preflightPendingDraft() || !workspace_.clip(id) || (toggle && extend))
        return false;
    finishPointerSelection(true);
    cancelClipDrag();
    cancelRangeGesture();
    pointer_selection_ =
        PointerSelection{{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_},
                         workspace_.revision(),
                         0,
                         linkedSelection,
                         snapping,
                         ripple,
                         autoCrossfade};
    pointer_selection_->loop_bounds = loop_snap_bounds_;
    if (selection_.range)
        pointer_selection_->range_bounds = edit::Span{selection_.range->start_us, selection_.range->end_us};
    pointer_selection_->playhead = workspace_.playhead();
    if (!preserve_selection && !toggle && !extend)
        selection_.clear();
    if (toggle && selection_.mode == edit::SelectionMode::Clips &&
        std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), id) != selection_.clip_ids.end()) {
        pointer_selection_->deferred_toggle = id;
        return true;
    }
    selection_.selectClip(*workspace_.snapshot(), id, toggle, extend, linkedSelection);
    workspace_.select(selection_.primary_clip);
    publishSelection();
    return true;
}

bool EditSessionAdapter::beginLanePointerSelection(qulonglong track, qint64 at) {
    if (!preflightPendingDraft() || at < 0 || at > workspace_.duration() ||
        std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(),
                     [track](const auto& row) { return row.id == track; }))
        return false;
    finishPointerSelection(true);
    cancelClipDrag();
    cancelRangeGesture();
    pointer_selection_ =
        PointerSelection{{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_},
                         workspace_.revision(),
                         0,
                         linkedSelection,
                         snapping,
                         ripple,
                         autoCrossfade};
    pointer_selection_->loop_bounds = loop_snap_bounds_;
    if (selection_.range)
        pointer_selection_->range_bounds = edit::Span{selection_.range->start_us, selection_.range->end_us};
    pointer_selection_->playhead = workspace_.playhead();
    selectGapUs(track, at);
    requestSeekUs(at);
    return true;
}

bool EditSessionAdapter::linkSelectedClips() {
    return selection_.clip_ids.size() >= 2 && applyCommand(edit::Relink{selection_.clip_ids});
}

bool EditSessionAdapter::unlinkSelectedClips() {
    return !selection_.clip_ids.empty() && applyCommand(edit::Unlink{selection_.clip_ids});
}

void EditSessionAdapter::selectClipArea(qint64 start, qint64 end, const QVariantList& track_ids,
                                        const QVariantList& base_ids, bool exact) {
    if (!preflightPendingDraft())
        return;
    cancelRangeGesture();
    const auto ids = [](const QVariantList& values) {
        std::vector<edit::Id> result;
        for (const auto& value : values)
            result.push_back(value.toULongLong());
        return result;
    };
    selection_.selectArea(*workspace_.snapshot(), std::min(start, end), std::max(start, end), ids(track_ids),
                          ids(base_ids), linkedSelection && !exact);
    workspace_.select(selection_.primary_clip);
    publishSelection();
}

void EditSessionAdapter::setLoopSnapBounds(qint64 start, qint64 end) {
    if (start >= 0 && end > start)
        loop_snap_bounds_ = edit::Span{start, end};
    else
        loop_snap_bounds_.reset();
}

void EditSessionAdapter::restoreGestureSelection(GestureSelection original) {
    selection_ = std::move(original.selection);
    selection_.prune(*workspace_.snapshot());
    selected_transition_ = original.transition;
    selected_spacer_ = original.spacer;
    selected_gap_track_ = original.gap_track;
    selected_gap_ = original.gap;
    if (selected_gap_track_ && std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(),
                                            [&](const auto& track) { return track.id == selected_gap_track_; }))
        selected_gap_track_ = 0;
    workspace_.select(selection_.mode == edit::SelectionMode::Clips ? selection_.primary_clip : 0);
    publishSelection();
}

void EditSessionAdapter::finishPointerSelection(bool cancel, bool dragged) {
    if (!pointer_selection_)
        return;
    auto captured = std::move(*pointer_selection_);
    pointer_selection_.reset();
    if (cancel || captured.revision != workspace_.revision()) {
        restoreGestureSelection(std::move(captured.original));
    } else if (!dragged && captured.deferred_toggle) {
        selection_.selectClip(*workspace_.snapshot(), captured.deferred_toggle, true, false, captured.linked);
        workspace_.select(selection_.primary_clip);
        publishSelection();
    }
}

QVariantMap EditSessionAdapter::selectionBounds() const {
    edit::Time start = (std::numeric_limits<edit::Time>::max)();
    edit::Time end = 0;
    if (selection_.mode == edit::SelectionMode::Range && selection_.range) {
        start = selection_.range->start_us;
        end = selection_.range->end_us;
    } else if (selection_.mode == edit::SelectionMode::Clips) {
        for (const auto id : selection_.clip_ids)
            if (const auto* clip = workspace_.clip(id)) {
                start = std::min(start, clip->start);
                end = std::max(end, clip->end());
            }
    }
    if (start >= end)
        return {{QStringLiteral("valid"), false}};
    return {{QStringLiteral("valid"), true}, {QStringLiteral("startUs"), start}, {QStringLiteral("endUs"), end}};
}

bool EditSessionAdapter::executeTimelineAction(const QString& action) {
    if (!preflightPendingDraft())
        return false;
    const bool next = action.endsWith(QStringLiteral("next"));
    const int direction = next ? 1 : -1;
    if (action == QStringLiteral("frame-next") || action == QStringLiteral("frame-previous")) {
        requestFrameStep(direction);
    } else if (action == QStringLiteral("sample-next") || action == QStringLiteral("sample-previous")) {
        if (!timeValueUsesSamples(0, QStringLiteral("playhead")))
            return false;
        const auto position = edit::AudioSampleGrid::step(workspace_.playhead(), direction);
        if (!position)
            return false;
        emit inspectionRequested();
        requestSeekUs(*position);
    } else if (action == QStringLiteral("second-next") || action == QStringLiteral("second-previous")) {
        const auto position = AddTimelineTime(workspace_.playhead(), direction * edit::Time{1'000'000});
        if (!position)
            return false;
        requestSeekUs(*position);
    } else if (action == QStringLiteral("timeline-start") || action == QStringLiteral("timeline-end")) {
        requestSeekUs(action == QStringLiteral("timeline-start") ? 0 : workspace_.duration());
    } else if (action == QStringLiteral("boundary-next") || action == QStringLiteral("boundary-previous")) {
        std::set<edit::Time> boundaries{0, workspace_.duration()};
        for (const auto& clip : workspace_.clips())
            if (!selection_.active_track || clip.track == selection_.active_track) {
                boundaries.insert(clip.start);
                boundaries.insert(clip.end());
            }
        const auto snapshot = workspace_.snapshot();
        for (const auto& spacer : snapshot->spacers)
            if (!selection_.active_track || spacer.track == selection_.active_track) {
                boundaries.insert(spacer.start_us);
                boundaries.insert(spacer.end());
            }
        for (const auto& junction : snapshot->junctions)
            if (!selection_.active_track || junction.track == selection_.active_track) {
                boundaries.insert(junction.in_us);
                boundaries.insert(junction.out_us);
                if (junction.kind == edit::TransitionKind::HardCut)
                    boundaries.insert(junction.cut_us);
            }
        const auto current = workspace_.playhead();
        auto boundary = next ? boundaries.upper_bound(current) : boundaries.lower_bound(current);
        if (next && boundary == boundaries.end())
            return true;
        if (!next) {
            if (boundary == boundaries.begin())
                return true;
            --boundary;
        }
        emit inspectionRequested();
        requestSeekUs(*boundary);
    } else if (action == QStringLiteral("clip-next") || action == QStringLiteral("clip-previous")) {
        selectAdjacentClip(direction);
    } else if (action == QStringLiteral("nudge-next") || action == QStringLiteral("nudge-previous") ||
               action == QStringLiteral("sample-nudge-next") || action == QStringLiteral("sample-nudge-previous") ||
               action == QStringLiteral("trim-start") || action == QStringLiteral("trim-end") ||
               action == QStringLiteral("extend-start") || action == QStringLiteral("extend-end")) {
        const auto* clip = workspace_.clip(workspace_.selection());
        if (!clip || selection_.mode != edit::SelectionMode::Clips)
            return true;
        const bool sample = action.startsWith(QStringLiteral("sample-"));
        if (sample && !timeValueUsesSamples(0, QStringLiteral("playhead")))
            return false;
        const bool left = action.endsWith(QStringLiteral("start"));
        const bool edge = left || action.endsWith(QStringLiteral("end"));
        const bool extend = action.startsWith(QStringLiteral("extend-"));
        const int offset = edge ? (left != extend ? 1 : -1) : direction;
        const auto boundary = edge ? (left ? clip->start : clip->end()) : clip->start;
        const auto target = sample ? edit::AudioSampleGrid::step(boundary, offset)
                                   : workspace_.snapshot()->output_fps.step(boundary, offset);
        if (!target)
            return false;
        nudgeSelectedUs(*target - boundary, edge ? (left ? 1 : 2) : 0);
    } else if (action == QStringLiteral("range-in") || action == QStringLiteral("range-out")) {
        setSelectionBoundary(action == QStringLiteral("range-out"));
    } else if (action == QStringLiteral("clear-range")) {
        clearEditRange();
    } else if (action == QStringLiteral("select-all")) {
        selectAllClips();
    } else if (action == QStringLiteral("split")) {
        splitSelected();
    } else if (action == QStringLiteral("lift") || action == QStringLiteral("ripple-remove")) {
        deleteSelected(action == QStringLiteral("ripple-remove"));
    } else if (action == QStringLiteral("copy")) {
        return copyClips();
    } else if (action == QStringLiteral("cut")) {
        return cutClips();
    } else if (action == QStringLiteral("paste") || action == QStringLiteral("insert-paste")) {
        return previewPasteClipsUs(workspace_.playhead(), selection_.active_track,
                                   action == QStringLiteral("insert-paste"));
    } else if (action == QStringLiteral("undo")) {
        undo();
    } else if (action == QStringLiteral("redo")) {
        redo();
    } else if (action == QStringLiteral("mute-track")) {
        const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                        [&](const auto& row) { return row.id == selection_.active_track; });
        if (track == workspace_.tracks().end() || track->type != edit::TrackType::Audio)
            return false;
        return setTrackEnabled(track->id, !track->edit_enabled);
    } else {
        return false;
    }
    return true;
}

bool EditSessionAdapter::beginClipGainDrag(qulonglong id, bool fine) {
    const auto* clip = workspace_.clip(id);
    if (!clip || std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(), [&](const auto& track) {
            return track.id == clip->track && track.type == edit::TrackType::Audio;
        }))
        return false;
    beginClipDrag(id);
    if (!drag_snapshot_)
        return false;
    drag_gain_ = true;
    drag_gain_fine_ = fine;
    updateClipGainDrag(0);
    return true;
}

void EditSessionAdapter::updateClipGainDrag(qreal delta_y) {
    if (!drag_snapshot_ || !drag_gain_ || !std::isfinite(delta_y))
        return;
    const auto lead = std::find_if(drag_snapshot_->clips.begin(), drag_snapshot_->clips.end(),
                                   [&](const auto& clip) { return clip.id == drag_clip_; });
    if (lead == drag_snapshot_->clips.end())
        return;
    const auto value =
        std::round(std::clamp(lead->gain_db - delta_y * (drag_gain_fine_ ? 0.02 : 0.2), -60.0, 24.0) * 10) / 10;
    drag_command_ = edit::SetGain{edit::GainTarget::Clip, lead->id, value};
    drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
    emit dragChanged();
}

void EditSessionAdapter::updateClipFadeDragPixels(qreal dx, qreal pixels_per_us, bool fine) {
    if (!drag_snapshot_ || drag_edge_ < 2)
        return;
    const auto reject = [this] {
        drag_command_.reset();
        drag_preview_.valid = false;
        emit dragChanged();
    };
    if (!std::isfinite(dx) || !std::isfinite(pixels_per_us) || pixels_per_us <= 0) {
        reject();
        return;
    }
    const auto lead = std::find_if(drag_snapshot_->clips.begin(), drag_snapshot_->clips.end(),
                                   [this](const auto& clip) { return clip.id == drag_clip_; });
    if (lead == drag_snapshot_->clips.end())
        return;
    long double delta = dx / pixels_per_us;
    if (fine) {
        const auto steps = std::trunc(static_cast<long double>(dx) / 4);
        if (steps <= (std::numeric_limits<int64_t>::min)() || steps >= (std::numeric_limits<int64_t>::max)()) {
            reject();
            return;
        }
        const auto endpoint =
            drag_edge_ == 2 ? lead->start + lead->fade_in.duration_us : lead->end() - lead->fade_out.duration_us;
        const bool audio =
            std::any_of(drag_snapshot_->tracks.begin(), drag_snapshot_->tracks.end(), [&](const auto& track) {
                return track.id == lead->track && track.type == edit::TrackType::Audio;
            });
        const auto shifted = audio ? edit::AudioSampleGrid::step(endpoint, static_cast<int64_t>(steps))
                                   : drag_snapshot_->output_fps.step(endpoint, static_cast<int64_t>(steps));
        if (!shifted) {
            reject();
            return;
        }
        delta = *shifted - endpoint;
    }
    if (!std::isfinite(delta) || delta <= (std::numeric_limits<edit::Time>::min)() ||
        delta >= (std::numeric_limits<edit::Time>::max)()) {
        reject();
        return;
    }
    updateClipDragUs(static_cast<edit::Time>(std::round(delta)));
}

bool EditSessionAdapter::beginRangeBandGestureUs(qint64 at, qint64 tolerance) {
    if (!preflightPendingDraft() || at < 0 || tolerance < 0)
        return false;
    const auto base = selection_.range;
    const auto target =
        edit::ResolveRangePointerTarget(static_cast<double>(at), base ? static_cast<double>(base->start_us) : 0,
                                        base ? static_cast<double>(base->end_us) : 0, static_cast<double>(tolerance),
                                        bool(base), static_cast<edit::RangePointerTarget>(last_range_edge_));
    if (target == edit::RangePointerTarget::None)
        return false;
    QVariantList tracks;
    if (base && target != edit::RangePointerTarget::Create) {
        for (const auto id : base->track_ids)
            tracks.push_back(QVariant::fromValue<qulonglong>(id));
    } else {
        for (const auto& track : workspace_.tracks())
            tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    }
    if (!beginRangeGestureUs(at, tracks))
        return false;
    range_band_target_ = static_cast<int>(target);
    range_band_press_ = at;
    if (target == edit::RangePointerTarget::Start || target == edit::RangePointerTarget::End)
        last_range_edge_ = range_band_target_;
    updateRangeBandGestureUs(at);
    return true;
}

void EditSessionAdapter::updateRangeBandGestureUs(qint64 at) {
    if (!range_gesture_snapshot_)
        return;
    const auto target = static_cast<edit::RangePointerTarget>(range_band_target_);
    range_gesture_candidate_.reset();
    range_gesture_preview_.reset();
    if (range_gesture_snapshot_->revision == workspace_.revision()) {
        edit::Time start = range_band_press_, end = at, anchor = range_band_press_;
        if (range_gesture_base_ && target != edit::RangePointerTarget::Create) {
            const auto& base = *range_gesture_base_;
            if (target == edit::RangePointerTarget::Body) {
                const auto delta = AddTimelineTime(at, -range_band_press_);
                const auto shifted_start = delta ? AddTimelineTime(base.start_us, *delta) : std::nullopt;
                const auto shifted_end = delta ? AddTimelineTime(base.end_us, *delta) : std::nullopt;
                const auto shifted_anchor = delta ? AddTimelineTime(base.anchor_us, *delta) : std::nullopt;
                if (!shifted_start || !shifted_end || !shifted_anchor) {
                    emit rangeGestureChanged();
                    return;
                }
                start = *shifted_start;
                end = *shifted_end;
                anchor = *shifted_anchor;
            } else {
                anchor = target == edit::RangePointerTarget::Start ? base.end_us : base.start_us;
                start = anchor;
                end = at;
            }
        }
        setRangeGestureCandidate(start, end, range_gesture_tracks_, anchor);
    }
    emit rangeGestureChanged();
}

void EditSessionAdapter::selectAllClips() {
    if (!preflightPendingDraft())
        return;
    cancelRangeGesture();
    selection_.selectAll(*workspace_.snapshot());
    workspace_.select(selection_.primary_clip);
    publishSelection();
}

void EditSessionAdapter::clearSelection() {
    if (!preflightPendingDraft())
        return;
    cancelRangeGesture();
    selection_.clear();
    workspace_.select(0);
    publishSelection();
}

void EditSessionAdapter::setActiveTrack(qulonglong id) {
    if (!preflightPendingDraft())
        return;
    if (id && std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(),
                           [id](const auto& track) { return track.id == id; }))
        return;
    if (selection_.active_track == id)
        return;
    selection_.active_track = id;
    emit selectionChanged();
}

bool EditSessionAdapter::setEditRangeUs(qint64 start, qint64 end, const QVariantList& track_ids, qint64 anchor) {
    if (!preflightPendingDraft())
        return false;
    std::vector<edit::Id> tracks;
    for (const auto& value : track_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid) {
            refuseEdit(tr("Choose existing tracks for the range."));
            return false;
        }
        tracks.push_back(id);
    }
    const auto range = edit::MakeEditRange(*workspace_.snapshot(), start, end, tracks, anchor);
    if (!range) {
        refuseEdit(tr("The range needs distinct valid boundaries and at least one existing track."));
        return false;
    }
    if (selection_.mode == edit::SelectionMode::Range && selection_.range == range)
        return true;
    selection_.setRange(*range);
    workspace_.select(0);
    workspace_error_.clear();
    publishSelection();
    emit editRangeCommitted(range->start_us, range->end_us);
    return true;
}

void EditSessionAdapter::clearEditRange() {
    if (!preflightPendingDraft())
        return;
    if (selection_.mode == edit::SelectionMode::Range) {
        selection_.clear();
        workspace_.select(0);
        publishSelection();
    }
    emit editRangeCleared();
}

bool EditSessionAdapter::applyRangeGain(double delta) {
    if (!preflightPendingDraft())
        return false;
    if (selection_.mode != edit::SelectionMode::Range || !selection_.range)
        return false;
    const auto& range = *selection_.range;
    return applyCommand(edit::AdjustRangeGain{{range.start_us, range.end_us}, range.track_ids, delta});
}

QVariantMap EditSessionAdapter::parseCurveBiasValue(const QString& text) const {
    static const QRegularExpression syntax(QStringLiteral(R"(^[+-]?(?:[0-9]+(?:[.,][0-9]*)?|[.,][0-9]+)\s*%?$)"));
    auto number = text.trimmed();
    const auto invalid = [this] {
        return QVariantMap{{QStringLiteral("valid"), false},
                           {QStringLiteral("value"), 0.0},
                           {QStringLiteral("error"), tr("Enter a curve bias from -100% to +100%.")}};
    };
    if (!syntax.match(number).hasMatch())
        return invalid();
    if (number.endsWith(QLatin1Char('%')))
        number.chop(1);
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool parsed = false;
    const auto value = number.trimmed().toDouble(&parsed);
    if (!parsed || !std::isfinite(value) || value < -100 || value > 100)
        return invalid();
    return {
        {QStringLiteral("valid"), true}, {QStringLiteral("value"), value / 100}, {QStringLiteral("error"), QString()}};
}

bool EditSessionAdapter::setJunctionCurveFor(qulonglong id, int kind, double bias) {
    if (kind < static_cast<int>(engine::CurveKind::Linear) || kind > static_cast<int>(engine::CurveKind::Bias)) {
        refuseEdit(TranslateEditError({edit::EditErrorCode::InvalidCurve, {}, {}}));
        return false;
    }
    return applyCommand(edit::SetJunctionCurve{id, {static_cast<engine::CurveKind>(kind), bias}});
}

bool EditSessionAdapter::setClipFadeCurve(qulonglong id, bool incoming, int kind, double bias) {
    if (!preflightPendingDraft())
        return false;
    const auto* clip = workspace_.clip(id);
    if (!clip) {
        refuseEdit(TranslateEditError({edit::EditErrorCode::InvalidTarget, {id}, {}}));
        return false;
    }
    if (kind < static_cast<int>(engine::CurveKind::Linear) || kind > static_cast<int>(engine::CurveKind::Bias)) {
        refuseEdit(TranslateEditError({edit::EditErrorCode::InvalidCurve, {id}, {}}));
        return false;
    }
    auto fade = incoming ? clip->fade_in : clip->fade_out;
    fade.curve = {static_cast<engine::CurveKind>(kind), bias};
    return applyCommand(edit::SetFade{id, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, fade});
}

bool EditSessionAdapter::inspectClip(qulonglong id) {
    if (!preflightPendingDraft() || !workspace_.clip(id))
        return false;
    cancelClipDrag();
    cancelRangeGesture();
    finishPointerSelection(true);
    selection_.clear();
    selection_.selectClip(*workspace_.snapshot(), id, false, false, false);
    workspace_.select(id);
    publishSelection();
    return true;
}

QVariantMap EditSessionAdapter::occupiedBounds() const {
    auto start = (std::numeric_limits<edit::Time>::max)();
    edit::Time end = 0;
    for (const auto& clip : workspace_.clips()) {
        if (clip.duration() <= 0)
            continue;
        start = std::min(start, clip.start);
        end = std::max(end, clip.end());
    }
    return {{QStringLiteral("valid"), end > start},
            {QStringLiteral("startUs"), end > start ? start : 0},
            {QStringLiteral("endUs"), end > start ? end : 0}};
}

bool EditSessionAdapter::previewRemoveTimeOnAllTracks() {
    if (!preflightPendingDraft() || selection_.mode != edit::SelectionMode::Range || !selection_.range)
        return false;
    const auto range = *selection_.range;
    cancelClipDrag();
    cancelRangeGesture();
    drag_snapshot_ = workspace_.snapshot();
    drag_selection_ =
        GestureSelection{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_};
    std::vector<edit::Id> tracks;
    for (const auto& track : drag_snapshot_->tracks)
        tracks.push_back(track.id);
    drag_command_ = edit::RippleRemoveRange{{range.start_us, range.end_us}, std::move(tracks)};
    drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
    emit dragChanged();
    if (!dragValid()) {
        const auto reason = dragReason();
        cancelClipDrag();
        refuseEdit(reason);
        return false;
    }
    return true;
}

bool EditSessionAdapter::setTrackLocked(qulonglong id, bool locked) {
    if (!workspace_.setTrackLocked(id, locked))
        return false;
    cancelClipDrag();
    publishSelection();
    return true;
}

QVariantList EditSessionAdapter::media() const {
    return media_rows_;
}

void EditSessionAdapter::syncMedia(bool force) {
    if (!force && media_revision_ == workspace_.revision())
        return;
    media_revision_ = workspace_.revision();
    QVariantList rows;
    for (const auto& asset : workspace_.assets()) {
        const auto publication = source_publications_.find(asset.id);
        const bool verified = publication != source_publications_.end() && bool(publication->second.proof);
        QStringList facts;
        if (asset.duration > 0)
            facts.push_back(FormatTimelineClock(asset.duration / 1000, asset.duration / 1000));
        if (asset.width > 0 && asset.height > 0)
            facts.push_back(QStringLiteral("%1 x %2").arg(asset.width).arg(asset.height));
        else if (asset.audio)
            facts.push_back(tr("Audio"));
        if (asset.fps > 0)
            facts.push_back(tr("%1 fps").arg(asset.fps));
        rows.push_back(QVariantMap{
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(asset.id)},
            {QStringLiteral("name"), QString::fromStdString(asset.name)},
            {QStringLiteral("path"), QString::fromStdWString(asset.path)},
            {QStringLiteral("durationMs"), QVariant::fromValue<qint64>(asset.duration / 1000)},
            {QStringLiteral("available"),
             asset.state == edit::AssetState::Available && !sourceVerificationFailed(asset.id)},
            {QStringLiteral("verified"), verified},
            {QStringLiteral("audioChoiceRequired"), asset.mix_decision_required},
            {QStringLiteral("fileSize"), publication != source_publications_.end() && publication->second.metadata
                                             ? QVariant::fromValue<qulonglong>(publication->second.metadata->file_size)
                                             : QVariant()},
            {QStringLiteral("verifying"), publication != source_publications_.end() && publication->second.verifying},
            {QStringLiteral("verificationReason"),
             publication == source_publications_.end() ? QString() : publication->second.reason},
            {QStringLiteral("sourceIdentity"), sourcePreviewIdentity(asset.id)},
            {QStringLiteral("metadata"), facts.join(QStringLiteral(" / "))}});
    }
    if (rows == media_rows_)
        return;
    media_rows_ = std::move(rows);
    media_model_->setRows(media_rows_);
    emit mediaChanged();
}

std::shared_ptr<const engine::MediaReadCapability> EditSessionAdapter::sourceAccess(edit::Id asset) const {
    const auto found = source_publications_.find(asset);
    return found == source_publications_.end() ? nullptr : found->second.access;
}

std::shared_ptr<const engine::VerifiedMediaIdentity> EditSessionAdapter::sourceProof(edit::Id asset) const {
    const auto found = source_publications_.find(asset);
    return found == source_publications_.end() ? nullptr : found->second.proof;
}

std::shared_ptr<const engine::EditMediaMetadata> EditSessionAdapter::sourceMetadata(edit::Id asset) const {
    const auto found = source_publications_.find(asset);
    return found == source_publications_.end() ? nullptr : found->second.metadata;
}

QString EditSessionAdapter::sourcePreviewIdentity(edit::Id asset_id) const {
    const auto publication = source_publications_.find(asset_id);
    if (publication != source_publications_.end() && publication->second.access) {
        if (publication->second.failed)
            return {};
        if (publication->second.proof)
            return QString::fromStdString(engine::ToHex(publication->second.proof->Identity().sha256));
        const auto& access = publication->second.access;
        QByteArray recipe("lease:");
        recipe += QByteArray::number(access->Generation());
        recipe += ':';
        recipe += QByteArray::number(access->Identity().file_identity.volume_serial);
        recipe.append(reinterpret_cast<const char*>(access->Identity().file_identity.file_id.data()), 16);
        return QString::fromStdString(engine::ToHex(engine::Sha256(std::span(
            reinterpret_cast<const std::uint8_t*>(recipe.constData()), static_cast<std::size_t>(recipe.size())))));
    }
    const auto* asset = workspace_.asset(asset_id);
    return asset && asset->source_descriptor
               ? QString::fromStdString(engine::ToHex(asset->source_descriptor->asset_identity_fingerprint))
               : QString();
}

bool EditSessionAdapter::sourceVerificationFailed(edit::Id asset) const {
    const auto found = source_publications_.find(asset);
    return found != source_publications_.end() && found->second.failed;
}

QString EditSessionAdapter::sourceVerificationReason(edit::Id asset) const {
    const auto found = source_publications_.find(asset);
    return found == source_publications_.end() ? QString() : found->second.reason;
}

void EditSessionAdapter::revokePendingImports() {
    ++import_session_;
    import_cancelled_->store(true, std::memory_order_relaxed);
    import_cancelled_ = std::make_shared<std::atomic<bool>>(false);
}

void EditSessionAdapter::cancelPendingImports() {
    revokePendingImports();
}

QVariantList EditSessionAdapter::tracks() const {
    QVariantList rows;
    int video = 0;
    int audio = 0;
    for (const auto& track : workspace_.tracks()) {
        const bool is_video = track.type == edit::TrackType::Video;
        const auto type_name = QStringLiteral("%1%2")
                                   .arg(is_video ? QStringLiteral("V") : QStringLiteral("A"))
                                   .arg(is_video ? ++video : ++audio);
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(track.id)},
                        {QStringLiteral("name"), track.name.empty() ? type_name : QString::fromStdString(track.name)},
                        {QStringLiteral("typeName"), type_name},
                        {QStringLiteral("video"), is_video},
                        {QStringLiteral("enabled"), track.edit_enabled},
                        {QStringLiteral("gainDb"), track.gain_db},
                        {QStringLiteral("locked"), workspace_.trackLocked(track.id)},
                        {QStringLiteral("solo"), solo_tracks_.contains(track.id)}});
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
    for (const auto& clip : workspace_.clips()) {
        if (clip.end() < from_us || clip.start > to_us)
            continue;
        rows.push_back(projectClipRow(clip));
    }
    return rows;
}

QVariantList EditSessionAdapter::visibleClipsOnTracksUs(qint64 from_us, qint64 to_us, const QVariantList& track_ids,
                                                        const QVariantList& retained_clip_ids) const {
    QVariantList rows;
    std::unordered_set<edit::Id> tracks;
    tracks.reserve(static_cast<std::size_t>(track_ids.size()));
    for (const auto& value : track_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (valid && id)
            tracks.insert(id);
    }
    std::unordered_set<edit::Id> retained;
    retained.reserve(static_cast<std::size_t>(retained_clip_ids.size()));
    for (const auto& value : retained_clip_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (valid && id)
            retained.insert(id);
    }
    for (const auto& clip : workspace_.clips()) {
        const bool visible =
            from_us <= to_us && tracks.contains(clip.track) && clip.end() >= from_us && clip.start <= to_us;
        if (!visible && !retained.contains(clip.id))
            continue;
        rows.push_back(projectClipRow(clip));
    }
    return rows;
}

QVariantMap EditSessionAdapter::projectClipRow(const edit::Clip& clip) const {
    const auto* asset = workspace_.asset(clip.asset);
    const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                    [&clip](const auto& t) { return t.id == clip.track; });
    if (!asset || track == workspace_.tracks().end())
        return {};
    auto cache_stream_identity = clip.stream ? clip.stream->stream_identity : engine::AudioTrackId{};
    // Foreign track UUIDs bind one open, so persistent caches use only verified recording identities.
    if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio && asset->source_descriptor &&
        !asset->source_descriptor->audio_identities_verified)
        cache_stream_identity = {};
    uint32_t source_channels = 0;
    if (clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio && asset->source_descriptor) {
        const auto& streams = asset->source_descriptor->streams;
        const auto source_stream = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
            return stream.ref.kind == clip.stream->kind && stream.ref.container_index == clip.stream->container_index &&
                   stream.ref.stream_identity == clip.stream->stream_identity;
        });
        if (source_stream != streams.end())
            source_channels = source_stream->codec.channels;
    }
    const auto fade_in = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                      [&](const auto& junction) { return junction.incoming == clip.id; });
    const auto fade_out = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                       [&](const auto& junction) { return junction.outgoing == clip.id; });
    return QVariantMap{
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
        {QStringLiteral("gainDb"), clip.gain_db},
        {QStringLiteral("enabled"), clip.enabled},
        {QStringLiteral("fadeInCurve"), static_cast<int>(clip.fade_in.curve.kind)},
        {QStringLiteral("fadeInBias"), clip.fade_in.curve.bias},
        {QStringLiteral("fadeOutCurve"), static_cast<int>(clip.fade_out.curve.kind)},
        {QStringLiteral("fadeOutBias"), clip.fade_out.curve.bias},
        {QStringLiteral("fadeInOwned"), fade_in != workspace_.transitions().end()},
        {QStringLiteral("fadeOutOwned"), fade_out != workspace_.transitions().end()},
        {QStringLiteral("incomingJunctionOutgoing"),
         QVariant::fromValue<qulonglong>(fade_in == workspace_.transitions().end() ? 0 : fade_in->outgoing)},
        {QStringLiteral("outgoingJunctionOutgoing"),
         QVariant::fromValue<qulonglong>(fade_out == workspace_.transitions().end() ? 0 : fade_out->outgoing)},
        {QStringLiteral("fadeInUs"), QVariant::fromValue<qint64>(clip.fade_in.duration_us)},
        {QStringLiteral("fadeOutUs"), QVariant::fromValue<qint64>(clip.fade_out.duration_us)},
        {QStringLiteral("startMs"), QVariant::fromValue<qint64>(clip.start / 1000)},
        {QStringLiteral("durationMs"), QVariant::fromValue<qint64>(clip.duration() / 1000)},
        {QStringLiteral("inMs"), QVariant::fromValue<qint64>(clip.source_in / 1000)},
        {QStringLiteral("outMs"), QVariant::fromValue<qint64>(clip.source_out / 1000)},
        {QStringLiteral("available"),
         asset->state == edit::AssetState::Available && !sourceVerificationFailed(clip.asset)},
        {QStringLiteral("selected"),
         selection_.mode == edit::SelectionMode::Clips &&
             std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), clip.id) != selection_.clip_ids.end()},
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
        {QStringLiteral("sourceIdentity"), sourcePreviewIdentity(clip.asset)},
        {QStringLiteral("streamIdentity"),
         clip.stream ? QString::fromLatin1(
                           QByteArray(reinterpret_cast<const char*>(cache_stream_identity.bytes.data()), 16).toHex())
                     : QString()}};
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

void EditSessionAdapter::publishWorkspace(bool changed, const QString& error) {
    if (changed)
        syncSourceVersions();
    ++edit_outcome_sequence_;
    last_edit_accepted_ = changed;
    if (range_gesture_snapshot_ && range_gesture_snapshot_->revision != workspace_.revision())
        cancelRangeGesture();
    if (drag_snapshot_ && drag_snapshot_->revision != workspace_.revision())
        cancelClipDrag();
    if (changed)
        selected_gap_track_ = 0;
    workspace_error_ = changed           ? QString()
                       : error.isEmpty() ? tr("The edit would overlap another clip or exceed the source.")
                                         : error;
    duration_ms_ = workspace_.duration() / 1000;
    position_ms_ = workspace_.playhead() / 1000;
    const bool open = workspace_.duration() > 0;
    if (open_ != open) {
        open_ = open;
        emit openChanged();
    }
    syncSelection();
    emit workspaceChanged();
    emit durationChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    emit dragChanged();
}

void EditSessionAdapter::selectClip(qulonglong id) {
    if (!id)
        clearSelection();
    else
        selectClipWithModifiers(id);
}

QVariantList EditSessionAdapter::transitions() const {
    return visibleTransitionsUs(0, (std::numeric_limits<qint64>::max)(), {}, false);
}

QVariantList EditSessionAdapter::visibleTransitionsUs(qint64 from, qint64 to, const QVariantList& track_ids,
                                                      bool include_envelope, const QVariantList& retained_ids) const {
    QVariantList result;
    if (from < 0 || to <= from || workspace_.transitions().empty())
        return result;
    std::set<edit::Id> scope;
    for (const auto& value : track_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid)
            return {};
        scope.insert(id);
    }
    std::set<edit::Id> retained;
    for (const auto& value : retained_ids) {
        bool valid = false;
        const auto id = value.toULongLong(&valid);
        if (!valid)
            return {};
        retained.insert(id);
    }
    std::unordered_map<edit::Id, const edit::Clip*> clips;
    clips.reserve(workspace_.clips().size());
    for (const auto& clip : workspace_.clips())
        clips.emplace(clip.id, &clip);
    std::unordered_map<edit::Id, std::pair<int, bool>> tracks;
    for (size_t index = 0; index < workspace_.tracks().size(); ++index) {
        const auto& track = workspace_.tracks()[index];
        tracks.emplace(track.id, std::pair{static_cast<int>(index), track.type == edit::TrackType::Video});
    }
    for (const auto& t : workspace_.transitions()) {
        const bool captured = retained.contains(t.id);
        if (!captured && !scope.empty() && !scope.contains(t.track))
            continue;
        const auto outgoing = clips.find(t.outgoing), incoming = clips.find(t.incoming);
        const auto track = tracks.find(t.track);
        if (outgoing == clips.end() || incoming == clips.end() || track == tracks.end())
            continue;
        const auto overlap_start = std::max(outgoing->second->start, incoming->second->start);
        const auto overlap_end = std::min(outgoing->second->end(), incoming->second->end());
        const bool outside = overlap_start == overlap_end ? overlap_start < from || overlap_start >= to
                                                          : overlap_end <= from || overlap_start >= to;
        if (overlap_start > overlap_end || (!captured && outside))
            continue;
        result.push_back(QVariantMap{
            {QStringLiteral("outgoing"), QVariant::fromValue<qulonglong>(t.outgoing)},
            {QStringLiteral("incoming"), QVariant::fromValue<qulonglong>(t.incoming)},
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(t.id)},
            {QStringLiteral("trackIndex"), track->second.first},
            {QStringLiteral("video"), track->second.second},
            {QStringLiteral("startMs"), t.in_us / 1000},
            {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(t.track)},
            {QStringLiteral("startUs"), t.in_us},
            {QStringLiteral("endUs"), t.out_us},
            {QStringLiteral("overlapStartUs"), overlap_start},
            {QStringLiteral("overlapEndUs"), overlap_end},
            {QStringLiteral("outgoingStartUs"), outgoing->second->start},
            {QStringLiteral("outgoingEndUs"), outgoing->second->end()},
            {QStringLiteral("incomingStartUs"), incoming->second->start},
            {QStringLiteral("incomingEndUs"), incoming->second->end()},
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
            {QStringLiteral("envelope"),
             !include_envelope || t.kind == edit::TransitionKind::HardCut
                 ? QVariantMap{}
                 : JunctionEnvelope(t, *outgoing->second, *incoming->second, !track->second.second, 0, 1, 64)},
            {QStringLiteral("selected"), selected_transition_ == t.outgoing}});
    }
    return result;
}

QVariantMap EditSessionAdapter::junctionEnvelopePoints(qulonglong id, qreal from, qreal to, int count) const {
    const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                    [id](const auto& junction) { return junction.id == id; });
    if (found == workspace_.transitions().end())
        return {};
    const auto outgoing = workspace_.clip(found->outgoing), incoming = workspace_.clip(found->incoming);
    const auto track = std::find_if(workspace_.tracks().begin(), workspace_.tracks().end(),
                                    [&](const auto& value) { return value.id == found->track; });
    return outgoing && incoming && track != workspace_.tracks().end()
               ? JunctionEnvelope(*found, *outgoing, *incoming, track->type == edit::TrackType::Audio, from, to, count)
               : QVariantMap{};
}

void EditSessionAdapter::selectTransition(qulonglong outgoing) {
    if (!preflightPendingDraft())
        return;
    selected_spacer_ = 0;
    selected_gap_track_ = 0;
    const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                    [outgoing](const auto& t) { return t.outgoing == outgoing; });
    selected_transition_ = found == workspace_.transitions().end() ? 0 : outgoing;
    selection_.clear();
    if (selected_transition_)
        selection_.mode = edit::SelectionMode::Junction;
    workspace_.select(0);
    publishSelection();
}

bool EditSessionAdapter::canCrossfade(qulonglong outgoing) const {
    const auto* a = workspace_.clip(outgoing ? outgoing : workspace_.selection());
    if (!a)
        return false;
    for (const auto& b : workspace_.clips())
        if (b.track == a->track && b.start > a->start)
            return workspace_.canCrossfade(a->id, b.id);
    return false;
}

bool EditSessionAdapter::previewCrossfadeAtCut(qulonglong outgoing, qint64 duration) {
    if (!preflightPendingDraft())
        return false;
    const auto snapshot = workspace_.snapshot();
    const auto command =
        AdjacentCutCommand(*snapshot, outgoing ? outgoing : workspace_.selection(), duration, linkedSelection);
    if (!command) {
        refuseEdit(tr("Choose a clip followed by an adjacent clip at the cut."));
        return false;
    }
    cancelClipDrag();
    cancelRangeGesture();
    drag_snapshot_ = snapshot;
    drag_selection_ =
        GestureSelection{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_};
    drag_clip_ = command->outgoing;
    drag_command_ = *command;
    drag_preview_ = edit::PreviewEdit(*snapshot, *command);
    emit dragChanged();
    if (!dragValid()) {
        const auto reason = dragReason();
        cancelClipDrag();
        refuseEdit(reason);
        return false;
    }
    return true;
}

bool EditSessionAdapter::previewRemoveOverlap(qulonglong junction_id) {
    if (!preflightPendingDraft())
        return false;
    const auto snapshot = workspace_.snapshot();
    const auto junction = std::find_if(snapshot->junctions.begin(), snapshot->junctions.end(), [&](const auto& value) {
        return junction_id ? value.id == junction_id : value.outgoing == selected_transition_;
    });
    if (junction == snapshot->junctions.end()) {
        refuseEdit(TranslateEditError({edit::EditErrorCode::InvalidTarget, {}, {}}));
        return false;
    }
    cancelClipDrag();
    cancelRangeGesture();
    drag_snapshot_ = snapshot;
    drag_selection_ =
        GestureSelection{selection_, selected_transition_, selected_spacer_, selected_gap_track_, selected_gap_};
    drag_clip_ = junction->outgoing;
    drag_targets_ =
        edit::ExpandClipTargetIds(snapshot->clips, {junction->outgoing, junction->incoming}, linkedSelection);
    drag_command_ = edit::RemoveOverlap{junction->id, linkedSelection};
    drag_preview_ = edit::PreviewEdit(*snapshot, *drag_command_);
    emit dragChanged();
    if (!dragValid()) {
        const auto reason = dragReason();
        cancelClipDrag();
        refuseEdit(reason);
        return false;
    }
    return true;
}

bool EditSessionAdapter::canCrossfadeAtCut(qulonglong outgoing) const {
    const auto snapshot = workspace_.snapshot();
    const auto command =
        AdjacentCutCommand(*snapshot, outgoing ? outgoing : workspace_.selection(), 500'000, linkedSelection);
    if (!command)
        return false;
    const auto targets =
        edit::ExpandClipTargetIds(snapshot->clips, {command->outgoing, command->incoming}, command->expand_linked);
    for (const auto& clip : snapshot->clips)
        if (std::find(targets.begin(), targets.end(), clip.id) != targets.end() && sourceVerificationFailed(clip.asset))
            return false;
    return edit::PreviewEdit(*snapshot, *command).valid;
}

bool EditSessionAdapter::applyCrossfade(qulonglong outgoing, qint64 duration_ms) {
    if (!preflightPendingDraft())
        return false;
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
    const auto snapshot = workspace_.snapshot();
    edit::Id selected = 0;
    auto distance = (std::numeric_limits<edit::Time>::max)();
    for (const auto& clip : snapshot->clips) {
        const auto candidate_distance = std::abs(clip.end() - at_us);
        if (candidate_distance <= tolerance_us &&
            (candidate_distance < distance || (candidate_distance == distance && (!selected || clip.id < selected))) &&
            AdjacentCutCommand(*snapshot, clip.id, 500'000, linkedSelection)) {
            selected = clip.id;
            distance = candidate_distance;
        }
    }
    if (selected)
        return previewCrossfadeAtCut(selected);
    refuseEdit(tr("Choose an adjacent cut with enough source handles for a crossfade."));
    return false;
}

void EditSessionAdapter::removeTransition() {
    if (!preflightPendingDraft())
        return;
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
    importMediaWithRevision(urls, append, at_us, workspace_.revision());
}

void EditSessionAdapter::importMediaWithRevision(const QList<QUrl>& urls, bool append, qint64 at_us,
                                                 edit::Revision revision) {
    QPointer<EditSessionAdapter> self(this);
    const auto cancelled = import_cancelled_;
    const auto session = import_session_;
    const auto placement = at_us < 0 ? workspace_.duration() : at_us;
    const auto crossfade = autoCrossfade;
    const auto admission = background_io_;
    beginImport();
    metadata_pool_.start([self, urls, append, placement, crossfade, cancelled, session, revision, admission]() {
        struct Candidate {
            QString path;
            std::shared_ptr<const engine::MediaReadCapability> access;
            engine::EditMediaMetadata metadata;
        };
        std::vector<Candidate> candidates;
        QStringList failures;
        for (const auto& url : urls) {
            if (cancelled->load(std::memory_order_relaxed))
                break;
            if (!url.isLocalFile()) {
                failures.push_back(
                    EditSessionAdapter::tr("%1: only local media files are supported.").arg(url.toDisplayString()));
                continue;
            }
            const QString path = QFileInfo(url.toLocalFile()).absoluteFilePath();
            const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
            if (!acquired.succeeded()) {
                failures.push_back(EditSessionAdapter::tr("%1: %2").arg(
                    QFileInfo(path).fileName(), QString::fromStdString(acquired.error()->context)));
                continue;
            }
            auto access = *acquired.value();
            auto metadata = engine::ProbeEditMedia(access, {}, admission, cancelled.get(), false);
            if (!metadata.error.empty() || metadata.duration_us <= 0) {
                failures.push_back(EditSessionAdapter::tr("%1: %2").arg(
                    QFileInfo(path).fileName(), metadata.error.empty()
                                                    ? EditSessionAdapter::tr("No supported timed media stream.")
                                                    : QString::fromStdString(metadata.error)));
                continue;
            }
            candidates.push_back({path, std::move(access), std::move(metadata)});
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, candidates = std::move(candidates), failures = std::move(failures), append, placement, crossfade,
             cancelled, session, revision]() mutable {
                if (!self)
                    return;
                if (cancelled->load(std::memory_order_relaxed) || session != self->import_session_) {
                    self->endImport();
                    return;
                }
                const bool placement_current = self->workspace_.revision() == revision;
                std::vector<edit::Id> ids;
                for (auto& candidate : candidates) {
                    for (const auto& [id, prior] : self->source_publications_)
                        if (prior.access == candidate.access && prior.proof && prior.metadata) {
                            candidate.metadata = *prior.metadata;
                            break;
                        }
                    auto asset = AssetFromProbe(candidate.path, candidate.metadata, candidate.access);
                    const auto id = self->workspace_.addAsset(std::move(asset));
                    if (!id)
                        continue;
                    ids.push_back(id);
                    auto& publication = self->source_publications_[id];
                    publication.version = self->workspace_.asset(id)->source_version;
                    if (publication.access != candidate.access || !publication.proof) {
                        publication = {std::move(candidate.access),
                                       {},
                                       std::make_shared<const engine::EditMediaMetadata>(std::move(candidate.metadata)),
                                       {},
                                       false};
                    }
                }
                self->workspace_error_.clear();
                if (append && placement_current && !ids.empty()) {
                    self->publishWorkspace(self->workspace_.insertAssets(ids, placement, crossfade));
                } else if (append && !ids.empty()) {
                    for (const auto id : ids)
                        if (std::find(self->pending_placement_assets_.begin(), self->pending_placement_assets_.end(),
                                      id) == self->pending_placement_assets_.end())
                            self->pending_placement_assets_.push_back(id);
                    emit self->pendingPlacementChanged();
                    self->workspace_error_ =
                        EditSessionAdapter::tr("The timeline changed during import. Media was kept in Project. Choose "
                                               "Place imported media to place it using the current timeline.");
                    emit self->workspaceChanged();
                } else {
                    emit self->workspaceChanged();
                }
                if (!failures.empty()) {
                    const auto detail = failures.join(QLatin1Char('\n'));
                    if (ids.empty())
                        self->refuseEdit(EditSessionAdapter::tr("Cannot import media: %1").arg(detail));
                    else {
                        self->workspace_error_ =
                            EditSessionAdapter::tr("Imported %1 file(s). Some files could not be imported:\n%2")
                                .arg(ids.size())
                                .arg(detail);
                        emit self->workspaceChanged();
                    }
                }
                for (const auto id : ids)
                    if (!self->sourceProof(id))
                        self->retrySourceVerification(id);
                self->endImport();
            },
            Qt::QueuedConnection);
    });
}

bool EditSessionAdapter::placeImportedMedia(qint64 at_us, bool auto_crossfade) {
    if (pending_placement_assets_.empty())
        return false;
    if (at_us < -1) {
        refuseEdit(tr("The placement time is invalid."));
        return false;
    }
    const auto placement = at_us < 0 ? workspace_.duration() : at_us;
    const bool accepted = workspace_.insertAssets(pending_placement_assets_, placement, auto_crossfade);
    publishWorkspace(accepted);
    if (accepted) {
        pending_placement_assets_.clear();
        emit pendingPlacementChanged();
    }
    return accepted;
}

void EditSessionAdapter::routeMediaDrop(const QList<QUrl>& urls, bool append, qint64 at_us) {
    if (at_us < -1) {
        refuseEdit(tr("The placement time is invalid."));
        return;
    }
    const auto request = ++drop_sequence_;
    pending_media_drop_ = {};
    const auto revision = workspace_.revision();
    const auto placement = at_us < 0 ? workspace_.duration() : at_us;
    const auto session = import_session_;
    const auto cancelled = import_cancelled_;
    QPointer<EditSessionAdapter> self(this);
    beginImport();
    metadata_pool_.start([self, urls, append, placement, session, revision, cancelled, request] {
        PendingMediaDrop pending;
        pending.request_id = request;
        pending.session = session;
        pending.append = append;
        pending.at_us = placement;
        QStringList errors;
        for (const auto& url : urls) {
            if (cancelled->load(std::memory_order_relaxed))
                break;
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir()) {
                pending.roots.push_back(url);
                QString reason;
                const auto files = EditFolderModel::DirectoryCandidates(url, cancelled, reason);
                pending.all_files.append(files);
                if (!reason.isEmpty())
                    errors.push_back(tr("%1: %2").arg(QFileInfo(url.toLocalFile()).fileName(), reason));
            } else {
                pending.direct_files.push_back(url);
                pending.all_files.push_back(url);
            }
        }
        if (!self)
            return;
        QMetaObject::invokeMethod(
            self,
            [self, pending = std::move(pending), cancelled, revision,
             reason = errors.join(QLatin1Char('\n'))]() mutable {
                if (!self)
                    return;
                self->endImport();
                if (cancelled->load(std::memory_order_relaxed) || pending.session != self->import_session_ ||
                    pending.request_id != self->drop_sequence_)
                    return;
                if (pending.roots.empty()) {
                    self->importMediaWithRevision(pending.direct_files, pending.append, pending.at_us, revision);
                    return;
                }
                QVariantList roots;
                QVariantList candidates;
                for (const auto& url : pending.roots)
                    roots.push_back(url);
                for (const auto& url : pending.all_files)
                    candidates.push_back(url);
                self->pending_media_drop_ = std::move(pending);
                emit self->directoryDropRequested(self->pending_media_drop_.request_id, roots, candidates,
                                                  self->pending_media_drop_.at_us, reason);
            },
            Qt::QueuedConnection);
    });
}

void EditSessionAdapter::resolveMediaDrop(qulonglong request_id, bool insert_directories) {
    if (!request_id || pending_media_drop_.request_id != request_id || pending_media_drop_.session != import_session_)
        return;
    auto pending = std::exchange(pending_media_drop_, {});
    if (!insert_directories)
        for (const auto& url : pending.roots)
            folder_model_->addRoot(url);
    const auto& files = insert_directories ? pending.all_files : pending.direct_files;
    if (!files.empty())
        importMediaBatchUs(files, pending.append, pending.at_us);
}

void EditSessionAdapter::cancelMediaDrop(qulonglong request_id) {
    if (pending_media_drop_.request_id == request_id)
        pending_media_drop_ = {};
}

void EditSessionAdapter::retrySourceVerification(qulonglong asset_id) {
    if (source_verification_cancelled_->load(std::memory_order_relaxed))
        return;
    const auto found = source_publications_.find(asset_id);
    const auto* asset = workspace_.asset(asset_id);
    if (found == source_publications_.end() || !found->second.access || found->second.verifying || !asset)
        return;
    auto& publication = found->second;
    if (publication.proof)
        return;
    std::optional<std::array<std::uint8_t, 32>> expected;
    if (publication.metadata)
        expected = publication.metadata->audio_metadata.expected_content_sha256;
    if (asset->source_descriptor) {
        const auto digest = asset->source_descriptor->asset_identity_fingerprint;
        if (std::any_of(digest.begin(), digest.end(), [](auto byte) { return byte != 0; }))
            expected = digest;
    }
    publication.verifying = true;
    publication.failed = false;
    publication.reason.clear();
    const auto access = publication.access;
    const auto version = asset->source_version;
    const auto cancelled = source_cancelled_;
    const auto verification_cancelled = source_verification_cancelled_;
    QPointer<EditSessionAdapter> self(this);
    syncMedia(true);
    emit sourceVerificationChanged(asset_id);
    const auto admission = optionalIoAdmission();
    scheduleSourceVerification([self, asset_id, version, access, cancelled, verification_cancelled, expected,
                                admission] {
        if (cancelled->load(std::memory_order_relaxed) || verification_cancelled->load(std::memory_order_relaxed))
            return;
        const auto result = engine::VerifyMediaIdentity(access, expected, {}, admission, verification_cancelled.get());
        std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
        std::shared_ptr<const engine::EditMediaMetadata> verified_metadata;
        QString reason;
        bool failed = false;
        if (result.succeeded()) {
            proof = *result.value();
            auto metadata = engine::ProbeEditMedia(access, {}, admission, verification_cancelled.get(), true);
            if (metadata.error.empty())
                verified_metadata = std::make_shared<const engine::EditMediaMetadata>(std::move(metadata));
            else {
                reason = QString::fromStdString(metadata.error);
                proof.reset();
                failed = true;
            }
        } else {
            reason = QString::fromStdString(result.error()->context);
            failed = result.error()->code != engine::EngineErrorCode::Cancelled;
        }
        if (verification_cancelled->load(std::memory_order_relaxed)) {
            proof.reset();
            verified_metadata.reset();
            reason.clear();
            failed = false;
        }
        if (self && !cancelled->load(std::memory_order_relaxed))
            QMetaObject::invokeMethod(
                self,
                [self, asset_id, version, access, cancelled, verification_cancelled, proof = std::move(proof),
                 verified_metadata = std::move(verified_metadata), reason, failed]() mutable {
                    if (!self || cancelled != self->source_cancelled_ ||
                        verification_cancelled != self->source_verification_cancelled_ ||
                        cancelled->load(std::memory_order_relaxed))
                        return;
                    const auto current = self->source_publications_.find(asset_id);
                    const auto* source = self->workspace_.asset(asset_id);
                    if (!source || source->source_version != version || current == self->source_publications_.end() ||
                        current->second.version != version || current->second.access != access)
                        return;
                    auto& publication = current->second;
                    publication.verifying = false;
                    publication.reason = reason;
                    publication.failed = failed;
                    if (proof && self->workspace_.establishAssetIdentity(asset_id, proof->Identity().sha256)) {
                        publication.metadata = std::move(verified_metadata);
                        publication.proof = std::move(proof);
                    } else if (proof) {
                        publication.reason =
                            EditSessionAdapter::tr("The source identity differs from the expected media.");
                        publication.failed = true;
                    }
                    bool drag_finalized = false;
                    bool range_finalized = false;
                    if (publication.proof && publication.metadata &&
                        publication.metadata->audio_metadata.status == engine::AudioMetadataStatus::Verified &&
                        publication.metadata->audio_metadata.manifest &&
                        !publication.metadata->audio_metadata.monitoring_confirmation_required) {
                        const auto imported =
                            edit::DescribeSourceImport(*publication.metadata, source->duration, access);
                        if (imported.descriptor && imported.input_mix && !imported.mix_decision_required &&
                            imported.input_mix->authority == edit::InputMixAuthority::VerifiedRecordedDefault &&
                            self->workspace_.establishRecordedDefault(asset_id, version, *imported.input_mix,
                                                                      imported.descriptor)) {
                            const auto finalize_snapshot = [&](auto& captured) {
                                if (!captured || captured->revision != self->workspace_.revision())
                                    return false;
                                auto updated = std::make_shared<edit::EditSnapshot>(*captured);
                                if (!FinalizeCapturedSourceDefault(*updated, asset_id, version, *imported.input_mix,
                                                                   imported.descriptor))
                                    return false;
                                captured = std::move(updated);
                                return true;
                            };
                            drag_finalized = finalize_snapshot(self->drag_snapshot_);
                            range_finalized = finalize_snapshot(self->range_gesture_snapshot_);
                            const auto finalize_clipboard = [&](auto& clipboard) {
                                if (!clipboard)
                                    return false;
                                auto updated = std::make_shared<edit::ClipClipboard>(*clipboard);
                                if (!FinalizeCapturedSourceDefault(*updated, asset_id, version, *imported.input_mix,
                                                                   imported.descriptor))
                                    return false;
                                clipboard = std::move(updated);
                                return true;
                            };
                            finalize_clipboard(self->clip_clipboard_);
                            if (self->drag_command_)
                                if (auto* paste = std::get_if<edit::PasteClips>(&*self->drag_command_))
                                    drag_finalized = finalize_clipboard(paste->clipboard) || drag_finalized;
                            if (drag_finalized && self->drag_snapshot_ && self->drag_command_)
                                self->drag_preview_ = edit::PreviewEdit(*self->drag_snapshot_, *self->drag_command_);
                            self->syncSourceVersions();
                        }
                    }
                    self->syncMedia(true);
                    if (drag_finalized)
                        emit self->dragChanged();
                    if (range_finalized)
                        emit self->rangeGestureChanged();
                    emit self->sourceVerificationChanged(asset_id);
                    emit self->workspaceChanged();
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
    if (!preflightPendingDraft())
        return;
    edit::Time at = std::max<qint64>(0, at_us);
    if (snap)
        at = workspace_.snap(at, id, 100000);
    const auto* clip = workspace_.clip(id);
    if (!clip)
        return;
    edit::Move command{{id}, at - clip->start};
    if (selection_.mode == edit::SelectionMode::Clips &&
        std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), id) != selection_.clip_ids.end())
        command.targets = selection_.clip_ids;
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
    if (!preflightPendingDraft())
        return;
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
    if (selection_.mode == edit::SelectionMode::Clips &&
        std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), id) != selection_.clip_ids.end())
        command.targets = selection_.clip_ids;
    command.mode = ripple ? edit::TrimMode::Ripple : edit::TrimMode::Fixed;
    command.expand_linked = linkedSelection;
    command.auto_crossfade = autoCrossfade;
    applyCommand(command);
}

void EditSessionAdapter::splitSelected() {
    if (!preflightPendingDraft())
        return;
    if (selection_.mode == edit::SelectionMode::Range && selection_.range) {
        const auto range = *selection_.range;
        applyCommand(edit::SplitRange{{range.start_us, range.end_us}, range.track_ids}, [this, range] {
            selection_.clear();
            const auto snapshot = workspace_.snapshot();
            for (const auto id : edit::OrderedClipIds(*snapshot)) {
                const auto* clip = workspace_.clip(id);
                if (clip &&
                    std::find(range.track_ids.begin(), range.track_ids.end(), clip->track) != range.track_ids.end() &&
                    clip->start >= range.start_us && clip->end() <= range.end_us)
                    selection_.clip_ids.push_back(id);
            }
            if (!selection_.clip_ids.empty()) {
                selection_.mode = edit::SelectionMode::Clips;
                selection_.primary_clip = selection_.anchor_clip = selection_.clip_ids.front();
            }
            workspace_.select(selection_.primary_clip);
        });
        return;
    }
    auto targets = selection_.mode == edit::SelectionMode::Clips ? selection_.clip_ids : std::vector<edit::Id>{};
    if (targets.empty())
        for (const auto& clip : workspace_.clips())
            if ((!selection_.active_track || clip.track == selection_.active_track) &&
                clip.start < workspace_.playhead() && workspace_.playhead() < clip.end())
                targets.push_back(clip.id);
    if (!targets.empty())
        applyCommand(edit::Split{targets, workspace_.playhead(), linkedSelection});
}

void EditSessionAdapter::deleteSelected(bool ripple_delete) {
    if (!preflightPendingDraft())
        return;
    if (selection_.mode == edit::SelectionMode::Range && selection_.range) {
        const auto range = *selection_.range;
        if (ripple_delete) {
            bool applied = false;
            applyCommand(edit::RippleRemoveRange{{range.start_us, range.end_us}, range.track_ids},
                         [this, range, &applied] {
                             selection_.clear();
                             workspace_.select(0);
                             workspace_.seek(range.start_us);
                             applied = true;
                         });
            if (applied)
                requestSeekUs(workspace_.playhead());
        } else {
            applyCommand(edit::LiftRange{{range.start_us, range.end_us}, range.track_ids});
        }
        return;
    }
    if (selection_.mode == edit::SelectionMode::Junction) {
        removeTransition();
        return;
    }
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
    if (selection_.mode != edit::SelectionMode::Clips || selection_.clip_ids.empty())
        return;
    if (ripple_delete)
        applyCommand(edit::RippleRemoveClips{selection_.clip_ids, linkedSelection});
    else
        applyCommand(edit::LiftClips{selection_.clip_ids, linkedSelection});
}
void EditSessionAdapter::undo() {
    if (!preflightPendingDraft())
        return;
    if (workspace_.undo())
        publishWorkspace(true);
}
void EditSessionAdapter::redo() {
    if (!preflightPendingDraft())
        return;
    if (workspace_.redo())
        publishWorkspace(true);
}

void EditSessionAdapter::selectAdjacentClip(int direction) {
    if (!preflightPendingDraft())
        return;
    const auto clips = edit::OrderedClipIds(*workspace_.snapshot());
    if (clips.empty() || !direction)
        return;
    const auto current = std::find(clips.begin(), clips.end(), workspace_.selection());
    const auto index = current == clips.end() ? (direction < 0 ? static_cast<int>(clips.size()) - 1 : 0)
                                              : std::clamp(static_cast<int>(current - clips.begin()) + direction, 0,
                                                           static_cast<int>(clips.size()) - 1);
    selection_.clear();
    workspace_.select(0);
    selectClip(clips[static_cast<size_t>(index)]);
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

bool EditSessionAdapter::applyCommand(const edit::EditCommand& command, std::function<void()> after_apply) {
    if (rangeScopeReviewActive()) {
        refuseEdit(tr("Apply or cancel the range scope preview first."));
        return false;
    }
    if (!preflightPendingDraft())
        return false;
    cancelRangeGesture();
    cancelClipDrag();
    QElapsedTimer clock;
    clock.start();
    const auto result = workspace_.apply(command, workspace_.revision());
    command_model_ns_ = clock.nsecsElapsed();
    clock.restart();
    if (result.status == edit::CommandStatus::Applied && after_apply)
        after_apply();
    publishWorkspace(result.status != edit::CommandStatus::Rejected,
                     result.error ? TranslateEditError(*result.error) : QString());
    command_publish_ns_ = clock.nsecsElapsed();
    return result.status != edit::CommandStatus::Rejected;
}

void EditSessionAdapter::beginClipDrag(qulonglong id, int edge, bool unlink_member) {
    if (!preflightPendingDraft())
        return;
    cancelClipDrag();
    if (!workspace_.clip(id))
        return;
    cancelRangeGesture();
    if (pointer_selection_ && pointer_selection_->revision != workspace_.revision()) {
        finishPointerSelection(true);
        return;
    }
    const auto captured = pointer_selection_;
    drag_selection_ = captured ? captured->original
                               : GestureSelection{selection_, selected_transition_, selected_spacer_,
                                                  selected_gap_track_, selected_gap_};
    pointer_selection_.reset();
    drag_targets_ =
        !unlink_member && selection_.mode == edit::SelectionMode::Clips &&
                std::find(selection_.clip_ids.begin(), selection_.clip_ids.end(), id) != selection_.clip_ids.end()
            ? selection_.clip_ids
            : std::vector<edit::Id>{id};
    std::erase(drag_targets_, id);
    drag_targets_.insert(drag_targets_.begin(), id);
    drag_expand_linked_ = (captured ? captured->linked : linkedSelection) && !unlink_member;
    drag_snapping_ = captured ? captured->snapping : snapping;
    drag_ripple_ = captured ? captured->ripple : ripple;
    drag_auto_crossfade_ = captured ? captured->auto_crossfade : autoCrossfade;
    drag_loop_snap_bounds_ = captured ? captured->loop_bounds : loop_snap_bounds_;
    drag_range_snap_bounds_ = captured ? captured->range_bounds
                              : selection_.range
                                  ? std::optional(edit::Span{selection_.range->start_us, selection_.range->end_us})
                                  : std::nullopt;
    drag_snap_marker_.reset();
    selected_gap_track_ = 0;
    drag_snapshot_ = workspace_.snapshot();
    drag_clip_ = id;
    drag_playhead_ = captured ? captured->playhead : workspace_.playhead();
    drag_operation_targets_ = edit::ExpandClipTargetIds(drag_snapshot_->clips, drag_targets_, drag_expand_linked_);
    drag_audio_grid_ =
        edit::ClipTargetsUseSamples(drag_snapshot_->clips, drag_snapshot_->tracks, drag_operation_targets_);
    drag_domain_end_ = edit::SnapshotDuration(*drag_snapshot_);
    drag_edge_ = edge;
    updateClipDrag(0);
}

void EditSessionAdapter::beginJunctionDrag(qulonglong outgoing, int edge) {
    if (!preflightPendingDraft())
        return;
    cancelClipDrag();
    const auto found = std::find_if(workspace_.transitions().begin(), workspace_.transitions().end(),
                                    [outgoing](const auto& junction) { return junction.outgoing == outgoing; });
    if (found == workspace_.transitions().end())
        return;
    if (pointer_selection_ && pointer_selection_->revision != workspace_.revision()) {
        finishPointerSelection(true);
        return;
    }
    drag_selection_ = pointer_selection_ ? pointer_selection_->original
                                         : GestureSelection{selection_, selected_transition_, selected_spacer_,
                                                            selected_gap_track_, selected_gap_};
    pointer_selection_.reset();
    selected_gap_track_ = 0;
    drag_snapshot_ = workspace_.snapshot();
    drag_clip_ = found->outgoing;
    drag_targets_ = {found->outgoing, found->incoming};
    drag_junction_ = found->id;
    drag_edge_ = edge;
    updateClipDrag(0);
}

void EditSessionAdapter::beginSpacerDrag(qulonglong id, int edge) {
    if (!preflightPendingDraft())
        return;
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
        if (drag_edge_ == 2) {
            const auto cut = AddTimelineTime(junction->cut_us, delta);
            if (!cut) {
                reject();
                return;
            }
            drag_command_ = edit::SetHardCut{junction->id, *cut};
            drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
            emit dragChanged();
            return;
        }
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
        auto fade = incoming ? lead->fade_in : lead->fade_out;
        fade.duration_us = std::max<edit::Time>(0, *duration);
        drag_command_ = edit::SetFade{lead->id, incoming ? edit::FadeEdge::In : edit::FadeEdge::Out, fade};
        drag_preview_ = edit::PreviewEdit(*drag_snapshot_, *drag_command_);
        emit dragChanged();
        return;
    }
    if (drag_snapping_) {
        const auto boundary = drag_edge_ == 1 ? lead->end() : lead->start;
        const auto requested = AddTimelineTime(boundary, delta);
        if (!requested) {
            reject();
            return;
        }
        const auto tolerance = static_cast<uint64_t>(std::max<qint64>(0, tolerance_us));
        const auto distance_to = [&](edit::Time marker) {
            return marker >= *requested ? static_cast<uint64_t>(marker) - static_cast<uint64_t>(*requested)
                                        : static_cast<uint64_t>(*requested) - static_cast<uint64_t>(marker);
        };
        if (!drag_snap_marker_ || distance_to(*drag_snap_marker_) > tolerance + tolerance / 3) {
            drag_snap_marker_.reset();
            auto best = (std::numeric_limits<uint64_t>::max)();
            int best_priority = (std::numeric_limits<int>::max)();
            const auto consider = [&](edit::Time raw, int priority) {
                if (raw < 0 || raw > drag_domain_end_)
                    return;
                const auto marker = drag_audio_grid_ ? edit::AudioSampleGrid::nearestSample(raw)
                                                     : drag_snapshot_->output_fps.nearestFrame(raw);
                if (!marker || *marker < 0 || *marker > drag_domain_end_)
                    return;
                const auto distance = distance_to(*marker);
                if (distance <= tolerance &&
                    (distance < best ||
                     (distance == best &&
                      (priority < best_priority ||
                       (priority == best_priority && (!drag_snap_marker_ || *marker < *drag_snap_marker_)))))) {
                    best = distance;
                    best_priority = priority;
                    drag_snap_marker_ = *marker;
                }
            };
            for (const auto& range : {drag_range_snap_bounds_, drag_loop_snap_bounds_})
                if (range) {
                    consider(range->in_us, 0);
                    consider(range->out_us, 0);
                }
            consider(drag_playhead_, 1);
            consider(0, 2);
            consider(drag_snapshot_->result_range.in_us, 2);
            if (drag_snapshot_->result_range.out_mode == edit::ResultOutMode::Fixed)
                consider(drag_snapshot_->result_range.out_us, 2);
            for (const auto& clip : drag_snapshot_->clips)
                if (!std::binary_search(drag_operation_targets_.begin(), drag_operation_targets_.end(), clip.id)) {
                    consider(clip.start, 2);
                    consider(clip.end(), 2);
                }
            for (const auto& source : drag_snapshot_->assets)
                if (source->id == lead->asset)
                    for (const auto& marker : source->markers) {
                        if (marker.time_ms > static_cast<uint64_t>((std::numeric_limits<edit::Time>::max)() / 1000))
                            continue;
                        const auto source_time = static_cast<edit::Time>(marker.time_ms) * 1000;
                        if (source_time < lead->source_in || source_time > lead->source_out)
                            continue;
                        if (const auto position = AddTimelineTime(lead->start, source_time - lead->source_in))
                            consider(*position, 3);
                    }
        }
        if (drag_snap_marker_)
            delta = *drag_snap_marker_ - boundary;
    }
    if (drag_edge_ >= 0) {
        edit::Trim command{drag_clip_, drag_edge_ == 0 ? edit::TrimEdge::Left : edit::TrimEdge::Right, delta};
        command.targets = drag_targets_;
        command.mode = drag_ripple_ ? edit::TrimMode::Ripple : edit::TrimMode::Fixed;
        command.expand_linked = drag_expand_linked_;
        command.auto_crossfade = drag_auto_crossfade_;
        drag_command_ = command;
    } else {
        edit::Move command{drag_targets_, delta};
        command.expand_linked = drag_expand_linked_;
        command.auto_crossfade = drag_auto_crossfade_;
        command.ripple = drag_ripple_ || insert;
        if (new_track >= 0) {
            const auto kind = new_track == 2 ? edit::TrackType::Audio : edit::TrackType::Video;
            track = drag_snapshot_->id_high_watermark + 1;
            command.new_tracks.push_back({track, kind});
            command.new_video_front = new_track == 0;
            if (drag_expand_linked_ && lead->group) {
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
    if (!preflightPendingDraft()) {
        cancelClipDrag();
        return false;
    }
    if (!drag_snapshot_ || !drag_command_ || !dragValid()) {
        const auto reason = dragReason();
        cancelClipDrag();
        refuseEdit(reason.isEmpty() ? tr("The drag is unavailable or the timeline changed. Try again.") : reason);
        return false;
    }
    const bool paste = std::holds_alternative<edit::PasteClips>(*drag_command_);
    const auto* removal = std::get_if<edit::RippleRemoveRange>(&*drag_command_);
    const auto removal_start = removal ? std::optional<edit::Time>(removal->span.in_us) : std::nullopt;
    const auto watermark = drag_snapshot_->id_high_watermark;
    const auto result = workspace_.apply(*drag_command_, drag_snapshot_->revision);
    if (result.status != edit::CommandStatus::Rejected) {
        drag_selection_.reset();
        if (removal_start && result.status == edit::CommandStatus::Applied) {
            selection_.clear();
            workspace_.select(0);
            workspace_.seek(*removal_start);
        }
        if (paste && result.status == edit::CommandStatus::Applied) {
            selection_.clear();
            for (const auto id : edit::OrderedClipIds(*workspace_.snapshot()))
                if (id > watermark)
                    selection_.clip_ids.push_back(id);
            if (!selection_.clip_ids.empty()) {
                selection_.mode = edit::SelectionMode::Clips;
                selection_.primary_clip = selection_.anchor_clip = selection_.clip_ids.front();
                selection_.active_track = workspace_.clip(selection_.primary_clip)->track;
            }
            workspace_.select(selection_.primary_clip);
        }
    }
    cancelClipDrag();
    publishWorkspace(result.status != edit::CommandStatus::Rejected,
                     result.error ? TranslateEditError(*result.error) : QString());
    if (removal_start && result.status == edit::CommandStatus::Applied)
        requestSeekUs(workspace_.playhead());
    return result.status != edit::CommandStatus::Rejected;
}

QString EditSessionAdapter::dragSourceFailure() const {
    if (!drag_snapshot_)
        return {};
    if (std::none_of(source_publications_.begin(), source_publications_.end(),
                     [](const auto& entry) { return entry.second.failed; }))
        return {};
    std::unordered_map<edit::Id, const edit::Clip*> originals;
    for (const auto& clip : drag_snapshot_->clips)
        originals.emplace(clip.id, &clip);
    for (const auto& clip : drag_preview_.clips) {
        if (!sourceVerificationFailed(clip.asset))
            continue;
        const auto before = originals.find(clip.id);
        const bool affected = clip.id == drag_clip_ ||
                              std::find(drag_targets_.begin(), drag_targets_.end(), clip.id) != drag_targets_.end() ||
                              before == originals.end() || *before->second != clip;
        if (affected) {
            const auto reason = sourceVerificationReason(clip.asset);
            return reason.isEmpty() ? tr("The source could not be verified.") : reason;
        }
    }
    return {};
}

bool EditSessionAdapter::dragValid() const {
    return drag_preview_.valid && drag_snapshot_ && workspace_.revision() == drag_snapshot_->revision &&
           dragSourceFailure().isEmpty();
}

QString EditSessionAdapter::dragReason() const {
    if (drag_snapshot_ && drag_snapshot_->revision != workspace_.revision())
        return TranslateEditError({edit::EditErrorCode::StaleRevision, {}, {}});
    if (const auto failure = dragSourceFailure(); !failure.isEmpty())
        return failure;
    return drag_preview_.error ? TranslateEditError(*drag_preview_.error) : QString();
}

void EditSessionAdapter::cancelClipDrag() {
    drag_snapshot_.reset();
    drag_command_.reset();
    drag_preview_ = {};
    drag_junction_ = 0;
    drag_spacer_ = 0;
    drag_targets_.clear();
    drag_operation_targets_.clear();
    drag_snap_marker_.reset();
    drag_gain_ = false;
    drag_gain_fine_ = false;
    if (drag_selection_) {
        auto original = std::move(*drag_selection_);
        drag_selection_.reset();
        restoreGestureSelection(std::move(original));
    }
    emit dragChanged();
}

QVariantList EditSessionAdapter::dragTracks() const {
    QVariantList rows;
    for (const auto& track : drag_preview_.tracks)
        rows.push_back(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(track.id)},
                                   {QStringLiteral("video"), track.type == edit::TrackType::Video}});
    return rows;
}

qint64 EditSessionAdapter::dragDurationUs() const {
    auto duration = workspace_.duration();
    if (!drag_snapshot_ || !drag_preview_.valid)
        return duration;
    duration = 0;
    for (const auto& clip : drag_preview_.clips)
        if (const auto end = AddTimelineTime(clip.start, clip.duration()))
            duration = std::max(duration, *end);
    for (const auto& spacer : drag_preview_.spacers)
        if (const auto end = AddTimelineTime(spacer.start_us, spacer.duration_us))
            duration = std::max(duration, *end);
    if (drag_preview_.timeline_end.mode == edit::TimelineEndMode::FixedEnd)
        duration = std::max(duration, drag_preview_.timeline_end.end_us);
    return duration;
}

QVariantMap EditSessionAdapter::dragBounds() const {
    if (!drag_snapshot_ || !drag_preview_.valid)
        return {{QStringLiteral("valid"), false}};
    edit::Time start = (std::numeric_limits<edit::Time>::max)(), end = 0;
    const auto include = [&](edit::Time from, edit::Time to) {
        start = std::min(start, from);
        end = std::max(end, to);
    };
    const auto clips = edit::detail::EntityDifference(drag_preview_.clips, drag_snapshot_->clips,
                                                      [](const auto& clip) { return clip.id; });
    for (const auto& clip : clips)
        include(clip.start, clip.end());
    const auto spacers = edit::detail::EntityDifference(drag_preview_.spacers, drag_snapshot_->spacers,
                                                        [](const auto& spacer) { return spacer.id; });
    for (const auto& spacer : spacers)
        include(spacer.start_us, spacer.end());
    const auto junctions = edit::detail::EntityDifference(drag_preview_.junctions, drag_snapshot_->junctions,
                                                          [](const auto& junction) { return junction.id; });
    for (const auto& junction : junctions)
        include(junction.in_us, junction.out_us);
    const auto old_clips = edit::detail::EntityDifference(drag_snapshot_->clips, drag_preview_.clips,
                                                          [](const auto& clip) { return clip.id; });
    for (const auto& clip : old_clips)
        include(clip.start, clip.end());
    const auto old_spacers = edit::detail::EntityDifference(drag_snapshot_->spacers, drag_preview_.spacers,
                                                            [](const auto& spacer) { return spacer.id; });
    for (const auto& spacer : old_spacers)
        include(spacer.start_us, spacer.end());
    const auto old_junctions = edit::detail::EntityDifference(drag_snapshot_->junctions, drag_preview_.junctions,
                                                              [](const auto& junction) { return junction.id; });
    for (const auto& junction : old_junctions)
        include(junction.in_us, junction.out_us);
    return {{QStringLiteral("valid"), end > start}, {QStringLiteral("startUs"), start}, {QStringLiteral("endUs"), end}};
}

QVariantMap EditSessionAdapter::dragDonorIds() const {
    if (!drag_snapshot_ || !drag_preview_.valid)
        return {};
    const auto ids = [](const auto& original, const auto& candidate) {
        QVariantList result;
        const auto changed =
            edit::detail::EntityDifference(original, candidate, [](const auto& value) { return value.id; });
        result.reserve(static_cast<qsizetype>(changed.size()));
        for (const auto& value : changed)
            result.push_back(QVariant::fromValue<qulonglong>(value.id));
        return result;
    };
    return {{QStringLiteral("clips"), ids(drag_snapshot_->clips, drag_preview_.clips)},
            {QStringLiteral("spacers"), ids(drag_snapshot_->spacers, drag_preview_.spacers)},
            {QStringLiteral("junctions"), ids(drag_snapshot_->junctions, drag_preview_.junctions)}};
}

QVariantList EditSessionAdapter::dragClips() const {
    QVariantList rows;
    if (!drag_snapshot_)
        return rows;
    const auto changed = edit::detail::EntityDifference(drag_preview_.clips, drag_snapshot_->clips,
                                                        [](const auto& clip) { return clip.id; });
    for (const auto& clip : changed) {
        const auto track = std::find_if(drag_preview_.tracks.begin(), drag_preview_.tracks.end(),
                                        [&clip](const auto& value) { return value.id == clip.track; });
        rows.push_back(
            QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(clip.id)},
                        {QStringLiteral("trackIndex"), static_cast<int>(track - drag_preview_.tracks.begin())},
                        {QStringLiteral("trackId"), QVariant::fromValue<qulonglong>(clip.track)},
                        {QStringLiteral("startMs"), clip.start / 1000.0},
                        {QStringLiteral("startUs"), clip.start},
                        {QStringLiteral("sourceInUs"), clip.source_in},
                        {QStringLiteral("sourceOutUs"), clip.source_out},
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
    if (!drag_snapshot_)
        return rows;
    const auto changed = edit::detail::EntityDifference(drag_preview_.junctions, drag_snapshot_->junctions,
                                                        [](const auto& junction) { return junction.id; });
    for (const auto& junction : changed) {
        const auto track = std::find_if(drag_preview_.tracks.begin(), drag_preview_.tracks.end(),
                                        [&junction](const auto& value) { return value.id == junction.track; });
        rows.push_back(QVariantMap{
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(junction.id)},
            {QStringLiteral("outgoing"), QVariant::fromValue<qulonglong>(junction.outgoing)},
            {QStringLiteral("incoming"), QVariant::fromValue<qulonglong>(junction.incoming)},
            {QStringLiteral("kind"),
             junction.kind == edit::TransitionKind::HardCut ? QStringLiteral("hardCut") : QStringLiteral("crossfade")},
            {QStringLiteral("cutUs"), junction.cut_us},
            {QStringLiteral("startMs"), junction.in_us / 1000.0},
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
    if (!preflightPendingDraft())
        return;
    edit::Time start = out ? 0 : workspace_.playhead();
    edit::Time end = out ? workspace_.playhead() : workspace_.duration();
    edit::Time anchor = out ? start : end;
    QVariantList tracks;
    if (selection_.mode == edit::SelectionMode::Range && selection_.range) {
        const auto& range = *selection_.range;
        start = out ? range.start_us : workspace_.playhead();
        end = out ? workspace_.playhead() : range.end_us;
        anchor = out ? range.start_us : range.end_us;
        for (const auto id : range.track_ids)
            tracks.push_back(QVariant::fromValue<qulonglong>(id));
    } else {
        std::set<edit::Id> scope;
        if (selection_.active_track)
            scope.insert(selection_.active_track);
        if (linkedSelection && selection_.active_track)
            for (const auto& clip : workspace_.clips())
                if (clip.track == selection_.active_track && clip.start <= workspace_.playhead() &&
                    workspace_.playhead() < clip.end() && clip.group)
                    for (const auto& member : workspace_.clips())
                        if (member.group == clip.group)
                            scope.insert(member.track);
        for (const auto& track : workspace_.tracks())
            if (!selection_.active_track || scope.contains(track.id))
                tracks.push_back(QVariant::fromValue<qulonglong>(track.id));
    }
    setEditRangeUs(start, end, tracks, anchor);
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
    applyTimeValue(id, incoming ? QStringLiteral("fadeIn") : QStringLiteral("fadeOut"), duration_ms * 1000);
}

QVariantMap EditSessionAdapter::selectedClipDetails() const {
    if (const auto* clip = workspace_.clip(workspace_.selection()))
        return projectClipRow(*clip);
    return {};
}

QVariantMap EditSessionAdapter::selectedClipSummary() const {
    int count = 0;
    int video = 0;
    int audio = 0;
    int unlocked = 0;
    int gain_eligible = 0;
    QVariantList audio_ids;
    int timing_eligible = 0;
    std::optional<double> gain;
    bool gain_mixed = false;
    std::array<edit::Time, 6> first_times{};
    std::array<bool, 6> mixed{};
    const std::unordered_set<edit::Id> ids(selection_.clip_ids.begin(), selection_.clip_ids.end());
    std::map<edit::Id, std::pair<bool, bool>> tracks;
    for (const auto& track : workspace_.tracks())
        tracks.emplace(track.id, std::pair{track.type == edit::TrackType::Video, workspace_.trackLocked(track.id)});
    if (selection_.mode == edit::SelectionMode::Clips)
        for (const auto& clip : workspace_.clips()) {
            if (!ids.contains(clip.id))
                continue;
            const auto track = tracks.find(clip.track);
            if (track == tracks.end())
                continue;
            const bool is_video = track->second.first;
            const bool is_unlocked = !track->second.second;
            video += is_video;
            audio += !is_video;
            if (!is_video)
                audio_ids.push_back(QVariant::fromValue<qulonglong>(clip.id));
            unlocked += is_unlocked;
            const auto* asset = workspace_.asset(clip.asset);
            const bool available =
                asset && asset->state == edit::AssetState::Available && !sourceVerificationFailed(clip.asset);
            timing_eligible += is_unlocked && available;
            gain_eligible += !is_video && is_unlocked;
            const std::array<edit::Time, 6> times{clip.start,      clip.source_in,           clip.source_out,
                                                  clip.duration(), clip.fade_in.duration_us, clip.fade_out.duration_us};
            if (!count)
                first_times = times;
            else
                for (std::size_t field = 0; field < times.size(); ++field)
                    mixed[field] = mixed[field] || times[field] != first_times[field];
            if (!is_video) {
                if (!gain)
                    gain = clip.gain_db;
                else
                    gain_mixed = gain_mixed || *gain != clip.gain_db;
            }
            ++count;
        }
    QVariantMap result{{QStringLiteral("count"), count},
                       {QStringLiteral("videoCount"), video},
                       {QStringLiteral("audioCount"), audio},
                       {QStringLiteral("audioIds"), audio_ids},
                       {QStringLiteral("unlockedCount"), unlocked},
                       {QStringLiteral("gainEligibleCount"), gain_eligible},
                       {QStringLiteral("timingEligibleCount"), timing_eligible},
                       {QStringLiteral("gainMixed"), gain_mixed},
                       {QStringLiteral("timingMixed"), mixed[0] || mixed[1] || mixed[2] || mixed[3]}};
    if (gain && !gain_mixed)
        result.insert(QStringLiteral("gainDb"), *gain);
    const std::array<QString, 6> names{QStringLiteral("start"),    QStringLiteral("in"),     QStringLiteral("out"),
                                       QStringLiteral("duration"), QStringLiteral("fadeIn"), QStringLiteral("fadeOut")};
    for (std::size_t field = 0; field < names.size(); ++field) {
        result.insert(names[field] + QStringLiteral("Mixed"), mixed[field]);
        if (count && !mixed[field])
            result.insert(names[field] + QStringLiteral("Us"), first_times[field]);
    }
    return result;
}

QVariantList EditSessionAdapter::sourceAudioChoices() const {
    QVariantList rows;
    for (const auto& asset : workspace_.assets()) {
        if (!asset.mix_decision_required)
            continue;
        const auto metadata = sourceMetadata(asset.id);
        const auto reason = sourceVerificationReason(asset.id);
        rows.push_back(QVariantMap{
            {QStringLiteral("assetId"), QVariant::fromValue<qulonglong>(asset.id)},
            {QStringLiteral("name"), QString::fromStdString(asset.name)},
            {QStringLiteral("available"), metadata && sourceAccess(asset.id) && !sourceVerificationFailed(asset.id)},
            {QStringLiteral("reason"), reason}});
    }
    return rows;
}
QVariantMap EditSessionAdapter::sourceAudioChoice() const {
    const auto* asset = workspace_.asset(source_audio_asset_);
    const auto metadata = sourceMetadata(source_audio_asset_);
    QString reason = source_audio_reason_;
    if (asset && (!sourceAccess(asset->id) || !metadata || sourceVerificationFailed(asset->id)))
        reason = sourceVerificationReason(asset->id).isEmpty() ? tr("The source is unavailable.")
                                                               : sourceVerificationReason(asset->id);
    const bool locked =
        asset && std::any_of(workspace_.clips().begin(), workspace_.clips().end(), [&](const auto& clip) {
            return clip.asset == asset->id && clip.stream && clip.stream->kind == engine::TimelineStreamKind::Audio &&
                   workspace_.trackLocked(clip.track);
        });
    if (locked)
        reason = tr("Unlock the source's audio tracks before choosing its input mix.");
    const bool scope_review = rangeScopeReviewActive();
    if (scope_review)
        reason = tr("Apply or cancel the track scope review before continuing.");
    const bool available = asset && metadata && sourceAccess(asset->id) && !sourceVerificationFailed(asset->id) &&
                           !locked && !scope_review;
    bool overlap = metadata && !metadata->audio_metadata.manifest && source_audio_selected_.size() > 1;
    if (metadata && metadata->audio_metadata.manifest) {
        engine::MonitorSelectionSnapshot selection;
        selection.media_id = metadata->audio_metadata.selection_media_id;
        selection.revision = {1};
        for (const auto id : source_audio_selected_)
            selection.tracks.push_back({id});
        overlap = !engine::ValidateMonitorSelection(selection, *metadata->audio_metadata.manifest).succeeded();
    }
    QVariantList tracks;
    if (metadata)
        for (const auto& track : metadata->audio_metadata.actual_tracks) {
            const bool selected = std::find(source_audio_selected_.begin(), source_audio_selected_.end(),
                                            track.track_id) != source_audio_selected_.end();
            tracks.push_back(QVariantMap{
                {QStringLiteral("id"), QString::fromStdString(engine::ToUuid(track.track_id))},
                {QStringLiteral("name"), track.name.empty() ? tr("Audio stream %1").arg(track.stream_index.value + 1)
                                                            : QString::fromStdString(track.name)},
                {QStringLiteral("role"), QString::fromStdString(track.role)},
                {QStringLiteral("channels"), track.codec.channels},
                {QStringLiteral("selected"), selected},
                {QStringLiteral("enabled"), available && !source_audio_busy_},
                {QStringLiteral("reason"), reason}});
        }
    const bool active = asset && asset->mix_decision_required;
    return {{QStringLiteral("active"), active},
            {QStringLiteral("assetId"), QVariant::fromValue<qulonglong>(source_audio_asset_)},
            {QStringLiteral("name"), asset ? QString::fromStdString(asset->name) : QString()},
            {QStringLiteral("busy"), source_audio_busy_},
            {QStringLiteral("canApply"),
             active && available && !source_audio_busy_ && (!overlap || source_audio_overlap_confirmed_)},
            {QStringLiteral("overlapRequired"), overlap},
            {QStringLiteral("overlapConfirmed"), source_audio_overlap_confirmed_},
            {QStringLiteral("tracks"), tracks},
            {QStringLiteral("reason"), reason}};
}
bool EditSessionAdapter::beginSourceAudioChoice(qulonglong id) {
    const auto* asset = workspace_.asset(id);
    if (!asset || !asset->mix_decision_required || !sourceMetadata(id) || !sourceAccess(id) ||
        sourceVerificationFailed(id))
        return false;
    ++source_audio_generation_;
    source_audio_cancelled_->store(true, std::memory_order_relaxed);
    source_audio_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    source_audio_selected_.clear();
    source_audio_overlap_confirmed_ = false;
    source_audio_busy_ = false;
    source_audio_reason_.clear();
    source_audio_asset_ = id;
    emit sourceAudioChoiceChanged();
    return true;
}
void EditSessionAdapter::setSourceAudioTrack(const QString& text, bool selected) {
    const auto metadata = sourceMetadata(source_audio_asset_);
    const auto id = engine::FromUuid<engine::AudioTrackId>(text.toStdString());
    if (source_audio_busy_ || !metadata || !id ||
        std::none_of(metadata->audio_metadata.actual_tracks.begin(), metadata->audio_metadata.actual_tracks.end(),
                     [&](const auto& track) { return track.track_id == *id; }))
        return;
    const auto found = std::find(source_audio_selected_.begin(), source_audio_selected_.end(), *id);
    if (selected && found == source_audio_selected_.end())
        source_audio_selected_.push_back(*id);
    else if (!selected && found != source_audio_selected_.end())
        source_audio_selected_.erase(found);
    source_audio_overlap_confirmed_ = false;
    source_audio_reason_.clear();
    emit sourceAudioChoiceChanged();
}
void EditSessionAdapter::setSourceAudioOverlapConfirmed(bool confirmed) {
    if (source_audio_busy_)
        return;
    source_audio_overlap_confirmed_ = confirmed;
    source_audio_reason_.clear();
    emit sourceAudioChoiceChanged();
}
bool EditSessionAdapter::applySourceAudioChoice() {
    if (!preflightPendingDraft() || !sourceAudioChoice().value(QStringLiteral("canApply")).toBool())
        return false;
    const auto asset = source_audio_asset_;
    const auto access = sourceAccess(asset);
    const auto metadata = sourceMetadata(asset);
    const auto audio_stream = std::find_if(metadata->streams.begin(), metadata->streams.end(), [](const auto& stream) {
        return stream.kind == engine::TimelineStreamKind::Audio;
    });
    if (audio_stream == metadata->streams.end())
        return false;
    const auto container_index = audio_stream->container_index;
    engine::MonitorSelectionSnapshot selection;
    selection.media_id = metadata->audio_metadata.selection_media_id;
    const auto* source = workspace_.asset(asset);
    const auto prior = source->monitor_selection ? source->monitor_selection->revision.value : 0;
    if (prior == (std::numeric_limits<uint64_t>::max)())
        return false;
    selection.revision = {prior + 1};
    selection.overlap_confirmed =
        source_audio_overlap_confirmed_ || (!metadata->audio_metadata.manifest && source_audio_selected_.size() <= 1);
    selection.unverified_asset_binding = metadata->audio_metadata.unverified_asset_binding;
    for (const auto id : source_audio_selected_)
        selection.tracks.push_back({id});
    const auto generation = ++source_audio_generation_;
    const auto revision = workspace_.revision();
    const auto cancelled = source_audio_cancelled_;
    source_audio_busy_ = true;
    emit sourceAudioChoiceChanged();
    QPointer<EditSessionAdapter> self(this);
    metadata_pool_.start(
        [self, asset, access, container_index, selection = std::move(selection), generation, revision,
         cancelled]() mutable {
            std::string error;
            engine::EditPlayerEngine player;
            const bool admitted = !cancelled->load(std::memory_order_relaxed) &&
                                  player.OpenAudioStream(access, container_index, error, cancelled.get()) &&
                                  player.SetMonitorSelection(selection, error, cancelled.get());
            if (self)
                QMetaObject::invokeMethod(
                    self,
                    [self, asset, access, selection = std::move(selection), generation, revision, admitted,
                     error = std::move(error)]() mutable {
                        if (!self || generation != self->source_audio_generation_)
                            return;
                        self->source_audio_busy_ = false;
                        if (revision != self->workspace_.revision() || access != self->sourceAccess(asset) ||
                            self->sourceVerificationFailed(asset)) {
                            self->source_audio_reason_ =
                                tr("The source or timeline changed. Review the choice and apply again.");
                        } else if (!admitted) {
                            self->source_audio_reason_ = QString::fromStdString(error);
                        } else if (self->sourceAudioChoice().value(QStringLiteral("canApply")).toBool()) {
                            self->setAssetMonitorSelection(asset, std::move(selection));
                            if (const auto* current = self->workspace_.asset(asset);
                                current && !current->mix_decision_required) {
                                self->cancelSourceAudioChoice();
                                return;
                            }
                            self->source_audio_reason_ = tr("The input mix could not be applied.");
                        }
                        emit self->sourceAudioChoiceChanged();
                    },
                    Qt::QueuedConnection);
        },
        1);
    return true;
}
void EditSessionAdapter::cancelSourceAudioChoice() {
    ++source_audio_generation_;
    source_audio_cancelled_->store(true, std::memory_order_relaxed);
    source_audio_asset_ = 0;
    source_audio_selected_.clear();
    source_audio_overlap_confirmed_ = false;
    source_audio_busy_ = false;
    source_audio_reason_.clear();
    emit sourceAudioChoiceChanged();
}

void EditSessionAdapter::syncOverview() {
    if (overview_revision_ == workspace_.revision())
        return;
    overview_revision_ = workspace_.revision();
    struct TrackInfo {
        int index;
        bool video;
        bool enabled;
    };
    std::map<edit::Id, TrackInfo> tracks;
    for (std::size_t index = 0; index < workspace_.tracks().size(); ++index) {
        const auto& track = workspace_.tracks()[index];
        tracks.emplace(track.id,
                       TrackInfo{static_cast<int>(index), track.type == edit::TrackType::Video, track.edit_enabled});
    }
    std::vector<OverviewSegment> segments;
    segments.reserve(workspace_.clips().size());
    for (const auto& clip : workspace_.clips())
        if (const auto track = tracks.find(clip.track); track != tracks.end())
            segments.emplace_back(clip.start, clip.end(), track->second.index, track->second.video,
                                  clip.enabled && track->second.enabled);
    if (segments == overview_state_)
        return;
    overview_state_ = std::move(segments);
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(overview_state_.size()));
    for (const auto& [start, end, track, video, enabled] : overview_state_)
        rows.push_back(QVariantMap{{QStringLiteral("startUs"), start},
                                   {QStringLiteral("endUs"), end},
                                   {QStringLiteral("trackIndex"), track},
                                   {QStringLiteral("video"), video},
                                   {QStringLiteral("enabled"), enabled}});
    overview_segments_ = std::move(rows);
    emit overviewChanged();
}

QVariantMap EditSessionAdapter::selectedJunctionDetails() const {
    for (const auto& value : transitions())
        if (value.toMap().value(QStringLiteral("selected")).toBool())
            return value.toMap();
    return {};
}

bool EditSessionAdapter::deleteTrack(qulonglong id, bool confirmed) {
    if (!preflightPendingDraft())
        return false;
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
    if (!preflightPendingDraft())
        return;
    const auto snapshot = workspace_.snapshot();
    if (std::none_of(snapshot->spacers.begin(), snapshot->spacers.end(),
                     [id](const auto& value) { return value.id == id; }))
        return;
    selected_spacer_ = id;
    selected_gap_track_ = 0;
    selected_transition_ = 0;
    selection_.clear();
    selection_.mode = edit::SelectionMode::Spacer;
    workspace_.select(0);
    publishSelection();
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
    if (!preflightPendingDraft())
        return;
    if (std::none_of(workspace_.tracks().begin(), workspace_.tracks().end(),
                     [track](const auto& value) { return value.id == track; }))
        return;
    selection_.clear();
    selection_.active_track = track;
    workspace_.select(0);
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
    if (span.out_us <= span.in_us || at < span.in_us || at >= span.out_us) {
        publishSelection();
        return;
    }
    selected_gap_ = span;
    selected_gap_track_ = track;
    selected_spacer_ = 0;
    selected_transition_ = 0;
    selection_.mode = edit::SelectionMode::Gap;
    workspace_.select(0);
    publishSelection();
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

void EditSessionAdapter::syncSourceVersions() {
    if (source_versions_.empty())
        return;
    std::vector<edit::Id> resumed;
    std::vector<edit::Id> changed;
    for (const auto& asset : workspace_.assets()) {
        auto& current = source_publications_[asset.id];
        if (current.version == asset.source_version)
            continue;
        auto archived = current;
        archived.verifying = false;
        source_versions_[{asset.id, current.version}] = std::move(archived);
        const auto prior = source_versions_.find({asset.id, asset.source_version});
        current = prior != source_versions_.end() ? prior->second : SourcePublication{};
        current.version = asset.source_version;
        current.verifying = false;
        if (current.access && !current.proof && !current.failed)
            resumed.push_back(asset.id);
        changed.push_back(asset.id);
    }
    const auto retained = workspace_.retainedSourceVersions();
    std::erase_if(source_versions_, [&](const auto& entry) { return !retained.contains(entry.first); });
    if (!changed.empty()) {
        ++clip_generation_;
        keyframe_timestamps_.clear();
        const auto session = source_cancelled_;
        const auto revision = workspace_.revision();
        if (std::find(changed.begin(), changed.end(), source_audio_asset_) != changed.end())
            cancelSourceAudioChoice();
        emit trimSnapReadyChanged();
        if (session != source_cancelled_ || session->load(std::memory_order_relaxed) ||
            revision != workspace_.revision())
            return;
        for (const auto id : changed)
            if (const auto current = source_publications_.find(id);
                current != source_publications_.end() && current->second.access)
                startKeyframeScan(current->second.access);
    }
    for (const auto id : resumed) {
        const auto* source = workspace_.asset(id);
        if (!source)
            continue;
        const auto version = source->source_version;
        QTimer::singleShot(0, this, [this, id, version] {
            const auto* source = workspace_.asset(id);
            if (source && source->source_version == version)
                retrySourceVerification(id);
        });
    }
}

std::optional<edit::ReplaceAssetSource> EditSessionAdapter::sourceReplacementCommand() const {
    if (!source_replacement_ || !source_replacement_->candidate)
        return std::nullopt;
    const auto& draft = *source_replacement_;
    auto candidate = *draft.candidate;
    if (draft.adopt && draft.publication.metadata && draft.publication.metadata->has_audio) {
        candidate.input_mix_selection = edit::InputMixSelection{};
        candidate.input_mix_selection->asset_identity_fingerprint =
            candidate.source_descriptor->asset_identity_fingerprint;
        candidate.input_mix_selection->authority = edit::InputMixAuthority::UserConfirmed;
        candidate.input_mix_selection->selected_stream_identities = draft.selected;
        candidate.mix_decision_required = false;
    }
    return edit::ReplaceAssetSource{draft.asset_id, draft.source_version, std::move(candidate), draft.adopt};
}

QVariantMap EditSessionAdapter::sourceReplacement() const {
    if (!source_replacement_)
        return {{QStringLiteral("active"), false}};
    const auto& draft = *source_replacement_;
    const auto old = std::find_if(draft.snapshot->assets.begin(), draft.snapshot->assets.end(),
                                  [&](const auto& asset) { return asset->id == draft.asset_id; });
    const auto metadata = draft.publication.metadata;
    QString reason = draft.reason;
    bool overlap = metadata && !metadata->audio_metadata.manifest && draft.selected.size() > 1;
    if (metadata && metadata->audio_metadata.manifest) {
        engine::MonitorSelectionSnapshot selection;
        selection.media_id = metadata->audio_metadata.selection_media_id;
        selection.revision = {1};
        for (const auto identity : draft.selected)
            selection.tracks.push_back({identity});
        overlap = !engine::ValidateMonitorSelection(selection, *metadata->audio_metadata.manifest).succeeded();
    }
    const auto* current = workspace_.asset(draft.asset_id);
    bool eligible =
        !draft.busy && draft.candidate && draft.publication.access && draft.publication.proof && reason.isEmpty();
    if (!current || current->source_version != draft.source_version ||
        workspace_.revision() != draft.snapshot->revision) {
        reason = tr("The source or timeline changed. Review the replacement again.");
        eligible = false;
    }
    const bool locked = std::any_of(workspace_.clips().begin(), workspace_.clips().end(), [&](const auto& clip) {
        return clip.asset == draft.asset_id && workspace_.trackLocked(clip.track);
    });
    if (locked) {
        reason = tr("Unlock every affected track before replacing its source.");
        eligible = false;
    }
    if (rangeScopeReviewActive()) {
        reason = tr("Apply or cancel the track scope review before continuing.");
        eligible = false;
    }
    if (const auto command = sourceReplacementCommand(); eligible && command) {
        const auto preview = edit::PreviewEdit(*draft.snapshot, *command);
        if (!preview.valid) {
            reason = preview.error ? TranslateEditError(*preview.error)
                                   : tr("The replacement is incompatible with the affected clips.");
            eligible = false;
        }
    }
    if (eligible && overlap && !draft.overlap_confirmed) {
        reason = tr("Confirm overlapping audio streams before applying the replacement.");
        eligible = false;
    }
    QVariantList tracks;
    if (metadata)
        for (const auto& track : metadata->audio_metadata.actual_tracks)
            tracks.push_back(QVariantMap{
                {QStringLiteral("id"), QString::fromStdString(engine::ToUuid(track.track_id))},
                {QStringLiteral("name"), track.name.empty() ? tr("Audio stream %1").arg(track.stream_index.value + 1)
                                                            : QString::fromStdString(track.name)},
                {QStringLiteral("role"), QString::fromStdString(track.role)},
                {QStringLiteral("channels"), track.codec.channels},
                {QStringLiteral("selected"),
                 std::find(draft.selected.begin(), draft.selected.end(), track.track_id) != draft.selected.end()},
                {QStringLiteral("enabled"), draft.adopt && !draft.busy}});
    return {{QStringLiteral("active"), true},
            {QStringLiteral("busy"), draft.busy},
            {QStringLiteral("canApply"), eligible},
            {QStringLiteral("reason"), reason},
            {QStringLiteral("stage"), draft.stage},
            {QStringLiteral("assetId"), QVariant::fromValue<qulonglong>(draft.asset_id)},
            {QStringLiteral("name"),
             old == draft.snapshot->assets.end() ? QString() : QString::fromStdString((*old)->name)},
            {QStringLiteral("oldPath"),
             old == draft.snapshot->assets.end() ? QString() : QString::fromStdWString((*old)->path)},
            {QStringLiteral("path"), draft.path},
            {QStringLiteral("url"), QUrl::fromLocalFile(draft.path)},
            {QStringLiteral("adoptChangedBytes"), draft.adopt},
            {QStringLiteral("expectedFingerprint"),
             old == draft.snapshot->assets.end() || !(*old)->source_descriptor ||
                     std::none_of((*old)->source_descriptor->asset_identity_fingerprint.begin(),
                                  (*old)->source_descriptor->asset_identity_fingerprint.end(),
                                  [](auto byte) { return byte != 0; })
                 ? QString()
                 : QString::fromStdString(engine::ToHex((*old)->source_descriptor->asset_identity_fingerprint))},
            {QStringLiteral("observedFingerprint"),
             draft.publication.proof ? QString::fromStdString(engine::ToHex(draft.publication.proof->Identity().sha256))
                                     : QString()},
            {QStringLiteral("durationUs"),
             metadata && metadata->duration_us > 0 ? QVariant::fromValue<qint64>(metadata->duration_us) : QVariant()},
            {QStringLiteral("requiredSourceEndUs"), draft.required_source_end_us},
            {QStringLiteral("affectedClips"), draft.affected_clips},
            {QStringLiteral("tracks"), tracks},
            {QStringLiteral("audioChoiceVisible"), draft.adopt && metadata && metadata->has_audio},
            {QStringLiteral("overlapRequired"), overlap},
            {QStringLiteral("overlapConfirmed"), draft.overlap_confirmed}};
}

void EditSessionAdapter::beginSourceReplacement(qulonglong asset_id, const QUrl& replacement,
                                                bool adopt_changed_bytes) {
    if (!preflightPendingDraft())
        return;
    std::shared_ptr<const engine::MediaReadCapability> retained_access;
    if (source_replacement_ && source_replacement_->asset_id == asset_id &&
        source_replacement_->path == replacement.toLocalFile())
        retained_access = source_replacement_->publication.access;
    ++source_replacement_generation_;
    source_replacement_cancelled_->store(true, std::memory_order_relaxed);
    source_replacement_.reset();
    const auto* source = workspace_.asset(asset_id);
    if (!source || !replacement.isLocalFile()) {
        emit sourceReplacementChanged();
        return;
    }
    source_replacement_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    SourceReplacementDraft draft;
    draft.asset_id = asset_id;
    draft.source_version = source->source_version;
    draft.snapshot = workspace_.snapshot();
    draft.path = replacement.toLocalFile();
    draft.adopt = adopt_changed_bytes;
    draft.overlap_confirmed =
        !adopt_changed_bytes && source->monitor_selection && source->monitor_selection->overlap_confirmed;
    draft.stage = QStringLiteral("metadata");
    draft.busy = true;
    for (const auto& clip : draft.snapshot->clips)
        if (clip.asset == asset_id) {
            const auto track = std::find_if(draft.snapshot->tracks.begin(), draft.snapshot->tracks.end(),
                                            [&](const auto& value) { return value.id == clip.track; });
            draft.affected_clips.push_back(
                QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(clip.id)},
                            {QStringLiteral("track"),
                             track == draft.snapshot->tracks.end() ? QString() : QString::fromStdString(track->name)},
                            {QStringLiteral("startUs"), clip.start},
                            {QStringLiteral("endUs"), clip.end()},
                            {QStringLiteral("sourceInUs"), clip.source_in},
                            {QStringLiteral("sourceOutUs"), clip.source_out}});
            draft.required_source_end_us = std::max(draft.required_source_end_us, clip.source_out);
        }
    std::optional<std::array<uint8_t, 32>> expected;
    if (source->source_descriptor &&
        std::any_of(source->source_descriptor->asset_identity_fingerprint.begin(),
                    source->source_descriptor->asset_identity_fingerprint.end(), [](auto byte) { return byte != 0; }))
        expected = source->source_descriptor->asset_identity_fingerprint;
    if (!adopt_changed_bytes && !expected) {
        draft.reason =
            tr("This source has no established identity. Explicitly adopt the replacement content to continue.");
        draft.stage = QStringLiteral("error");
        draft.busy = false;
    }
    source_replacement_ = std::move(draft);
    const auto generation = source_replacement_generation_;
    const auto cancelled = source_replacement_cancelled_;
    emit sourceReplacementChanged();
    if (!source_replacement_ || generation != source_replacement_generation_ || !source_replacement_->busy)
        return;
    // Explicit validation must remain runnable when optional analysis is paused.
    // A required export can join this capability's in-flight identity check.
    const engine::BackgroundIoAdmission admission;
    const auto metadata_admission = admission;
    const auto old_metadata = sourceMetadata(asset_id);
    const auto path = replacement.toLocalFile();
    QPointer<EditSessionAdapter> self(this);
    metadata_pool_.start([self, generation, cancelled, admission, metadata_admission, old_metadata, path, expected,
                          adopt_changed_bytes, access = std::move(retained_access)]() mutable {
        if (cancelled->load(std::memory_order_relaxed))
            return;
        std::string error;
        engine::EditMediaMetadata metadata;
        const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(path.toStdWString()));
        if (acquired.succeeded()) {
            access = *acquired.value();
            metadata = engine::ProbeEditMedia(access, {}, metadata_admission, cancelled.get(), false);
            error = metadata.error;
        } else {
            error = acquired.error()->context;
        }
        if (!self || cancelled->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(
            self,
            [self, generation, cancelled, admission, old_metadata, path, expected, adopt_changed_bytes,
             access = std::move(access), metadata = std::move(metadata), error = std::move(error)]() mutable {
                if (!self || generation != self->source_replacement_generation_ ||
                    cancelled->load(std::memory_order_relaxed) || !self->source_replacement_)
                    return;
                auto& current = *self->source_replacement_;
                if (error.empty() && metadata.duration_us < current.required_source_end_us)
                    error = tr("The replacement does not contain every required source interval.").toStdString();
                if (error.empty() && old_metadata) {
                    if (old_metadata->streams.size() != metadata.streams.size())
                        error = tr("The replacement stream layout does not match the source.").toStdString();
                    for (const auto& old : old_metadata->streams) {
                        const auto found =
                            std::find_if(metadata.streams.begin(), metadata.streams.end(), [&](const auto& value) {
                                return value.kind == old.kind && value.container_index == old.container_index;
                            });
                        if (found == metadata.streams.end() ||
                            (old.kind == engine::TimelineStreamKind::Audio &&
                             (found->codec.channels != old.codec.channels ||
                              found->codec.channel_layout != old.codec.channel_layout)) ||
                            (old.kind == engine::TimelineStreamKind::Video &&
                             edit::RationalFps{static_cast<uint32_t>(found->fps_num),
                                               static_cast<uint32_t>(found->fps_den)}
                                     .reduced() != edit::RationalFps{static_cast<uint32_t>(old.fps_num),
                                                                     static_cast<uint32_t>(old.fps_den)}
                                                       .reduced()))
                            error = tr("The replacement stream layout does not match the source.").toStdString();
                    }
                }
                current.publication.access = access;
                current.publication.metadata = std::make_shared<const engine::EditMediaMetadata>(std::move(metadata));
                if (!error.empty()) {
                    current.busy = false;
                    current.stage = QStringLiteral("error");
                    current.reason = QString::fromStdString(error);
                    emit self->sourceReplacementChanged();
                    return;
                }
                current.stage = QStringLiteral("verification");
                emit self->sourceReplacementChanged();
                self->scheduleSourceVerification([self, generation, cancelled, admission, path, adopt_changed_bytes,
                                                  access = std::move(access), expected] {
                    std::string error;
                    std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
                    engine::EditMediaMetadata metadata;
                    const auto verified = engine::VerifyMediaIdentity(
                        access, adopt_changed_bytes ? std::nullopt : expected, {}, admission, cancelled.get());
                    if (verified.succeeded()) {
                        proof = *verified.value();
                        metadata =
                            adopt_changed_bytes
                                ? engine::ProbeEditMediaForAdoption(access, proof, {}, admission, cancelled.get())
                                : engine::ProbeEditMedia(access, {}, admission, cancelled.get(), true);
                        error = metadata.error;
                    } else {
                        error = verified.error()->context;
                    }
                    if (!self || cancelled->load(std::memory_order_relaxed))
                        return;
                    QMetaObject::invokeMethod(
                        self,
                        [self, generation, cancelled, path, access, proof = std::move(proof),
                         metadata = std::move(metadata), error = std::move(error)]() mutable {
                            if (!self || generation != self->source_replacement_generation_ ||
                                cancelled->load(std::memory_order_relaxed) || !self->source_replacement_)
                                return;
                            auto& current = *self->source_replacement_;
                            current.busy = false;
                            current.stage = error.empty() ? QStringLiteral("ready") : QStringLiteral("error");
                            current.reason = QString::fromStdString(error);
                            current.publication.proof = std::move(proof);
                            if (error.empty()) {
                                auto candidate = AssetFromProbe(path, metadata, access);
                                if (!candidate.source_descriptor || current.snapshot->source_version_high_watermark ==
                                                                        (std::numeric_limits<uint64_t>::max)()) {
                                    current.stage = QStringLiteral("error");
                                    current.reason = tr("The replacement cannot be represented by this timeline.");
                                } else {
                                    candidate.id = current.asset_id;
                                    candidate.source_version = current.snapshot->source_version_high_watermark + 1;
                                    current.publication.version = candidate.source_version;
                                    if (candidate.input_mix_selection)
                                        current.selected = candidate.input_mix_selection->selected_stream_identities;
                                    current.candidate = std::move(candidate);
                                    current.publication.metadata =
                                        std::make_shared<const engine::EditMediaMetadata>(std::move(metadata));
                                    if (!current.adopt) {
                                        const auto command = self->sourceReplacementCommand();
                                        const auto preview = edit::PreviewEdit(*current.snapshot, *command);
                                        if (preview.valid && !preview.source_bindings.empty() &&
                                            preview.source_bindings.front()->input_mix_selection)
                                            current.selected = preview.source_bindings.front()
                                                                   ->input_mix_selection->selected_stream_identities;
                                    }
                                }
                            }
                            emit self->sourceReplacementChanged();
                        },
                        Qt::QueuedConnection);
                });
            },
            Qt::QueuedConnection);
    });
}

bool EditSessionAdapter::applySourceReplacement() {
    if (!preflightPendingDraft() || !sourceReplacement().value(QStringLiteral("canApply")).toBool())
        return false;
    const auto command = sourceReplacementCommand();
    const auto preview = edit::PreviewEdit(*source_replacement_->snapshot, *command);
    if (!preview.valid || preview.source_bindings.empty())
        return false;
    const auto metadata = source_replacement_->publication.metadata;
    const auto generation = source_replacement_generation_;
    if (!metadata->has_audio)
        return commitSourceReplacement(generation);
    const auto stream = std::find_if(metadata->streams.begin(), metadata->streams.end(),
                                     [](const auto& value) { return value.kind == engine::TimelineStreamKind::Audio; });
    if (stream == metadata->streams.end())
        return false;
    engine::MonitorSelectionSnapshot selection;
    selection.media_id = metadata->audio_metadata.selection_media_id;
    selection.revision = {1};
    selection.unverified_asset_binding = metadata->audio_metadata.unverified_asset_binding;
    const auto& mix = preview.source_bindings.front()->input_mix_selection;
    if (mix)
        for (const auto identity : mix->selected_stream_identities)
            selection.tracks.push_back({identity});
    selection.overlap_confirmed =
        source_replacement_->overlap_confirmed || (!metadata->audio_metadata.manifest && selection.tracks.size() <= 1);
    source_replacement_->busy = true;
    source_replacement_->stage = QStringLiteral("admission");
    emit sourceReplacementChanged();
    const auto access = source_replacement_->publication.access;
    const auto cancelled = source_replacement_cancelled_;
    const auto container_index = stream->container_index;
    QPointer<EditSessionAdapter> self(this);
    metadata_pool_.start(
        [self, generation, access, cancelled, container_index, selection = std::move(selection)]() mutable {
            std::string error;
            engine::EditPlayerEngine player;
            const bool admitted = !cancelled->load(std::memory_order_relaxed) &&
                                  player.OpenAudioStream(access, container_index, error, cancelled.get()) &&
                                  player.SetMonitorSelection(selection, error, cancelled.get());
            if (!self || cancelled->load(std::memory_order_relaxed))
                return;
            QMetaObject::invokeMethod(
                self,
                [self, generation, cancelled, selection = std::move(selection), admitted,
                 error = std::move(error)]() mutable {
                    if (!self || generation != self->source_replacement_generation_ ||
                        cancelled->load(std::memory_order_relaxed) || !self->source_replacement_)
                        return;
                    self->source_replacement_->busy = false;
                    if (!admitted) {
                        self->source_replacement_->stage = QStringLiteral("error");
                        self->source_replacement_->reason = QString::fromStdString(error);
                        emit self->sourceReplacementChanged();
                    } else {
                        self->commitSourceReplacement(generation, std::move(selection));
                    }
                },
                Qt::QueuedConnection);
        },
        1);
    return true;
}

bool EditSessionAdapter::commitSourceReplacement(uint64_t generation,
                                                 std::optional<engine::MonitorSelectionSnapshot> selection) {
    if (!preflightPendingDraft() || !source_replacement_ || generation != source_replacement_generation_ ||
        source_replacement_cancelled_->load(std::memory_order_relaxed))
        return false;
    auto& draft = *source_replacement_;
    const auto* old = workspace_.asset(draft.asset_id);
    if (!old || old->source_version != draft.source_version || workspace_.revision() != draft.snapshot->revision) {
        draft.busy = false;
        draft.stage = QStringLiteral("error");
        draft.reason = tr("The source or timeline changed. Review the replacement again.");
        emit sourceReplacementChanged();
        return false;
    }
    auto command = sourceReplacementCommand();
    const auto preview = edit::PreviewEdit(*draft.snapshot, *command);
    if (!preview.valid || preview.source_bindings.empty()) {
        draft.busy = false;
        draft.stage = QStringLiteral("error");
        draft.reason = preview.error ? TranslateEditError(*preview.error)
                                     : tr("The replacement is incompatible with the affected clips.");
        emit sourceReplacementChanged();
        return false;
    }
    command->replacement = *preview.source_bindings.front();
    command->replacement.monitor_selection = std::move(selection);
    const auto asset_id = draft.asset_id;
    const auto old_version = draft.source_version;
    const auto revision = draft.snapshot->revision;
    const auto publication = draft.publication;
    cancelRangeGesture();
    cancelClipDrag();
    if (!source_replacement_ || generation != source_replacement_generation_ ||
        source_replacement_cancelled_->load(std::memory_order_relaxed) || workspace_.revision() != revision)
        return false;
    const auto result = workspace_.apply(*command, revision);
    if (result.status != edit::CommandStatus::Applied) {
        draft.busy = false;
        draft.stage = QStringLiteral("error");
        draft.reason = result.error ? TranslateEditError(*result.error) : tr("The replacement could not be applied.");
        emit sourceReplacementChanged();
        return false;
    }
    auto previous = source_publications_.contains(asset_id) ? source_publications_.at(asset_id) : SourcePublication{};
    previous.version = old_version;
    previous.verifying = false;
    source_versions_[{asset_id, old_version}] = std::move(previous);
    source_versions_[{asset_id, command->replacement.source_version}] = publication;
    syncSourceVersions();
    cancelSourceReplacement();
    publishWorkspace(true);
    return true;
}

void EditSessionAdapter::cancelSourceReplacement() {
    ++source_replacement_generation_;
    source_replacement_cancelled_->store(true, std::memory_order_relaxed);
    const bool active = source_replacement_.has_value();
    source_replacement_.reset();
    if (active)
        emit sourceReplacementChanged();
}

void EditSessionAdapter::setSourceReplacementTrack(const QString& identity, bool selected) {
    if (!source_replacement_ || source_replacement_->busy || !source_replacement_->adopt ||
        !source_replacement_->publication.metadata)
        return;
    const auto parsed = engine::FromUuid<engine::AudioTrackId>(identity.toStdString());
    if (!parsed)
        return;
    const auto& tracks = source_replacement_->publication.metadata->audio_metadata.actual_tracks;
    if (std::none_of(tracks.begin(), tracks.end(), [&](const auto& track) { return track.track_id == *parsed; }))
        return;
    auto& chosen = source_replacement_->selected;
    std::erase(chosen, *parsed);
    if (selected)
        chosen.push_back(*parsed);
    source_replacement_->overlap_confirmed = false;
    emit sourceReplacementChanged();
}

void EditSessionAdapter::setSourceReplacementOverlapConfirmed(bool confirmed) {
    if (!source_replacement_ || source_replacement_->busy)
        return;
    source_replacement_->overlap_confirmed = confirmed;
    emit sourceReplacementChanged();
}

} // namespace exosnap::quick
