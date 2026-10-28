#include "EditSessionAdapter.h"
#include "EditTimelineModels.h"
#include "EditTrackLayoutModel.h"
#include <QCoreApplication>

#include "diagnostics/AppLog.h"
#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"
#include "services/BackgroundStorageBudget.h"
#include "settings/ConfigPaths.h"
#include <exosnap/engine/edit_timeline_export.h>

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QThread>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace exosnap::quick {

QUrl EditSessionAdapter::layoutSettingsLocation() const {
    return QUrl::fromLocalFile(QDir(settings::ResolveAppConfigDir()).filePath(QStringLiteral("editor-layout.ini")));
}

namespace {

const QString kEmptyValue = QStringLiteral("\xe2\x80\x94");

QVariantMap factRow(const QString& label, const QString& value) {
    QVariantMap row;
    row.insert(QStringLiteral("label"), label);
    // The rail renders an unset fact as the app-wide em dash rather than as an
    // empty cell, so an unknown value still reads as a value.
    row.insert(QStringLiteral("value"), value.isEmpty() ? kEmptyValue : value);
    return row;
}

} // namespace

int64_t SnapTrimBoundaryUs(int64_t requested_us, const std::vector<int64_t>& keyframes_us,
                           const std::vector<RecordingMarker>& markers) {
    int64_t snapped = requested_us;
    if (!keyframes_us.empty()) {
        auto it = std::upper_bound(keyframes_us.begin(), keyframes_us.end(), snapped);
        if (it != keyframes_us.begin())
            --it;
        snapped = *it;
    }
    for (const auto& marker : markers) {
        const auto marker_us = static_cast<int64_t>(marker.time_ms) * 1000LL;
        if (std::abs(marker_us - snapped) <= kMarkerSnapWindowUs)
            return marker_us;
    }
    return snapped;
}

EditSessionAdapter::EditSessionAdapter(QObject* parent) : QObject(parent) {
    track_layout_ = new EditTrackLayoutModel(this);
    media_model_ = new EditTimelineClipModel(this);
    folder_model_ = new EditFolderModel(layoutSettingsLocation().toLocalFile(), this);
    optional_io_budget_ = std::make_shared<BackgroundStorageBudget>();
    connect(this, &EditSessionAdapter::workspaceChanged, this, [this] { track_layout_->setTracks(tracks()); });
    connect(this, &EditSessionAdapter::workspaceChanged, this, [this] { syncMedia(); });
    connect(this, &EditSessionAdapter::workspaceChanged, this, [this] { syncOverview(); });
    connect(this, &EditSessionAdapter::workspaceChanged, this, &EditSessionAdapter::sourceAudioChoiceChanged);
    connect(this, &EditSessionAdapter::workspaceChanged, this, &EditSessionAdapter::sourceReplacementChanged);
    connect(this, &EditSessionAdapter::selectionChanged, this, &EditSessionAdapter::sourceReplacementChanged);
    connect(this, &EditSessionAdapter::selectionChanged, this, &EditSessionAdapter::sourceAudioChoiceChanged);
    connect(this, &EditSessionAdapter::rangeGestureChanged, this, &EditSessionAdapter::sourceReplacementChanged);
    connect(this, &EditSessionAdapter::rangeGestureChanged, this, &EditSessionAdapter::sourceAudioChoiceChanged);
    connect(this, &EditSessionAdapter::sourceVerificationChanged, this, [this] { emit sourceAudioChoiceChanged(); });
    connect(this, &EditSessionAdapter::workspaceChanged, this, &EditSessionAdapter::selectedClipSummaryChanged);
    connect(this, &EditSessionAdapter::selectionChanged, this, &EditSessionAdapter::selectedClipSummaryChanged);
    connect(this, &EditSessionAdapter::sourceVerificationChanged, this, [this] { emit selectedClipSummaryChanged(); });
    connect(this, &EditSessionAdapter::sourceVerificationChanged, this, [this] {
        if (dragActive())
            emit dragChanged();
    });
    connect(this, &EditSessionAdapter::dragChanged, this, [this] {
        track_layout_->setTracks(tracks());
        track_layout_->setPreviewTracks(dragTracks());
    });
    // One reader at a time: a second clip supersedes the first, and two parallel
    // container index reads on the same disk are slower than one.
    keyframe_pool_.setMaxThreadCount(1);
    metadata_pool_.setMaxThreadCount(1);
    verification_pool_.setMaxThreadCount(1);
    keyframe_pool_.setThreadPriority(QThread::LowestPriority);
    verification_pool_.setThreadPriority(QThread::LowestPriority);
}

EditSessionAdapter::~EditSessionAdapter() {
    source_replacement_cancelled_->store(true, std::memory_order_relaxed);
    source_audio_cancelled_->store(true, std::memory_order_relaxed);
    source_cancelled_->store(true, std::memory_order_relaxed);
    source_verification_cancelled_->store(true, std::memory_order_relaxed);
    import_cancelled_->store(true, std::memory_order_relaxed);
    keyframe_cancelled_->store(true, std::memory_order_relaxed);
    optional_io_budget_->Close();
    keyframe_pool_.clear();
    metadata_pool_.clear();
    verification_pool_.clear();
}

void EditSessionAdapter::scheduleSourceVerification(std::function<void()> task) {
    verification_pool_.start(std::move(task));
}

engine::BackgroundIoAdmission EditSessionAdapter::optionalIoAdmission() const {
    return [budget = optional_io_budget_, shared = background_io_](uint64_t bytes, const std::atomic<bool>* cancelled) {
        return budget->Acquire(bytes, cancelled) && (!shared || shared(bytes, cancelled));
    };
}

void EditSessionAdapter::setBackgroundAnalysisSuspended(bool suspended) {
    if (optional_io_budget_->Paused() == suspended)
        return;
    // A required verifier can share this source. Release an optional leader's
    // slot before pausing admission, rather than making the required job wait
    // for the pressure that it created to disappear.
    if (suspended)
        source_verification_cancelled_->store(true, std::memory_order_relaxed);
    else
        source_verification_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    optional_io_budget_->SetPaused(suspended);
    std::vector<edit::Id> retry;
    std::vector<edit::Id> changed;
    for (auto& [id, publication] : source_publications_) {
        if (publication.verifying) {
            publication.verifying = false;
            changed.push_back(id);
        }
        if (!suspended && publication.access && !publication.proof && !publication.failed && workspace_.asset(id))
            retry.push_back(id);
    }
    if (!changed.empty()) {
        syncMedia(true);
        for (const auto id : changed)
            emit sourceVerificationChanged(id);
    }
    for (const auto id : retry)
        retrySourceVerification(id);
}

void EditSessionAdapter::setEditContext(const EditContext& context, qint64 timeline_start_ms) {
    if (timeline_start_ms >= 0 && !FitsMilliseconds(timeline_start_ms)) {
        refuseEdit(tr("The timeline position is out of range."));
        return;
    }
    setEditContextUs(context, timeline_start_ms < 0 ? -1 : timeline_start_ms * 1000);
}

void EditSessionAdapter::setEditContextUs(const EditContext& context, qint64 timeline_start_us) {
    auto pending = context;
    if (pending.segments.empty())
        pending.segments.push_back({context.mkv_master_path.isEmpty() ? context.output_path : context.mkv_master_path,
                                    context.duration_seconds, true});
    // Existing media is probed once off the UI thread: the per-stream import
    // needs the exact stream inventory and content identity of each segment.
    const auto needs_probe = [](const auto& segment) { return segment.succeeded && QFileInfo::exists(segment.path); };
    if (std::any_of(pending.segments.begin(), pending.segments.end(), needs_probe)) {
        QPointer<EditSessionAdapter> self(this);
        const auto cancelled = import_cancelled_;
        const auto session = import_session_;
        beginImport();
        const auto admission = background_io_;
        metadata_pool_.start([self, pending = std::move(pending), timeline_start_us, needs_probe, cancelled, session,
                              admission]() mutable {
            std::vector<edit::SourceImport> imports(pending.segments.size());
            std::vector<SourcePublication> publications(pending.segments.size());
            for (size_t index = 0; index < pending.segments.size(); ++index) {
                if (cancelled->load(std::memory_order_relaxed))
                    break;
                auto& segment = pending.segments[index];
                if (!needs_probe(segment))
                    continue;
                const auto admitted =
                    engine::AcquireMediaReadCapability(std::filesystem::path(segment.path.toStdWString()));
                if (!admitted.succeeded()) {
                    segment.succeeded = false;
                    continue;
                }
                auto access = *admitted.value();
                auto metadata = engine::ProbeEditMedia(access, {}, admission, cancelled.get(), false);
                if (segment.duration_seconds <= 0) {
                    segment.duration_seconds = static_cast<double>(metadata.duration_us) / 1000000.0;
                    segment.succeeded = metadata.error.empty() && metadata.duration_us > 0;
                }
                imports[index] = edit::DescribeSourceImport(
                    metadata, static_cast<edit::Time>(std::llround(segment.duration_seconds * 1000000.0)), access);
                publications[index].access = std::move(access);
                publications[index].metadata = std::make_shared<const engine::EditMediaMetadata>(std::move(metadata));
            }
            if (self)
                QMetaObject::invokeMethod(
                    self,
                    [self, pending = std::move(pending), timeline_start_us, imports = std::move(imports),
                     publications = std::move(publications), cancelled, session]() mutable {
                        if (!self)
                            return;
                        if (!cancelled->load(std::memory_order_relaxed) && session == self->import_session_)
                            self->applyEditContextUs(pending, timeline_start_us, std::move(imports),
                                                     std::move(publications));
                        self->endImport();
                    },
                    Qt::QueuedConnection);
        });
        emit editPageRequested();
        return;
    }
    applyEditContextUs(pending, timeline_start_us, {});
}

void EditSessionAdapter::applyEditContextUs(const EditContext& context, qint64 timeline_start_us,
                                            std::vector<edit::SourceImport> imports,
                                            std::vector<SourcePublication> publications) {
    context_ = context;
    const auto previous_duration_us = workspace_.duration();
    const qint64 append_start = timeline_start_us < 0 ? workspace_.duration() : timeline_start_us;
    std::vector<edit::Id> assets;
    auto segments = context.segments;
    if (segments.empty())
        segments.push_back({context.mkv_master_path.isEmpty() ? context.output_path : context.mkv_master_path,
                            context.duration_seconds, true});
    uint64_t segment_start_ms = 0;
    for (size_t index = 0; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        edit::Asset asset;
        if (index < imports.size()) {
            asset.source_descriptor = std::move(imports[index].descriptor);
            asset.input_mix_selection = std::move(imports[index].input_mix);
            asset.mix_decision_required = imports[index].mix_decision_required;
        }
        asset.path = segment.path.toStdWString();
        asset.name = QFileInfo(segment.path).fileName().toStdString();
        asset.duration = static_cast<int64_t>(std::llround(segment.duration_seconds * 1000000.0));
        asset.width = context.resolution.section(QLatin1Char('x'), 0, 0).toInt();
        asset.height = context.resolution.section(QLatin1Char('x'), 1, 1).toInt();
        asset.fps = context.fps.section(QLatin1Char(' '), 0, 0).toDouble();
        if (asset.source_descriptor) {
            for (const auto& stream : asset.source_descriptor->streams)
                if (stream.ref.kind == engine::TimelineStreamKind::Video && stream.fps.valid()) {
                    asset.fps_num = static_cast<int>(stream.fps.num);
                    asset.fps_den = static_cast<int>(stream.fps.den);
                    break;
                }
        } else if (std::isfinite(asset.fps) && asset.fps >= 1 && asset.fps <= 1000 &&
                   std::floor(asset.fps) == asset.fps) {
            asset.fps_num = static_cast<int>(asset.fps);
            asset.fps_den = 1;
        }
        asset.state = !segment.succeeded                ? edit::AssetState::Failed
                      : QFileInfo::exists(segment.path) ? edit::AssetState::Available
                                                        : edit::AssetState::Missing;
        asset.markers =
            PartitionSegmentMarkers(context.markers, segment_start_ms, static_cast<uint64_t>(asset.duration / 1000));
        segment_start_ms += static_cast<uint64_t>(asset.duration / 1000);
        const auto id = workspace_.addAsset(std::move(asset));
        if (index < publications.size() && publications[index].access) {
            auto prior = source_publications_.find(id);
            if (prior == source_publications_.end() || prior->second.access != publications[index].access ||
                !prior->second.proof)
                source_publications_[id] = std::move(publications[index]);
            source_publications_[id].version = workspace_.asset(id)->source_version;
        }
        if (segment.duration_seconds > 0)
            assets.push_back(id);
    }
    if (!assets.empty()) {
        if (!workspace_.insertAssets(assets, append_start, autoCrossfade))
            workspace_error_ = tr("The recording overlaps existing clips.");
        else
            workspace_error_.clear();
    }
    if (std::any_of(segments.begin(), segments.end(),
                    [](const auto& segment) { return segment.duration_seconds <= 0; }))
        workspace_error_ = tr("Some segments have unknown duration. They remain in Media and cannot be placed yet.");
    workspace_.seek(append_start);
    ++clip_generation_;

    const qint64 duration = workspace_.duration() / 1000;
    const bool duration_changed = workspace_.duration() != previous_duration_us;
    duration_ms_ = duration;

    keyframe_timestamps_.clear();
    trim_snap_ready_ = false;
    trim_start_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    trim_end_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    position_ms_ = workspace_.playhead() / 1000;

    applyReport(context_);
    rebuildFacts();
    loadMarkers();

    const bool was_open = open_;
    open_ = !context_.mkv_master_path.isEmpty();

    emit clipChanged();
    if (duration_changed)
        emit durationChanged();
    emit trimChanged();
    emit trimSnapReadyChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    if (was_open != open_)
        emit openChanged();

    if (open_) {
        emit clipOpened(context_.mkv_master_path, static_cast<qint64>(context_.duration_seconds * 1000.0));
        startKeyframeScan();
    } else {
        emit clipClosed();
    }
    syncSelection();
    emit workspaceChanged();
    for (const auto id : assets)
        if (sourceAccess(id) && !sourceProof(id))
            retrySourceVerification(id);
    emit editPageRequested();
}

void EditSessionAdapter::setDeliveryPending(bool pending) {
    if (context_.delivery_pending == pending)
        return;
    context_.delivery_pending = pending;
    emit clipChanged();
}

const EditContext& EditSessionAdapter::editContext() const noexcept {
    return context_;
}

bool EditSessionAdapter::open() const noexcept {
    return open_;
}

QString EditSessionAdapter::clipTitle() const {
    const QString& path = context_.output_path;
    const int separator = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    return separator >= 0 ? path.mid(separator + 1) : path;
}

const QString& EditSessionAdapter::clipPath() const noexcept {
    return context_.output_path;
}

QString EditSessionAdapter::playerMetaText() const {
    QStringList parts;
    for (const QString& part : {context_.resolution, context_.fps, context_.container}) {
        if (!part.isEmpty())
            parts.append(part);
    }
    return parts.join(QStringLiteral("  "));
}

const QVariantList& EditSessionAdapter::facts() const noexcept {
    return facts_;
}

qint64 EditSessionAdapter::durationMs() const noexcept {
    return duration_ms_;
}

qint64 EditSessionAdapter::trimStartMs() const noexcept {
    return trim_start_us_ != exosnap::engine::TrimRange::kNoTimestamp ? trim_start_us_ / 1000 : 0;
}

qint64 EditSessionAdapter::trimEndMs() const noexcept {
    return trim_end_us_ != exosnap::engine::TrimRange::kNoTimestamp ? trim_end_us_ / 1000 : duration_ms_;
}

bool EditSessionAdapter::trimmed() const noexcept {
    return duration_ms_ > 0 && (trim_start_us_ != exosnap::engine::TrimRange::kNoTimestamp ||
                                trim_end_us_ != exosnap::engine::TrimRange::kNoTimestamp);
}

bool EditSessionAdapter::trimSnapReady() const noexcept {
    return trim_snap_ready_;
}

qint64 EditSessionAdapter::positionMs() const noexcept {
    return position_ms_;
}

int64_t EditSessionAdapter::trimStartUs() const noexcept {
    return trim_start_us_;
}

int64_t EditSessionAdapter::trimEndUs() const noexcept {
    return trim_end_us_;
}

const std::vector<RecordingMarker>& EditSessionAdapter::markers() const noexcept {
    return markers_;
}

const std::vector<int64_t>& EditSessionAdapter::keyframeTimestamps() const noexcept {
    return keyframe_timestamps_;
}

int EditSessionAdapter::reportSeverityValue() const noexcept {
    return static_cast<int>(report_severity_);
}

const QString& EditSessionAdapter::reportLabel() const noexcept {
    return report_label_;
}

QString EditSessionAdapter::reportTooltip() const {
    return QStringLiteral("%1\n%2\n%3").arg(report_drops_text_, report_drift_text_, report_health_text_);
}

bool EditSessionAdapter::exportRunning() const noexcept {
    return export_running_;
}

void EditSessionAdapter::setExportRunning(bool running) {
    if (export_running_ == running)
        return;
    export_running_ = running;
    emit exportRunningChanged();
}

bool EditSessionAdapter::hasUnsavedEdits() const {
    return trimmed() || !markers_.empty();
}

void EditSessionAdapter::requestTrim(qint64 start_ms, qint64 end_ms) {
    if (duration_ms_ <= 0)
        return;

    // Clamp first (handles never cross, minimum gap holds), then snap. Doing it
    // the other way round would let a snap push a handle past its neighbour.
    const qint64 clamped_end = ClampTrimEndMs(end_ms, start_ms, duration_ms_);
    const qint64 clamped_start = ClampTrimStartMs(start_ms, clamped_end);

    const int64_t start_us = clamped_start <= 0
                                 ? exosnap::engine::TrimRange::kNoTimestamp
                                 : SnapTrimBoundaryUs(clamped_start * 1000, keyframe_timestamps_, markers_);
    const int64_t end_us = clamped_end >= duration_ms_
                               ? exosnap::engine::TrimRange::kNoTimestamp
                               : SnapTrimBoundaryUs(clamped_end * 1000, keyframe_timestamps_, markers_);

    // Which handle moved, from the values BEFORE this call. The previous rule was
    // `clamped_start <= 0 ? end_us : start_us` -- a test on the in-point's value,
    // not on what the user dragged. With an in-point already set, dragging the
    // out-point seeked to the in-point instead: the preview jumped to the start
    // of the range while the user was looking for the frame they were cutting at.
    const bool start_moved = start_us != trim_start_us_;
    const bool end_moved = end_us != trim_end_us_;

    setTrimUs(start_us, end_us);
    if (const auto* selected = workspace_.clip(workspace_.selection())) {
        const auto id = selected->id;
        const auto* asset = workspace_.asset(selected->asset);
        const auto in = start_us == engine::TrimRange::kNoTimestamp ? 0 : start_us;
        const auto out = end_us == engine::TrimRange::kNoTimestamp ? asset->duration : end_us;
        if (workspace_.trim(id, in, out)) {
            workspace_.move(id, 0);
            emit workspaceChanged();
        }
    }

    // Show the frame at the boundary that actually moved. When both moved (a
    // clamp pushed the neighbour, or the range was set at once) the in-point is
    // the answer, which is where the range now begins.
    int64_t shown_us = exosnap::engine::TrimRange::kNoTimestamp;
    if (start_moved)
        shown_us = start_us;
    else if (end_moved)
        shown_us = end_us;
    if (shown_us != exosnap::engine::TrimRange::kNoTimestamp) {
        emit seekRequestedUs(shown_us);
        emit seekRequested(shown_us / 1000);
    }
}

void EditSessionAdapter::requestSeek(qint64 position_ms) {
    const qint64 clamped = ClampPlayheadMs(position_ms, duration_ms_);
    requestSeekUs(clamped * 1000);
}

void EditSessionAdapter::requestSeekUs(qint64 position_us) {
    setPositionUs(position_us);
    const auto clamped = workspace_.playhead();
    emit seekRequestedUs(clamped);
    emit seekRequested(clamped / 1000);
}

void EditSessionAdapter::setPositionMs(qint64 position_ms) {
    const qint64 clamped = ClampPlayheadMs(position_ms, duration_ms_);
    setPositionUs(clamped * 1000);
}

void EditSessionAdapter::setPositionUs(qint64 position_us) {
    const auto previous = workspace_.playhead();
    workspace_.seek(position_us);
    const auto clamped = workspace_.playhead();
    if (clamped == previous)
        return;
    position_ms_ = clamped / 1000;
    emit positionChanged();
}

// Explicitly clear the workspace and release decoder resources. Navigation never calls this.
void EditSessionAdapter::close() {
    cancelSourceReplacement();
    cancelSourceAudioChoice();
    source_cancelled_->store(true, std::memory_order_relaxed);
    source_verification_cancelled_->store(true, std::memory_order_relaxed);
    source_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    source_verification_cancelled_ = std::make_shared<std::atomic<bool>>(optional_io_budget_->Paused());
    pointer_selection_.reset();
    cancelPendingDraft(tr("The editor was closed."));
    revokePendingImports();
    keyframe_cancelled_->store(true, std::memory_order_relaxed);
    pending_media_drop_ = {};
    source_publications_.clear();
    source_versions_.clear();
    if (!pending_placement_assets_.empty()) {
        pending_placement_assets_.clear();
        emit pendingPlacementChanged();
    }
    media_revision_ = (std::numeric_limits<edit::Revision>::max)();
    // A fixture context carries a duration but no master path, and must close
    // just as completely as a real clip -- hence not `open_` alone.
    const bool had_clip = open_ || duration_ms_ > 0 || !context_.output_path.isEmpty() ||
                          !workspace_.assets().empty() || !workspace_.clips().empty();
    if (!had_clip) {
        emit closeRequested();
        return;
    }

    const bool was_open = open_;
    // A keyframe scan still running for this clip belongs to a generation nobody
    // is showing any more; advancing the counter drops its result on arrival.
    ++clip_generation_;
    context_ = EditContext{};
    cancelClipDrag();
    cancelRangeGesture();
    if (!solo_tracks_.empty()) {
        solo_tracks_.clear();
        emit auditionChanged();
    }
    selection_.clear();
    time_selection_in_us_ = time_selection_out_us_ = -1;
    selected_gap_ = {};
    selected_gap_track_ = selected_spacer_ = selected_transition_ = 0;
    copied_blank_duration_ = 0;
    emit timeSelectionChanged();
    workspace_ = edit::Workspace{};
    syncSelection();
    track_layout_->reset();
    emit workspaceChanged();
    keyframe_timestamps_.clear();
    trim_snap_ready_ = false;
    trim_start_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    trim_end_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    duration_ms_ = 0;
    position_ms_ = 0;
    open_ = false;

    applyReport(context_);
    rebuildFacts();
    loadMarkers();

    emit clipChanged();
    emit durationChanged();
    emit trimChanged();
    emit trimSnapReadyChanged();
    emit positionChanged();
    emit unsavedEditsChanged();
    if (was_open)
        emit openChanged();

    emit clipClosed();
    emit closeRequested();
}

QString EditSessionAdapter::formatTimestamp(qint64 position_ms) const {
    return FormatTimelineTimestamp(position_ms, duration_ms_);
}

QString EditSessionAdapter::formatClock(qint64 position_ms) const {
    return FormatTimelineClock(position_ms, duration_ms_);
}

qint64 EditSessionAdapter::clampTrimStartMs(qint64 requested_ms, qint64 trim_end_ms) const {
    return ClampTrimStartMs(requested_ms, trim_end_ms);
}

qint64 EditSessionAdapter::clampTrimEndMs(qint64 requested_ms, qint64 trim_start_ms) const {
    return ClampTrimEndMs(requested_ms, trim_start_ms, duration_ms_);
}

void EditSessionAdapter::setKeyframeTimestampsForTest(std::vector<int64_t> keyframes_us) {
    keyframe_timestamps_ = std::move(keyframes_us);
    std::sort(keyframe_timestamps_.begin(), keyframe_timestamps_.end());
    if (!trim_snap_ready_) {
        trim_snap_ready_ = true;
        emit trimSnapReadyChanged();
    }
}

void EditSessionAdapter::setTrimUs(int64_t start_us, int64_t end_us) {
    if (trim_start_us_ == start_us && trim_end_us_ == end_us)
        return;
    const bool had_edits = hasUnsavedEdits();
    trim_start_us_ = start_us;
    trim_end_us_ = end_us;
    emit trimChanged();
    if (had_edits != hasUnsavedEdits())
        emit unsavedEditsChanged();
}

void EditSessionAdapter::setAssetMonitorSelection(edit::Id asset, engine::MonitorSelectionSnapshot selection) {
    // A source whose starting mix is still undecided takes the first confirmed
    // monitoring choice as that decision. Unverified track identities are new on
    // every open, so they are matched by their audio-stream order instead.
    std::vector<engine::AudioTrackId> chosen;
    size_t requested = 0;
    const auto* source = workspace_.asset(asset);
    const bool decide = source && source->mix_decision_required && source->source_descriptor;
    if (decide && rangeScopeReviewActive()) {
        refuseEdit(tr("Apply or cancel the track scope review before continuing."));
        return;
    }
    if (decide) {
        std::vector<engine::AudioTrackId> streams;
        for (const auto& stream : source->source_descriptor->streams)
            if (stream.ref.kind == engine::TimelineStreamKind::Audio)
                streams.push_back(stream.ref.stream_identity);
        for (const auto& track : selection.tracks) {
            if (track.muted || track.gain_linear <= 0)
                continue;
            ++requested;
            if (selection.unverified_asset_binding) {
                const auto& actual = selection.unverified_asset_binding->actual_tracks;
                const auto match = std::find_if(actual.begin(), actual.end(),
                                                [&](const auto& value) { return value.track_id == track.track_id; });
                if (match != actual.end() && match->stream_index.value < streams.size())
                    chosen.push_back(streams[match->stream_index.value]);
            } else if (std::find(streams.begin(), streams.end(), track.track_id) != streams.end()) {
                chosen.push_back(track.track_id);
            }
        }
    }
    if (!workspace_.setMonitorSelection(asset, std::move(selection)))
        return;
    // A selection that names a stream this source does not have leaves the
    // decision open instead of resolving it to fewer streams than were chosen.
    if (decide && chosen.size() == requested)
        workspace_.resolveInputMix(asset, chosen);
    emit workspaceChanged();
}

void EditSessionAdapter::applyReport(const EditContext& context) {
    const auto& snapshot = context.completed_snapshot;
    const bool has_snapshot = snapshot.valid || snapshot.session_generation > 0;

    // REAL drops only (encoder backpressure plus frame-processing failures) --
    // not deliberate CFR pacing/coalescing, which is intentional frame
    // selection, not a drop. frames_dropped_problem() is the shared definition.
    const uint64_t dropped = snapshot.capture.frames_dropped_problem();
    const uint64_t total = snapshot.capture.frames_emitted + dropped;
    if (has_snapshot && total > 0) {
        const double percent = 100.0 * static_cast<double>(dropped) / static_cast<double>(total);
        report_drops_text_ =
            QCoreApplication::translate("EditSessionAdapter", "Frame drops: %1%").arg(percent, 0, 'f', 1);
    } else {
        report_drops_text_ = QCoreApplication::translate("EditSessionAdapter", "Frame drops: %1").arg(kEmptyValue);
    }

    if (context.av_drift_available) {
        report_drift_text_ = QCoreApplication::translate("EditSessionAdapter", "Peak A/V drift: \xc2\xb1%1\xc2\xa0ms")
                                 .arg(context.peak_av_drift_ms, 0, 'f', 0);
    } else {
        report_drift_text_ = QCoreApplication::translate("EditSessionAdapter", "Peak A/V drift: %1").arg(kEmptyValue);
    }

    // The label is the verdict itself in every case, not only in the two that go
    // wrong. It used to be empty unless the pipeline reported Warning or
    // Critical, so the header element showed the word "Report" the rest of the
    // time — one element switching between reading as an ACTION ("open the
    // report") and as a STATUS ("this recording has a warning"), which is
    // exactly the ambiguity a header must not carry. Now it always states a
    // verdict and the severity decides only how loudly.
    report_severity_ = ReportSeverity::Neutral;
    if (has_snapshot) {
        QString health = QCoreApplication::translate("EditSessionAdapter", "Unknown");
        switch (snapshot.health) {
        case exosnap::engine::PipelineHealth::Good:
            health = QCoreApplication::translate("EditSessionAdapter", "Good");
            break;
        case exosnap::engine::PipelineHealth::Warning:
            health = QCoreApplication::translate("EditSessionAdapter", "Warning");
            report_severity_ = ReportSeverity::Warning;
            break;
        case exosnap::engine::PipelineHealth::Critical:
            health = QCoreApplication::translate("EditSessionAdapter", "Critical");
            report_severity_ = ReportSeverity::Critical;
            break;
        case exosnap::engine::PipelineHealth::Unavailable:
            health = QCoreApplication::translate("EditSessionAdapter", "Unavailable");
            break;
        default:
            break;
        }
        report_label_ = health;
        report_health_text_ = QCoreApplication::translate("EditSessionAdapter", "Pipeline health: %1").arg(health);
    } else {
        report_label_ = kEmptyValue;
        report_health_text_ = QCoreApplication::translate("EditSessionAdapter", "Pipeline health: %1").arg(kEmptyValue);
    }
    emit reportChanged();
}

void EditSessionAdapter::rebuildFacts() {
    facts_.clear();
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Duration"), context_.duration));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Size"), context_.size));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Resolution"), context_.resolution));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Frame rate"), context_.fps));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Video"), context_.video_codec));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Audio"), context_.audio_codec));
    facts_.append(factRow(QCoreApplication::translate("EditSessionAdapter", "Container"), context_.container));
}

