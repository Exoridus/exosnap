#include "EditPlayerWorker.h"
#include <QScopeGuard>
#include <exosnap/engine/timeline_audio_queue.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

// Fallback tick, used only until a clip is open (or when its container declares
// no usable frame rate).
constexpr int kWorkerFallbackTickMs = 16;

std::optional<int64_t> AddTime(int64_t a, int64_t b) {
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
        return std::nullopt;
    return a + b;
}

std::optional<int64_t> SubtractTime(int64_t a, int64_t b) {
    if ((b > 0 && a < std::numeric_limits<int64_t>::min() + b) ||
        (b < 0 && a > std::numeric_limits<int64_t>::max() + b))
        return std::nullopt;
    return a - b;
}

int64_t SampleAt(int64_t time_us) {
    return time_us / 1000 * 48 + (time_us % 1000 * 48 + 500) / 1000;
}

// The front-most visible clip. Evaluation lists contributions back to front.
const edit::Clip* EvaluatedClip(const edit::Workspace& workspace, int64_t time_us) {
    const auto evaluation = engine::EvaluateTimeline(workspace.timeline(), time_us);
    return evaluation.error.empty() && !evaluation.video.empty() ? workspace.clip(evaluation.video.back().clip)
                                                                 : nullptr;
}

// The single-clip session shows exactly one decoded source at full opacity.
// Junctions, partial opacity, video edge fades and simultaneously visible
// video clips need the compositing reader instead.
bool NeedsComposition(const edit::Workspace& workspace) {
    if (!workspace.transitions().empty())
        return true;
    std::vector<const edit::Clip*> video;
    for (const auto& clip : workspace.clips()) {
        const auto track = std::find_if(workspace.tracks().begin(), workspace.tracks().end(),
                                        [&clip](const auto& value) { return value.id == clip.track; });
        if (track == workspace.tracks().end() || track->type != edit::TrackType::Video || !track->edit_enabled ||
            !clip.enabled)
            continue;
        if (clip.opacity != 1 || clip.fade_in.duration_us > 0 || clip.fade_out.duration_us > 0)
            return true;
        video.push_back(&clip);
    }
    std::sort(video.begin(), video.end(), [](const auto* a, const auto* b) { return a->start < b->start; });
    int64_t reach = std::numeric_limits<int64_t>::min();
    for (const auto* clip : video) {
        if (clip->start < reach)
            return true;
        reach = std::max(reach, clip->end());
    }
    return false;
}

// Display geometry of an empty composition: the timeline output, else the
// first video source. Only its aspect ratio is visible.
engine::FrameSize BlankGeometry(const edit::Workspace& workspace) {
    const auto even = [](int width, int height) {
        return engine::FrameSize{static_cast<uint32_t>(std::max(2, width & ~1)),
                                 static_cast<uint32_t>(std::max(2, height & ~1))};
    };
    const auto timeline = workspace.timeline();
    if (timeline.output_width > 0 && timeline.output_height > 0)
        return even(static_cast<int>(timeline.output_width), static_cast<int>(timeline.output_height));
    for (const auto& asset : workspace.assets())
        if (asset.width > 0 && asset.height > 0)
            return even(asset.width, asset.height);
    return {2, 2};
}

} // namespace

EditPlayerWorker::EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink) : sink_(std::move(sink)) {
}

EditPlayerWorker::~EditPlayerWorker() {
    close();
}

QVariantMap EditPlayerWorker::performanceSnapshot() const {
    auto reads = retired_seek_performance_;
    if (session_) {
        const auto active = session_->SeekPerformanceSnapshot();
        reads.read_count += active.read_count;
        reads.read_ns += active.read_ns;
    }
    return {{"timelineUpdates", QVariant::fromValue(timeline_updates_)},
            {"timelineRevision", QVariant::fromValue(workspace_.revision())},
            {"timelinePlayheadUs", workspace_.playhead()},
            {"sessionOpens", QVariant::fromValue(session_opens_)},
            {"sessionCloses", QVariant::fromValue(session_closes_)},
            {"sessionOpenNs", session_open_ns_},
            {"sessionCloseNs", session_close_ns_},
            {"seekDispatches", QVariant::fromValue(seek_dispatches_)},
            {"seekDispatchNs", seek_dispatch_ns_},
            {"timelineQueueWaitNs", timeline_queue_wait_ns_},
            {"seekQueueWaitNs", seek_queue_wait_ns_},
            {"decoderReadCount", QVariant::fromValue(reads.read_count)},
            {"decoderReadNs", reads.read_ns}};
}