void EditSessionAdapter::loadMarkers() {
    markers_ = context_.markers;
    if (context_.marker_sidecar_path.isEmpty())
        return;
    const QPointer<EditSessionAdapter> self(this);
    const auto generation = clip_generation_;
    const auto cancelled = source_cancelled_;
    metadata_pool_.start([self, generation, cancelled, path = context_.marker_sidecar_path] {
        if (cancelled->load(std::memory_order_relaxed))
            return;
        const std::filesystem::path sidecar(path.toStdWString());
        std::error_code ec;
        if (!std::filesystem::exists(sidecar, ec))
            return;
        int skipped = 0;
        auto markers = ReadMarkerSidecar(sidecar, &skipped);
        if (self)
            QMetaObject::invokeMethod(
                self,
                [self, generation, cancelled, path, markers = std::move(markers), skipped]() mutable {
                    if (!self || cancelled->load(std::memory_order_relaxed) || generation != self->clip_generation_)
                        return;
                    const bool had_unsaved_edits = self->hasUnsavedEdits();
                    self->markers_ = std::move(markers);
                    if (skipped > 0)
                        diagnostics::AppLog::warning(
                            QStringLiteral("edit"),
                            QCoreApplication::translate("EditSessionAdapter",
                                                        "Skipped %1 marker(s) with an unusable time in %2")
                                .arg(skipped)
                                .arg(path));
                    emit self->markersChanged();
                    if (self && had_unsaved_edits != self->hasUnsavedEdits())
                        emit self->unsavedEditsChanged();
                },
                Qt::QueuedConnection);
    });
}