void EditPlayerWorker::recordQueueWait(qint64 elapsed_ns, bool timeline) {
    (timeline ? timeline_queue_wait_ns_ : seek_queue_wait_ns_) += elapsed_ns;
}

void EditPlayerWorker::open(const QString& master_path, qint64 duration_ms, double screen_hz) {
    close();
    duration_ms_ = duration_ms;
    screen_hz_ = screen_hz;
    session_ = std::make_unique<exosnap::engine::EditPlayerSession>();
    std::string error;
    QElapsedTimer open_clock;
    open_clock.start();
    ++session_opens_;
    const bool opened = session_->Open(std::filesystem::path(master_path.toStdWString()), error);
    session_open_ns_ += open_clock.nsecsElapsed();
    if (!opened) {
        session_.reset();
        publishOpenResult(false, QString::fromStdString(error));
        return;
    }
    // Delivered straight from the decode/seek thread into the scene-graph item,
    // with no UI-thread hop: the item's mailbox is thread-safe by design, and a
    // per-frame hop would undo the point of the GPU path.
    auto sink = sink_;
    const auto* mapped_clip = workspace_.clip(active_clip_);
    const auto timeline_offset =
        mapped_clip ? SubtractTime(mapped_clip->start, mapped_clip->source_in) : std::optional<int64_t>{0};
    if (!timeline_offset) {
        close();
        publishOpenResult(false, tr("The source time is outside the supported range."));
        return;
    }
    session_->SetOnFrameReadyWithGeneration(
        [sink, offset = *timeline_offset](exosnap::engine::RawDecodedVideoFrame frame, uint64_t generation) {
            sink->deliver(std::move(frame), generation, offset);
        });
    active_timeline_offset_ = *timeline_offset;
    session_->SetVolume(static_cast<float>(volume_));
    const auto* clip = workspace_.clip(active_clip_);
    const auto* asset = clip ? workspace_.asset(clip->asset) : nullptr;
    if (asset && asset->monitor_selection) {
        monitor_revision_ = std::max(monitor_revision_, asset->monitor_selection->revision.value);
        if (!session_->SetMonitorSelection(*asset->monitor_selection, error))
            emit monitoringRejected(QString::fromStdString(error));
    }
    if (asset) {
        active_source_ = *asset;
        active_stream_ = clip->stream;
        std::error_code stamp_error;
        active_source_size_ = std::filesystem::file_size(asset->path, stamp_error);
        if (!stamp_error)
            active_source_write_time_ = std::filesystem::last_write_time(asset->path, stamp_error);
        if (stamp_error)
            active_source_.reset();
    }
    publishMonitoring();
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    // Poster frame: show the clip's first frame instead of the placeholder while
    // the user is still reviewing. Publishing the clock matters here too -- a
    // second clip opened over a first would otherwise leave the gate holding the
    // previous clip's value and drop this very frame.
    session_->SeekTo(0, request_generation_);
    syncClock();
    publishOpenResult(true, {});
}

void EditPlayerWorker::close() {
    active_source_.reset();
    active_stream_.reset();
    timeline_playing_ = false;
    publishPlaybackState(false);
    if (render_audio_)
        render_audio_->Shutdown();
    render_audio_.reset();
    render_reader_.reset();
    render_snapshot_ = {};
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (session_) {
        QElapsedTimer close_clock;
        close_clock.start();
        session_->SetOnFrameReady({});
        session_->Close();
        const auto reads = session_->SeekPerformanceSnapshot();
        retired_seek_performance_.read_count += reads.read_count;
        retired_seek_performance_.read_ns += reads.read_ns;
        session_.reset();
        ++session_closes_;
        session_close_ns_ += close_clock.nsecsElapsed();
    }
    position_ms_ = 0;
    position_us_ = 0;
    sink_->publishClock(-1);
    emit monitoringChanged({}, false);
}

void EditPlayerWorker::play(qint64 from_ms) {
    from_ms = std::max<qint64>(from_ms, 0);
    if (from_ms > std::numeric_limits<qint64>::max() / 1000) {
        publishOpenResult(false, tr("The requested time is outside the supported range."));
        return;
    }
    playUs(from_ms * 1000);
}