// Reading a container's full index is proportional to the recording's length,
// and the Widgets surface did it inline in setEditContext -- a long recording
// froze the GUI thread on open. Off-thread here; a trim released before it lands
// still snaps to markers, and re-snaps as soon as the table arrives.
void EditSessionAdapter::startKeyframeScan(std::shared_ptr<const engine::MediaReadCapability> source) {
    keyframe_cancelled_->store(true, std::memory_order_relaxed);
    keyframe_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    const auto cancelled = keyframe_cancelled_;
    const auto admission = optionalIoAdmission();
    const auto generation = clip_generation_;
    const auto master = QDir::fromNativeSeparators(context_.mkv_master_path);
    auto access = std::move(source);
    for (const auto& [id, publication] : source_publications_)
        if (!access && publication.access &&
            QDir::fromNativeSeparators(QString::fromStdWString(publication.access->Path().wstring()))
                    .compare(master, Qt::CaseInsensitive) == 0) {
            access = publication.access;
            break;
        }
    QPointer<EditSessionAdapter> self(this);
    keyframe_pool_.start([self, generation, master, cancelled, admission, access = std::move(access)]() mutable {
        if (!access && !cancelled->load(std::memory_order_relaxed)) {
            const auto acquired = engine::AcquireMediaReadCapability(std::filesystem::path(master.toStdWString()));
            if (acquired.succeeded())
                access = *acquired.value();
        }
        auto keyframes = engine::ExtractKeyframeTimestamps(access, admission, cancelled.get());
        if (!self || cancelled->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(
            self,
            [self, generation, cancelled, keyframes = std::move(keyframes)]() mutable {
                if (!self || cancelled->load(std::memory_order_relaxed) || generation != self->clip_generation_)
                    return;
                self->keyframe_timestamps_ = std::move(keyframes);
                self->trim_snap_ready_ = true;
                emit self->trimSnapReadyChanged();
            },
            Qt::QueuedConnection);
    });
}

} // namespace exosnap::quick