void EditPlayerWorker::playUs(qint64 from_us) {
    from_us = std::max<qint64>(from_us, 0);
    if (timeline_mode_) {
        seekTimeline(from_us >= workspace_.duration() ? 0 : from_us, true);
        return;
    }
    if (!session_)
        return;
    if (session_->MonitoringConfirmationRequired()) {
        publishMonitoring();
        emit monitoringRejected(tr("Confirm the audio monitoring selection before playback."));
        return;
    }
    position_us_ = from_us;
    position_ms_ = from_us / 1000;
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    elapsed_.restart();
    tick_timer_->start();
    session_->Play(position_us_, request_generation_);
    publishPlaybackState(true);
    syncClock();
}

void EditPlayerWorker::pause() {
    publishPlaybackState(false);
    timeline_playing_ = false;
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (render_audio_)
        render_audio_->Stop();
    if (!session_)
        return;
    session_->Pause();
    syncClock();
}

void EditPlayerWorker::seek(qint64 position_ms) {
    if (position_ms > std::numeric_limits<qint64>::max() / 1000) {
        publishOpenResult(false, tr("The requested time is outside the supported range."));
        return;
    }
    seekUs(std::max<qint64>(position_ms, 0) * 1000);
}

void EditPlayerWorker::seekUs(qint64 target_us) {
    QElapsedTimer dispatch_clock;
    dispatch_clock.start();
    ++seek_dispatches_;
    const auto record = qScopeGuard([&]() { seek_dispatch_ns_ += dispatch_clock.nsecsElapsed(); });
    pause();
    target_us = std::max<qint64>(target_us, 0);
    if (timeline_mode_) {
        seekTimeline(target_us, false);
        return;
    }
    position_us_ = target_us;
    position_ms_ = target_us / 1000;
    if (!session_)
        return;
    session_->SeekTo(target_us, request_generation_);
    syncClock();
}
void EditPlayerWorker::setScreenRefreshHz(double screen_hz) {
    screen_hz_ = screen_hz;
    if (tick_timer_ != nullptr && session_)
        tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
}

void EditPlayerWorker::publishOpenResult(bool opened, const QString& error) {
    if (!opened)
        pause();
    emit openFinishedForGeneration(request_generation_, opened, error);
    emit openFinished(opened, error);
}

void EditPlayerWorker::publishPlaybackState(bool playing) {
    if (published_playing_ == playing)
        return;
    published_playing_ = playing;
    emit playbackStateChanged(playing);
}

void EditPlayerWorker::ensureTimer() {
    if (tick_timer_ != nullptr)
        return;
    tick_timer_ = new QTimer(this);
    // Coarse timers are allowed a 5% slop, which at 144 fps (6 ms) is most of a
    // frame.
    tick_timer_->setTimerType(Qt::PreciseTimer);
    tick_timer_->setInterval(kWorkerFallbackTickMs);
    connect(tick_timer_, &QTimer::timeout, this, &EditPlayerWorker::onTick);
}

void EditPlayerWorker::syncClock() {
    sink_->publishClock(session_ ? session_->ClockSnapshotUs() : -1);
}

void EditPlayerWorker::onTick() {
    if (render_mode_) {
        tickRenderTimeline();
        return;
    }
    if (timeline_mode_) {
        if (!timeline_playing_)
            return;
        const auto* clip = workspace_.clip(active_clip_);
        if (clip && session_) {
            const auto offset = SubtractTime(clip->start, clip->source_in);
            const auto timeline_time = offset ? AddTime(session_->CurrentPositionUs(), *offset) : std::nullopt;
            if (!timeline_time) {
                pause();
                publishOpenResult(false, tr("The source time is outside the supported range."));
                return;
            }
            position_us_ = *timeline_time;
            position_ms_ = position_us_ / 1000;
            syncClock();
        } else {
            position_us_ += elapsed_.restart() * 1000;
            position_ms_ = position_us_ / 1000;
        }
        const auto* next = EvaluatedClip(workspace_, position_us_);
        if ((next ? next->id : 0) != active_clip_)
            seekTimeline(position_us_, true);
        else if (!next && position_us_ < workspace_.duration())
            presentBlank(position_us_);
        position_us_ = std::min<qint64>(position_us_, workspace_.duration());
        position_ms_ = position_us_ / 1000;
        emit positionAdvancedUs(position_us_);
        emit positionAdvanced(position_ms_);
        if (position_us_ >= workspace_.duration()) {
            pause();
            emit reachedEnd();
        }
        return;
    }
    if (!session_)
        return;
    const qint64 total = std::max<qint64>(duration_ms_, 0);
    position_us_ = std::clamp<qint64>(session_->CurrentPositionUs(), 0, total * 1000);
    position_ms_ = position_us_ / 1000;
    syncClock();
    emit positionAdvancedUs(position_us_);
    emit positionAdvanced(position_ms_);
    if (total > 0 && position_ms_ >= total) {
        tick_timer_->stop();
        session_->Pause();
        publishPlaybackState(false);
        syncClock();
        emit reachedEnd();
    }
}

void EditPlayerWorker::setTimeline(edit::Workspace workspace) {
    ++timeline_updates_;
    const auto makeSnapshot = [this](const edit::Workspace& value) {
        engine::TimelineRenderSnapshot snapshot;
        snapshot.timeline = value.timeline();
        for (const auto& asset : value.assets()) {
            const std::filesystem::path path(asset.path);
            const auto previous =
                std::find_if(render_snapshot_.sources.begin(), render_snapshot_.sources.end(),
                             [&](const auto& source) { return source.id == asset.id && source.path == path; });
            auto metadata =
                previous != render_snapshot_.sources.end() ? previous->metadata : engine::ProbeEditMedia(path);
            snapshot.sources.push_back({asset.id, path, std::move(metadata), asset.monitor_selection});
        }
        return snapshot;
    };
    const bool use_render = NeedsComposition(workspace);
    if (timeline_mode_ && !render_mode_ && !use_render) {
        pause();
        workspace_ = std::move(workspace);
        seekTimeline(workspace_.playhead(), false);
        return;
    }
    if (timeline_mode_ && render_mode_ && use_render && render_reader_) {
        auto snapshot = makeSnapshot(workspace);
        const auto plan =
            engine::ClassifyEditExport(snapshot, {engine::TimelineExportEligibility::RenderRequired, {}}, false);
        std::string error;
        if (!render_reader_->UpdateSnapshot(snapshot, plan, error)) {
            pause();
            publishOpenResult(false, QString::fromStdString(error));
            return;
        }
        workspace_ = std::move(workspace);
        render_snapshot_ = std::move(snapshot);
        render_plan_ = plan;
        if (!timeline_playing_) {
            position_us_ = std::clamp<qint64>(workspace_.playhead(), 0, workspace_.duration());
            position_ms_ = position_us_ / 1000;
            renderTimeline(position_us_);
        }
        return;
    }
    pause();
    close();
    sink_->clear();
    workspace_ = std::move(workspace);
    timeline_mode_ = true;
    active_clip_ = 0;
    render_mode_ = use_render;
    if (render_mode_) {
        auto snapshot = makeSnapshot(workspace_);
        render_plan_ =
            engine::ClassifyEditExport(snapshot, {engine::TimelineExportEligibility::RenderRequired, {}}, false);
        std::string error;
        render_reader_ = std::make_unique<engine::EditTimelineReader>();
        if (!render_reader_->Open(snapshot, render_plan_, error)) {
            render_reader_.reset();
            publishOpenResult(false, QString::fromStdString(error));
            return;
        }
        render_snapshot_ = std::move(snapshot);
        publishOpenResult(true, {});
    }
    seekTimeline(workspace_.playhead(), false);
    if (!EvaluatedClip(workspace_, workspace_.playhead()))
        publishOpenResult(workspace_.duration() > 0, {});
}
void EditPlayerWorker::publishMonitoring() {
    if (!session_) {
        emit monitoringChanged({}, false);
        return;
    }
    const auto metadata = session_->AudioMetadata();
    const auto* clip = workspace_.clip(active_clip_);
    const auto* asset = clip ? workspace_.asset(clip->asset) : nullptr;
    QVariantList tracks;
    for (const auto& actual : metadata.actual_tracks) {
        bool selected = false;
        if (asset && asset->monitor_selection && !session_->MonitoringConfirmationRequired()) {
            selected =
                std::any_of(asset->monitor_selection->tracks.begin(), asset->monitor_selection->tracks.end(),
                            [&](const auto& entry) { return entry.track_id == actual.track_id && !entry.muted; });
        } else if (metadata.status == engine::AudioMetadataStatus::Verified && metadata.manifest) {
            const auto& defaults = metadata.manifest->default_monitor_track_ids;
            selected = std::find(defaults.begin(), defaults.end(), actual.track_id) != defaults.end();
        }
        tracks.push_back(QVariantMap{
            {QStringLiteral("id"), QString::fromStdString(engine::ToUuid(actual.track_id))},
            {QStringLiteral("name"), QString::fromStdString(actual.name)},
            {QStringLiteral("role"), QString::fromStdString(actual.role)},
            {QStringLiteral("selected"), selected},
            {QStringLiteral("provenanceVerified"), metadata.status == engine::AudioMetadataStatus::Verified},
            {QStringLiteral("available"), true}});
    }
    emit monitoringChanged(std::move(tracks), session_->MonitoringConfirmationRequired());
}

void EditPlayerWorker::confirmMonitoring(const QStringList& track_ids, bool overlap_confirmed) {
    if (!session_)
        return;
    pause();
    const auto metadata = session_->AudioMetadata();
    engine::MonitorSelectionSnapshot selection;
    selection.media_id = metadata.selection_media_id;
    selection.revision = {++monitor_revision_};
    selection.overlap_confirmed = overlap_confirmed;
    selection.unverified_asset_binding = metadata.unverified_asset_binding;
    for (const auto& text : track_ids) {
        const auto id = engine::FromUuid<engine::AudioTrackId>(text.toStdString());
        if (!id) {
            publishMonitoring();
            emit monitoringRejected(tr("The audio monitoring track identity is invalid."));
            return;
        }
        selection.tracks.push_back({*id, 1.0f, false});
    }
    std::string error;
    if (!session_->SetMonitorSelection(selection, error)) {
        publishMonitoring();
        emit monitoringRejected(QString::fromStdString(error));
        return;
    }
    if (active_source_)
        active_source_->monitor_selection = selection;
    const auto* clip = workspace_.clip(active_clip_);
    if (clip) {
        const auto asset = clip->asset;
        workspace_.setMonitorSelection(asset, selection);
        emit monitorSelectionConfirmed(asset, selection);
    }
    publishMonitoring();
}

void EditPlayerWorker::setVolume(double volume) {
    volume_ = volume;
    if (session_)
        session_->SetVolume(static_cast<float>(volume));
}

bool EditPlayerWorker::canReuseSource(const edit::Asset& asset) const {
    if (!session_ || !active_source_ || render_mode_ || active_source_->id != asset.id ||
        active_source_->path != asset.path || !active_source_->source_descriptor || !asset.source_descriptor ||
        active_source_->source_descriptor->asset_identity_fingerprint !=
            asset.source_descriptor->asset_identity_fingerprint)
        return false;
    std::error_code error;
    const auto size = std::filesystem::file_size(asset.path, error);
    if (error || size != active_source_size_)
        return false;
    const auto write_time = std::filesystem::last_write_time(asset.path, error);
    return !error && write_time == active_source_write_time_;
}

bool EditPlayerWorker::bindTimelineSource(const edit::Clip& clip, const edit::Asset& asset) {
    const auto offset = SubtractTime(clip.start, clip.source_in);
    if (!offset) {
        publishOpenResult(false, tr("The source time is outside the supported range."));
        return false;
    }
    const bool mapping_changed = *offset != active_timeline_offset_;
    const bool monitoring_changed = asset.monitor_selection && (!active_source_->monitor_selection ||
                                                                asset.monitor_selection->revision.value !=
                                                                    active_source_->monitor_selection->revision.value);
    if (mapping_changed || monitoring_changed)
        pause();
    if (mapping_changed) {
        // Pause joins playback, but a scrub read can still invoke the old mapping.
        session_->CancelPendingSeek();
        auto sink = sink_;
        session_->SetOnFrameReadyWithGeneration(
            [sink, offset = *offset](engine::RawDecodedVideoFrame frame, uint64_t generation) {
                sink->deliver(std::move(frame), generation, offset);
            });
        active_timeline_offset_ = *offset;
    }
    if (monitoring_changed) {
        std::string error;
        if (!session_->SetMonitorSelection(*asset.monitor_selection, error)) {
            emit monitoringRejected(QString::fromStdString(error));
            publishOpenResult(false, QString::fromStdString(error));
            return false;
        }
    }
    active_clip_ = clip.id;
    active_source_ = asset;
    if (monitoring_changed)
        publishMonitoring();
    return true;
}

void EditPlayerWorker::seekTimeline(qint64 position_us, bool resume) {
    if (render_mode_) {
        if (!render_reader_)
            return;
        pause();
        position_us_ = std::clamp<qint64>(position_us, 0, workspace_.duration());
        position_ms_ = position_us_ / 1000;
        const auto prepareMonitoring = [this](const edit::Clip& clip) {
            const auto* asset = workspace_.asset(clip.asset);
            if (!asset || asset->state != edit::AssetState::Available)
                return false;
            if (active_clip_ != clip.id || !session_) {
                if (session_)
                    session_->Close();
                session_ = std::make_unique<engine::EditPlayerSession>();
                active_clip_ = clip.id;
                std::string error;
                if (!session_->Open(std::filesystem::path(asset->path), error)) {
                    session_.reset();
                    emit monitoringRejected(QString::fromStdString(error));
                    return false;
                }
                if (asset->monitor_selection) {
                    monitor_revision_ = std::max(monitor_revision_, asset->monitor_selection->revision.value);
                    if (!session_->SetMonitorSelection(*asset->monitor_selection, error))
                        emit monitoringRejected(QString::fromStdString(error));
                }
            }
            publishMonitoring();
            return !session_->MonitoringConfirmationRequired();
        };
        if (resume) {
            for (const auto& clip : workspace_.clips()) {
                const auto* asset = workspace_.asset(clip.asset);
                if (asset && asset->audio && !prepareMonitoring(clip)) {
                    if (!session_)
                        emit monitoringChanged({}, true);
                    emit monitoringRejected(tr("Confirm the audio monitoring selection before playback."));
                    return;
                }
            }
        }
        if (const auto* clip = EvaluatedClip(workspace_, position_us_))
            prepareMonitoring(*clip);
        else
            emit monitoringChanged({}, false);
        if (render_audio_)
            render_audio_->Stop();
        render_reader_->ResetAudio();
        position_us_ = std::clamp<qint64>(position_us, 0, workspace_.duration());
        position_ms_ = position_us_ / 1000;
        render_origin_us_ = position_us_;
        audio_sample_ = SampleAt(position_us_);
        timeline_playing_ = resume;
        if (!renderTimeline(position_us_, engine::VideoReadMode::RandomAccess))
            return;
        // A seek can supersede the original open result while reusing its reader.
        publishOpenResult(true, {});
        ensureTimer();
        tick_timer_->setInterval(
            EditPreviewTickMsFor(static_cast<double>(render_plan_.fps_num) / render_plan_.fps_den, screen_hz_));
        elapsed_.restart();
        if (resume) {
            const bool has_audio =
                std::any_of(workspace_.clips().begin(), workspace_.clips().end(), [this](const auto& c) {
                    const auto* asset = workspace_.asset(c.asset);
                    return asset && asset->audio;
                });
            if (has_audio && !render_audio_) {
                render_audio_ = std::make_unique<engine::WasapiAudioRenderer>();
                std::string error;
                if (!render_audio_->Init(error)) {
                    render_audio_.reset();
                    timeline_playing_ = false;
                    publishOpenResult(false, QString::fromStdString(error));
                    return;
                }
            }
            if (render_audio_) {
                std::vector<float> initial;
                std::string error;
                const auto count =
                    static_cast<uint32_t>(std::min<int64_t>(9600, SampleAt(workspace_.duration()) - audio_sample_));
                if (!render_reader_->AudioAt(audio_sample_, count, initial, error)) {
                    pause();
                    publishOpenResult(false, QString::fromStdString(error));
                    return;
                }
                for (auto& sample : initial)
                    sample *= static_cast<float>(volume_);
                render_audio_->Start(initial, true);
                if (!render_audio_->Running()) {
                    pause();
                    publishOpenResult(false, tr("The audio output could not start."));
                    return;
                }
                audio_sample_ += count;
            }
            tickRenderTimeline();
            if (timeline_playing_)
                tick_timer_->start();
        } else
            tick_timer_->stop();
        publishPlaybackState(timeline_playing_);
        return;
    }
    const auto* clip = EvaluatedClip(workspace_, position_us);
    const auto* asset = clip ? workspace_.asset(clip->asset) : nullptr;
    const auto id = clip ? clip->id : 0;
    const bool reuse = clip && asset && asset->state == edit::AssetState::Available && canReuseSource(*asset) &&
                       active_stream_ == clip->stream;
    if (!reuse && (id != active_clip_ || clip || session_)) {
        close();
        sink_->clear();
        active_clip_ = id;
        if (asset && asset->state == edit::AssetState::Available)
            open(QString::fromStdWString(asset->path), asset->duration / 1000, screen_hz_);
        else
            publishOpenResult(workspace_.duration() > 0, asset ? tr("Media unavailable") : QString());
    }
    if (reuse && !bindTimelineSource(*clip, *asset))
        return;
    position_us_ = position_us;
    position_ms_ = position_us / 1000;
    if (session_ && clip) {
        const auto offset = SubtractTime(position_us, clip->start);
        const auto source = offset ? AddTime(*offset, clip->source_in) : std::nullopt;
        if (!source) {
            pause();
            publishOpenResult(false, tr("The source time is outside the supported range."));
            return;
        }
        const auto source_us = *source;
        session_->SeekTo(source_us, request_generation_);
        if (resume && session_->MonitoringConfirmationRequired()) {
            pause();
            publishMonitoring();
            emit monitoringRejected(tr("Confirm the audio monitoring selection before playback."));
            return;
        }
        if (resume)
            session_->Play(source_us, request_generation_);
        syncClock();
        publishOpenResult(true, {});
    }
    // Valid blank time is a presented black frame, not a pending decode: the
    // seek converges and playback continues on the wall clock.
    if (!clip && workspace_.duration() > 0) {
        presentBlank(position_us);
        publishOpenResult(true, {});
    }
    timeline_playing_ = resume;
    publishPlaybackState(timeline_playing_);
    ensureTimer();
    elapsed_.restart();
    if (resume)
        tick_timer_->start();
    else
        tick_timer_->stop();
}

bool EditPlayerWorker::renderTimeline(qint64 position_us, engine::VideoReadMode mode) {
    if (!render_reader_)
        return false;
    engine::TimelineVideoFrame frame;
    std::string error;
    if (!render_reader_->VideoAt(position_us, frame, error, mode, 0)) {
        sink_->clear();
        publishOpenResult(false, QString::fromStdString(error));
        pause();
        return false;
    }
    sink_->publishClock(-1);
    sink_->deliverTimeline(std::move(frame), request_generation_);
    return true;
}

void EditPlayerWorker::presentBlank(qint64 position_us) {
    sink_->publishClock(-1);
    sink_->deliverTimeline({position_us, BlankGeometry(workspace_), {}}, request_generation_);
}

void EditPlayerWorker::tickRenderTimeline() {
    if (!timeline_playing_ || !render_reader_)
        return;
    position_us_ =
        render_origin_us_ +
        (render_audio_ ? static_cast<qint64>(render_audio_->FramesPlayed() * 1'000'000 / render_audio_->SampleRate())
                       : elapsed_.nsecsElapsed() / 1000);
    position_us_ = std::min<qint64>(position_us_, workspace_.duration());
    position_ms_ = position_us_ / 1000;
    if (render_audio_) {
        if (!render_audio_->Running()) {
            pause();
            publishOpenResult(false, tr("The audio output stopped unexpectedly."));
            return;
        }
        const auto consumed = render_audio_->TimelineConsumedFrames();
        const int64_t origin = SampleAt(render_origin_us_);
        audio_sample_ = std::max(audio_sample_, origin + static_cast<int64_t>(consumed));
        const int64_t through =
            origin + static_cast<int64_t>(engine::TimelineAudioRenderThrough(
                         static_cast<uint64_t>(SampleAt(workspace_.duration()) - origin), consumed));
        std::vector<float> samples;
        std::string error;
        while (audio_sample_ < through) {
            const auto count = static_cast<uint32_t>(std::min<int64_t>(9600, through - audio_sample_));
            if (!render_reader_->AudioAt(audio_sample_, count, samples, error)) {
                pause();
                publishOpenResult(false, QString::fromStdString(error));
                return;
            }
            for (auto& sample : samples)
                sample *= static_cast<float>(volume_);
            render_audio_->PushTimelineSamples(static_cast<uint64_t>(audio_sample_ - SampleAt(render_origin_us_)),
                                               samples);
            audio_sample_ += count;
        }
    }
    if (!renderTimeline(position_us_))
        return;
    emit positionAdvancedUs(position_us_);
    emit positionAdvanced(position_ms_);
    if (position_us_ >= workspace_.duration()) {
        pause();
        emit reachedEnd();
    }
}

} // namespace exosnap::quick
