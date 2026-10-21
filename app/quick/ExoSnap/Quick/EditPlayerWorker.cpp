#include "EditPlayerWorker.h"
#include <QScopeGuard>
#include <capability/translatable.h>
#include <exosnap/engine/timeline_audio_queue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

// Fallback tick, used only until a clip is open (or when its container declares
// no usable frame rate).
constexpr int kWorkerFallbackTickMs = 16;

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
const edit::Clip* EvaluatedClip(const EditPlaybackSnapshot& workspace, int64_t time_us) {
    const auto evaluation = engine::EvaluateTimeline(workspace.timeline(), time_us);
    return evaluation.error.empty() && !evaluation.video.empty() ? workspace.clip(evaluation.video.back().clip)
                                                                 : nullptr;
}

// The single-clip session shows exactly one decoded source at full opacity.
// Junctions, partial opacity, video edge fades and simultaneously visible
// video clips need the compositing reader instead.
bool NeedsComposition(const EditPlaybackSnapshot& workspace) {
    for (const auto& junction : workspace.transitions()) {
        const auto track = std::find_if(workspace.tracks().begin(), workspace.tracks().end(),
                                        [&](const auto& value) { return value.id == junction.track; });
        if (track != workspace.tracks().end() && track->type == edit::TrackType::Video && track->edit_enabled)
            return true;
    }
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
engine::FrameSize BlankGeometry(const EditPlaybackSnapshot& workspace) {
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

struct EditPlayerWorker::VideoPump {
    enum class Kind { Video, Monitoring, Probe, Clear };
    struct Request {
        Kind kind = Kind::Video;
        std::shared_ptr<const engine::TimelineRenderSnapshot> snapshot;
        engine::EditRenderPlan plan;
        qint64 position_us = 0;
        engine::VideoReadMode mode = engine::VideoReadMode::Sequential;
        uint64_t epoch = 0, generation = 0, asset = 0, monitor_request = 0, session = 0, source_version = 0;
        bool composed = false;
        std::filesystem::path path;
        std::optional<engine::MonitorSelectionSnapshot> selection;
        std::function<bool(const std::atomic<bool>&)> before_read, before_open;
        std::atomic<bool> cancelled{false};
    };
    struct Result {
        Kind kind = Kind::Video;
        engine::TimelineVideoFrame frame;
        std::string error;
        uint64_t epoch = 0, generation = 0, asset = 0, monitor_request = 0, session = 0, source_version = 0;
        std::optional<int64_t> raw_offset;
        std::optional<engine::MonitorSelectionSnapshot> selection;
        engine::AudioMetadataReadResult monitoring_metadata;
        std::shared_ptr<const engine::MediaReadCapability> bound_access;
        bool metadata_available = false;
        std::shared_ptr<const engine::TimelineRenderSnapshot> resolved;
        std::shared_ptr<const EditPlaybackSnapshot> workspace;
    };
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::shared_ptr<Request> pending, active;
    std::thread thread;
    std::function<void(Result)> completed;
    std::atomic<uint64_t> requests{0}, frames{0}, superseded{0}, seeks{0}, opens{0};
    std::atomic<int64_t> read_ns{0};
    std::atomic<uint64_t> read_count{0};
    std::atomic<int64_t> video_read_ns{0};

    explicit VideoPump(std::function<void(Result)> callback) : completed(std::move(callback)) {
        thread = std::thread([this] { Run(); });
    }
    ~VideoPump() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            CancelLocked();
        }
        changed.notify_all();
        thread.join();
    }
    void CancelLocked() {
        for (const auto& request : {pending, active})
            if (request) {
                request->cancelled.store(true, std::memory_order_release);
                request->cancelled.notify_all();
            }
        pending.reset();
    }
    void Cancel() {
        {
            std::lock_guard lock(mutex);
            CancelLocked();
        }
        changed.notify_all();
    }
    void CancelVideo() {
        {
            std::lock_guard lock(mutex);
            if (active && active->kind == Kind::Video) {
                active->cancelled.store(true, std::memory_order_release);
                active->cancelled.notify_all();
            }
            if (pending && pending->kind == Kind::Video) {
                pending->cancelled.store(true, std::memory_order_release);
                pending.reset();
            }
        }
        changed.notify_all();
    }
    bool SameConfirmationSource(const Request& before, const Request& after) const {
        if (!before.snapshot || !after.snapshot)
            return false;
        const auto find = [&](const auto& sources) {
            return std::find_if(sources.begin(), sources.end(),
                                [&](const auto& source) { return source.id == before.asset; });
        };
        const auto old_source = find(before.snapshot->sources);
        const auto new_source = find(after.snapshot->sources);
        return old_source != before.snapshot->sources.end() && new_source != after.snapshot->sources.end() &&
               old_source->path == new_source->path && old_source->access == new_source->access;
    }
    void Queue(std::shared_ptr<Request> request) {
        {
            std::lock_guard lock(mutex);
            if (pending && pending->kind == Kind::Monitoring && request->kind == Kind::Video &&
                SameConfirmationSource(*pending, *request)) {
                superseded.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const bool retained_confirmation = active && active->kind == Kind::Monitoring &&
                                               request->kind == Kind::Video &&
                                               SameConfirmationSource(*active, *request);
            if (active && !retained_confirmation &&
                (active->epoch != request->epoch || active->kind != Kind::Video || request->kind != Kind::Video)) {
                active->cancelled.store(true, std::memory_order_release);
                active->cancelled.notify_all();
            }
            if (pending)
                superseded.fetch_add(1, std::memory_order_relaxed);
            pending = std::move(request);
        }
        requests.fetch_add(1, std::memory_order_relaxed);
        changed.notify_one();
    }
    void Clear(uint64_t epoch) {
        auto request = std::make_shared<Request>();
        request->kind = Kind::Clear;
        request->epoch = epoch;
        Queue(std::move(request));
    }
    void Submit(std::shared_ptr<const engine::TimelineRenderSnapshot> snapshot, const engine::EditRenderPlan& plan,
                qint64 position_us, engine::VideoReadMode mode, uint64_t epoch, uint64_t generation,
                const std::function<bool(const std::atomic<bool>&)>& before_read) {
        auto request = std::make_shared<Request>();
        request->snapshot = std::move(snapshot);
        request->plan = plan;
        request->position_us = position_us;
        request->mode = mode;
        request->epoch = epoch;
        request->generation = generation;
        request->before_read = before_read;
        request->composed = true;
        Queue(std::move(request));
    }
    bool Resolve(const Request& request, Result& result) {
        if (!request.snapshot) {
            result.error = EXOSNAP_TRANSLATABLE("EditRender", "The video source snapshot is unavailable.");
            return false;
        }
        std::shared_ptr<engine::TimelineRenderSnapshot> resolved;
        for (size_t index = 0; index < request.snapshot->sources.size(); ++index) {
            const auto& source = request.snapshot->sources[index];
            if (source.access || !source.metadata.streams.empty())
                continue;
            if (request.cancelled.load())
                return false;
            if (request.before_open && !request.before_open(request.cancelled))
                return false;
            const auto access = engine::AcquireMediaReadCapability(source.path);
            if (!access.succeeded()) {
                result.error = access.error()->context;
                return false;
            }
            const auto metadata = engine::ProbeEditMedia(*access.value(), {}, {}, &request.cancelled, false);
            if (!metadata.error.empty()) {
                result.error = metadata.error;
                return false;
            }
            if (!resolved)
                resolved = std::make_shared<engine::TimelineRenderSnapshot>(*request.snapshot);
            resolved->sources[index].access = *access.value();
            resolved->sources[index].metadata = metadata;
        }
        result.resolved = resolved ? resolved : request.snapshot;
        return true;
    }
    bool Probe(const Request& request, Result& result) {
        if (request.before_open && !request.before_open(request.cancelled))
            return false;
        const auto access = engine::AcquireMediaReadCapability(request.path);
        if (!access.succeeded()) {
            result.error = access.error()->context;
            return false;
        }
        const auto metadata = engine::ProbeEditMedia(*access.value(), {}, {}, &request.cancelled);
        if (!metadata.error.empty()) {
            result.error = metadata.error;
            return false;
        }
        const auto proof =
            engine::VerifyMediaIdentity(*access.value(), metadata.content_sha256, {}, {}, &request.cancelled);
        if (!proof.succeeded()) {
            result.error = proof.error()->context;
            return false;
        }
        edit::Workspace workspace;
        edit::Asset asset;
        asset.path = request.path.wstring();
        asset.duration = metadata.duration_us;
        asset.width = metadata.width;
        asset.height = metadata.height;
        asset.fps = metadata.fps;
        asset.fps_num = metadata.fps_num;
        asset.fps_den = metadata.fps_den;
        asset.audio = metadata.has_audio;
        asset.mix_decision_required = metadata.audio_metadata.monitoring_confirmation_required;
        const auto id = workspace.addAsset(std::move(asset));
        const auto inserted =
            workspace.apply(edit::AddAssetGroup{id, 0, 0, {}, {}, metadata.width > 0, false}, workspace.revision());
        if (inserted.status != edit::CommandStatus::Applied) {
            result.error = EXOSNAP_TRANSLATABLE("EditRender", "The source cannot be placed in the preview timeline.");
            return false;
        }
        std::vector<engine::TimelineMediaSource> sources{
            {id, request.path, metadata, {}, *access.value(), *proof.value()}};
        result.workspace = std::make_shared<const EditPlaybackSnapshot>(workspace, std::move(sources));
        return true;
    }
    void Run() {
        auto reader = std::make_unique<engine::EditTimelineReader>();
        std::shared_ptr<const engine::TimelineRenderSnapshot> installed;
        uint64_t retired_opens = 0, retired_seeks = 0;
        for (;;) {
            std::shared_ptr<Request> request;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || pending; });
                if (stopping)
                    return;
                request = std::move(pending);
                active = request;
            }
            Result result;
            result.kind = request->kind;
            result.epoch = request->epoch;
            result.generation = request->generation;
            result.asset = request->asset;
            result.monitor_request = request->monitor_request;
            result.session = request->session;
            result.source_version = request->source_version;
            result.selection = request->selection;
            const auto begin = std::chrono::steady_clock::now();
            bool okay = true;
            try {
                if (request->kind == Kind::Clear) {
                    retired_opens += reader->VideoDecoderOpens();
                    retired_seeks += reader->VideoReadStats().seeks;
                    reader = std::make_unique<engine::EditTimelineReader>();
                    installed.reset();
                } else if (request->kind == Kind::Probe) {
                    okay = Probe(*request, result);
                } else {
                    okay = Resolve(*request, result);
                    auto plan = request->plan;
                    if (okay && request->composed && installed != result.resolved) {
                        plan = engine::ClassifyEditExport(
                            *result.resolved, {engine::TimelineExportEligibility::RenderRequired, {}}, false);
                        if (plan.path != engine::EditExportPath::Render) {
                            result.error = plan.reason;
                            okay = false;
                        }
                    }
                    if (okay && installed != result.resolved) {
                        okay = installed ? reader->UpdateSnapshot(*result.resolved, plan, result.error)
                                         : reader->Open(*result.resolved, plan, result.error);
                        if (okay)
                            installed = result.resolved;
                    }
                    if (okay && request->before_read)
                        okay = request->before_read(request->cancelled);
                    if (okay && !request->cancelled.load()) {
                        if (request->kind == Kind::Monitoring) {
                            okay = request->selection && reader->ConfirmMonitorSelection(
                                                             request->asset, request->position_us, *request->selection,
                                                             result.monitoring_metadata, result.error,
                                                             &request->cancelled, request->before_open);
                            if (okay) {
                                const auto source =
                                    std::find_if(result.resolved->sources.begin(), result.resolved->sources.end(),
                                                 [&](const auto& value) { return value.id == request->asset; });
                                if (source != result.resolved->sources.end())
                                    result.bound_access = source->access;
                            }
                        } else {
                            const auto video_begin = std::chrono::steady_clock::now();
                            read_count.fetch_add(1, std::memory_order_relaxed);
                            okay = reader->VideoAt(request->position_us, result.frame, result.error, request->mode,
                                                   request->epoch, &request->cancelled, request->before_open);
                            video_read_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                        std::chrono::steady_clock::now() - video_begin)
                                                        .count(),
                                                    std::memory_order_relaxed);
                            if (okay) {
                                const auto evaluated =
                                    engine::EvaluateTimeline(result.resolved->timeline, request->position_us);
                                if (!evaluated.video.empty()) {
                                    const auto& contribution = evaluated.video.back();
                                    result.asset = contribution.asset;
                                    if (const auto metadata = reader->VideoAudioMetadata(contribution.asset)) {
                                        result.monitoring_metadata = *metadata;
                                        result.metadata_available = true;
                                    }
                                    if (!request->composed && result.frame.layers.size() == 1)
                                        result.raw_offset = SubtractTime(request->position_us, contribution.source_us);
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& error) {
                okay = false;
                result.error = error.what();
            } catch (...) {
                okay = false;
                result.error = EXOSNAP_TRANSLATABLE("EditRender", "The timeline video request failed.");
            }
            read_ns.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
            opens.store(retired_opens + reader->VideoDecoderOpens());
            seeks.store(retired_seeks + reader->VideoReadStats().seeks);
            if (!request->cancelled.load() && request->kind != Kind::Clear) {
                if (okay && request->kind == Kind::Video)
                    ++frames;
                else if (!okay && result.error.empty())
                    result.error = EXOSNAP_TRANSLATABLE("EditRender", "The timeline video request did not complete.");
                completed(std::move(result));
            } else if (request->cancelled.load()) {
                ++superseded;
            }
            {
                std::lock_guard lock(mutex);
                if (active == request)
                    active.reset();
            }
        }
    }
};
struct EditPlayerWorker::PendingRevision {
    std::shared_ptr<const EditPlaybackSnapshot> workspace;
    std::shared_ptr<const engine::TimelineRenderSnapshot> render;
    engine::TimelineAudioStamp stamp;
    int64_t first_sample = 0;
    uint64_t old_clock_anchor = 0;
    uint64_t command = 0;
    uint64_t generation = 0;
    std::function<bool()> may_play;
    bool failed = false;
};
EditPlayerWorker::EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink) : sink_(std::move(sink)) {
}

EditPlayerWorker::~EditPlayerWorker() {
    close();
}

QVariantMap EditPlayerWorker::performanceSnapshot() const {
    const auto audio = audio_producer_ ? audio_producer_->Measurements() : engine::TimelineAudioMeasurements{};
    const auto standby =
        standby_audio_producer_ ? standby_audio_producer_->Measurements() : engine::TimelineAudioMeasurements{};
    const auto consumed = sink_->sceneGraphConsumption();
    auto reads = retired_seek_performance_;
    if (video_pump_) {
        reads.read_count += video_pump_->read_count.load();
        reads.read_ns += video_pump_->video_read_ns.load();
    }

    QVariantMap result{
        {"requestGeneration", QVariant::fromValue(request_generation_)},
        {"timelineUpdates", QVariant::fromValue(timeline_updates_)},
        {"timelineRevision", QVariant::fromValue(workspace_->revision())},
        {"requestedTimelineRevision",
         QVariant::fromValue(pending_revision_ ? pending_revision_->workspace->revision() : workspace_->revision())},
        {"revisionPreparationPending", bool(pending_revision_)},
        {"revisionHandoffs", QVariant::fromValue(revision_handoffs_)},
        {"retainedPicturePendingTicks", QVariant::fromValue(late_video_mapping_ticks_)},
        {"audioLeadBeforeRevisionResetFrames", QVariant::fromValue(revision_lead_before_reset_frames_)},
        {"audioProducerInstances", (audio_producer_ ? 1 : 0) + (standby_audio_producer_ ? 1 : 0)},
        {"audioCurrentProducerSourceBytes", QVariant::fromValue(audio.source_bytes + standby.source_bytes)},
        {"audioPendingSourceBufferFrames", QVariant::fromValue(standby.source_buffer_frames)},
        {"audioPendingReadyGeneration", QVariant::fromValue(standby.ready_generation)},
        {"outputRevision", sink_->outputRevision() ? QVariant::fromValue(*sink_->outputRevision()) : QVariant{}},
        {"sceneGraphConsumedRevision", consumed ? QVariant::fromValue(consumed->revision) : QVariant{}},
        {"sceneGraphConsumedTimeUs", consumed ? QVariant::fromValue(consumed->timeline_time_us) : QVariant{}},
        {"sceneGraphConsumptionSource", QStringLiteral("successful_render_state_postback")},
        {"timelinePlayheadUs", snapshot_position_us_},
        {"sessionOpens", QVariant::fromValue(session_opens_)},
        {"sessionCloses", QVariant::fromValue(session_closes_)},
        {"sessionOpenNs", session_open_ns_},
        {"sessionCloseNs", session_close_ns_},
        {"seekDispatches", QVariant::fromValue(seek_dispatches_)},
        {"videoSeeks", QVariant::fromValue(video_seeks_)},
        {"audioStreamGeneration", QVariant::fromValue(audio.stream_generation)},
        {"audioRequestedRevision", QVariant::fromValue(audio.requested_revision)},
        {"audioPartialControlsFromRevision",
         audible_controls_timeline_ ? QVariant::fromValue(audible_control_revision_) : QVariant{}},
        {"audioAppliedRevision", QVariant::fromValue(audio.applied_revision)},
        {"audioRequestedControlRevision", QVariant::fromValue(audio.requested_control_revision)},
        {"audioAppliedControlRevision", QVariant::fromValue(audio.applied_control_revision)},
        {"audioSourceBufferFrames", QVariant::fromValue(audio.source_buffer_frames)},
        {"audioSourceCapacityFrames", QVariant::fromValue(audio.source_capacity_frames)},
        {"audioSourceBytes", QVariant::fromValue(audio.source_bytes)},
        {"audioLoopHeadFrames", QVariant::fromValue(audio.loop_head_frames)},
        {"audioLoopHeadBytes", QVariant::fromValue(audio.loop_head_bytes)},
        {"audioLoopHeadReuses", QVariant::fromValue(audio.loop_head_reuses)},
        {"audioPeakDecoders", QVariant::fromValue(audio.peak_decoders)},
        {"audioUnderrunFrames", QVariant::fromValue(audio.underrun_frames)},
        {"audioStaleBlocks", QVariant::fromValue(audio.stale_blocks)},
        {"audioLookaheadDeferred", QVariant::fromValue(audio.lookahead_deferred)},
        {"audioDecoderOpens", QVariant::fromValue(audio.source_reads.decoder_opens)},
        {"audioCursorReuses", QVariant::fromValue(audio.source_reads.cursor_reuses)},
        {"audioPreparedSourceRanges", QVariant::fromValue(audio.source_reads.prepared_source_ranges)},
        {"audioPreparedCacheHits", QVariant::fromValue(audio.source_reads.prepared_cache_hits)},
        {"audioSourceSeeks", QVariant::fromValue(audio.source_reads.seeks)},
        {"audioDecodedSourceFrames", QVariant::fromValue(audio.source_reads.decoded_frames)},
        {"audioRetiredControlTables", QVariant::fromValue(audio.retired_control_tables)},
        {"audioPeakRetiredControlTables", QVariant::fromValue(audio.peak_retired_control_tables)},
        {"audioRejectedControls", QVariant::fromValue(audio.rejected_controls)},
        {"audioEndpointPublicationLockFree",
         audio_producer_ ? QVariant(audio.endpoint_publication_lock_free) : QVariant{}},
        {"audioPublicationRetryExhaustions", QVariant::fromValue(audio.publication_misses)},
        {"seekDispatchNs", seek_dispatch_ns_},
        {"timelineQueueWaitNs", timeline_queue_wait_ns_},
        {"seekQueueWaitNs", seek_queue_wait_ns_},
        {"decoderReadCount", QVariant::fromValue(reads.read_count)},
        {"decoderReadNs", reads.read_ns}};
    if (video_pump_) {
        result.insert("videoOwnerRequests", QVariant::fromValue(video_pump_->requests.load()));
        result.insert("videoOwnerFrames", QVariant::fromValue(video_pump_->frames.load()));
        result.insert("videoOwnerSuperseded", QVariant::fromValue(video_pump_->superseded.load()));
        result.insert("videoOwnerWorkNs", QVariant::fromValue(video_pump_->read_ns.load()));
        result.insert("videoOwnerDecoderSeeks", QVariant::fromValue(video_pump_->seeks.load()));
        result.insert("videoDecoderOpens", QVariant::fromValue(video_pump_->opens.load()));
    }
    if (render_audio_) {
        const auto endpoint = render_audio_->Measurements();
        result.insert("audioEndpointInitialized", endpoint.initialized);
        result.insert("audioEndpointRunning", endpoint.running);
        if (endpoint.initialized) {
            result.insert("audioEndpointSampleRate", endpoint.sample_rate);
            result.insert("audioEndpointBufferFrames", endpoint.buffer_frames);
            result.insert("audioEndpointRequestedBufferMs", endpoint.requested_buffer_ms);
            result.insert("audioEndpointPeriodNs",
                          endpoint.device_period_ns ? QVariant::fromValue(*endpoint.device_period_ns) : QVariant{});
            result.insert("audioEndpointSubmissions", QVariant::fromValue(endpoint.endpoint_submissions));
            result.insert("audioEndpointLeadFrames", QVariant::fromValue(endpoint.submitted_lead_frames));
            result.insert("audioEndpointPeakLeadFrames", QVariant::fromValue(endpoint.peak_submitted_lead_frames));
            result.insert("audioClockReanchors", QVariant::fromValue(endpoint.clock_reanchors));
            result.insert("audioOutputRecoveryPending", endpoint.timeline_recovery_pending);
            result.insert("audioOutputReanchors", QVariant::fromValue(endpoint.timeline_reanchors));
            result.insert("audioOutputSkippedFrames", QVariant::fromValue(endpoint.timeline_skipped_frames));
            result.insert("audioClockMode",
                          QString::fromLatin1(endpoint.clock_mode == engine::RenderClockMode::Stopped ? "stopped"
                                              : endpoint.clock_mode == engine::RenderClockMode::Device
                                                  ? "device"
                                                  : "monotonic_fallback"));
        }
    }
    const auto hashes = engine::GlobalMediaHashMeasurements();
    if (!audio_output_error_.isEmpty()) {
        result.insert("audioOutputStatus", QStringLiteral("unavailable"));
        result.insert("audioOutputUnavailableReason", audio_output_error_);
        result.insert("audioClockMode", QStringLiteral("monotonic_fallback"));
    }
    result.insert("mediaHashStarts", QVariant::fromValue(hashes.total.starts));
    result.insert("mediaHashCompletions", QVariant::fromValue(hashes.total.completions));
    result.insert("mediaHashBytes", QVariant::fromValue(hashes.total.bytes));
    result.insert("mediaHashCancellations", QVariant::fromValue(hashes.total.cancellations));
    result.insert("mediaHashNs", QVariant::fromValue(hashes.total.elapsed_ns));
    constexpr std::array hash_causes{std::pair{engine::MediaHashCause::Unspecified, "unspecified"},
                                     std::pair{engine::MediaHashCause::SourceProbe, "source_probe"},
                                     std::pair{engine::MediaHashCause::AudioMetadata, "audio_metadata"},
                                     std::pair{engine::MediaHashCause::Sidecar, "sidecar"},
                                     std::pair{engine::MediaHashCause::RecordingFinalization, "recording_finalization"},
                                     std::pair{engine::MediaHashCause::LiveLease, "live_lease"},
                                     std::pair{engine::MediaHashCause::DecoderValidation, "decoder_validation"}};
    static_assert(hash_causes.size() == static_cast<size_t>(engine::MediaHashCause::Count));
    QVariantMap by_cause;
    for (const auto& [cause, name] : hash_causes) {
        const auto& measurements = hashes.by_cause[static_cast<size_t>(cause)];
        by_cause.insert(QString::fromLatin1(name),
                        QVariantMap{{"starts", QVariant::fromValue(measurements.starts)},
                                    {"completions", QVariant::fromValue(measurements.completions)},
                                    {"bytes", QVariant::fromValue(measurements.bytes)},
                                    {"cancellations", QVariant::fromValue(measurements.cancellations)},
                                    {"elapsedNs", QVariant::fromValue(measurements.elapsed_ns)}});
    }
    result.insert("mediaHashByCause", by_cause);
    return result;
}

void EditPlayerWorker::recordQueueWait(qint64 elapsed_ns, bool timeline) {
    (timeline ? timeline_queue_wait_ns_ : seek_queue_wait_ns_) += elapsed_ns;
}

void EditPlayerWorker::open(const QString& path, qint64 duration_ms, double screen_hz) {
    close();
    duration_ms_ = duration_ms;
    screen_hz_ = screen_hz;
    timeline_mode_ = false;
    standalone_probe_pending_ = true;
    standalone_play_requested_ = false;
    standalone_position_us_ = 0;
    ensureVideoPump();
    auto request = std::make_shared<VideoPump::Request>();
    request->kind = VideoPump::Kind::Probe;
    request->path = std::filesystem::path(path.toStdWString());
    request->epoch = video_epoch_;
    request->generation = request_generation_;
    request->before_open = video_before_open_;
    video_pump_->Queue(std::move(request));
}

void EditPlayerWorker::close() {
    if (!adopting_audio_revision_)
        cancelPreparedRevision();
    source_opened_ = false;
    source_open_error_.clear();
    standalone_probe_pending_ = false;
    standalone_play_requested_ = false;
    pause();
    ++video_epoch_;
    if (video_pump_) {
        video_pump_->Cancel();
        if (!adopting_audio_revision_)
            video_pump_->Clear(video_epoch_);
    }
    video_snapshot_.reset();
    if (!adopting_audio_revision_) {
        if (render_audio_)
            render_audio_->Shutdown();
        render_audio_.reset();
        if (audio_producer_)
            audio_producer_->Cancel();
        audio_producer_.reset();
        if (standby_audio_producer_)
            standby_audio_producer_->Cancel();
        standby_audio_producer_.reset();
        audio_start_pending_ = false;
        ++audio_session_generation_;
        audio_output_error_.clear();
    }
    render_snapshot_ = {};
    metadata_cache_.clear();
    workspace_ = std::make_shared<EditPlaybackSnapshot>(edit::Workspace{});
    active_clip_ = 0;
    if (tick_timer_)
        tick_timer_->stop();
    position_ms_ = position_us_ = 0;
    sink_->publishClock(-1);
    publishMonitoringState({}, false);
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
    if (standalone_probe_pending_) {
        standalone_position_us_ = from_us;
        standalone_play_requested_ = true;
        return;
    }
    if (timeline_mode_)
        seekTimeline(from_us >= workspace_->duration() ? 0 : from_us, true);
}

void EditPlayerWorker::pause() {
    if (adopting_audio_revision_)
        return;
    const auto requested = pending_revision_ ? pending_revision_->workspace : nullptr;
    const auto stopped_at = timeline_mode_ && timeline_playing_ ? timelinePositionUs() : position_us_;
    cancelPreparedRevision();
    audible_controls_timeline_.reset();
    audible_control_revision_ = 0;
    standalone_play_requested_ = false;
    publishPlaybackState(false);
    timeline_playing_ = false;
    if (tick_timer_)
        tick_timer_->stop();
    if (!standalone_probe_pending_) {
        ++video_epoch_;
        if (video_pump_)
            video_pump_->CancelVideo();
    }
    if (render_audio_)
        render_audio_->Stop();
    audio_start_pending_ = false;
    if (audio_producer_)
        audio_producer_->Cancel();
    syncClock();
    if (requested)
        applyTimeline(requested, stopped_at, [] { return false; });
}

void EditPlayerWorker::seek(qint64 position_ms) {
    if (position_ms > std::numeric_limits<qint64>::max() / 1000) {
        publishOpenResult(false, tr("The requested time is outside the supported range."));
        return;
    }
    seekUs(std::max<qint64>(position_ms, 0) * 1000);
}

void EditPlayerWorker::seekUs(qint64 target_us) {
    seekWithIntent(target_us, false);
}

void EditPlayerWorker::requestTransport(uint64_t command, uint64_t generation, qint64 target_us, bool seek,
                                        bool playing) {
    transport_command_ = command;
    setRequestGeneration(generation);
    if (seek)
        seekWithIntent(target_us, playing);
    else if (playing)
        playUs(target_us);
    else {
        pause();
        if (timeline_mode_ && workspace_->duration() > 0)
            publishOpenResult(source_opened_, source_open_error_);
    }
    acknowledgeTransport();
}

void EditPlayerWorker::acknowledgeTransport() {
    emit transportCompleted(transport_command_, request_generation_, published_playing_);
}

void EditPlayerWorker::seekWithIntent(qint64 target_us, bool resume) {
    QElapsedTimer dispatch_clock;
    dispatch_clock.start();
    ++seek_dispatches_;
    const auto record = qScopeGuard([&] { seek_dispatch_ns_ += dispatch_clock.nsecsElapsed(); });
    target_us = std::max<qint64>(target_us, 0);
    if (standalone_probe_pending_) {
        standalone_position_us_ = target_us;
        standalone_play_requested_ = resume;
        return;
    }
    pause();
    if (timeline_mode_)
        seekTimeline(target_us, resume);
}

void EditPlayerWorker::setScreenRefreshHz(double screen_hz) {
    screen_hz_ = screen_hz;
    if (tick_timer_)
        tick_timer_->setInterval(EditPreviewTickMsFor(
            render_plan_.fps_den > 0 ? static_cast<double>(render_plan_.fps_num) / render_plan_.fps_den : 0,
            screen_hz_));
}

void EditPlayerWorker::publishOpenResult(bool opened, const QString& error) {
    source_opened_ = opened;
    source_open_error_ = error;
    if (!opened)
        pause();
    emit openFinishedForCommand(transport_command_, request_generation_, opened, error);
    emit openFinishedForGeneration(request_generation_, opened, error);
    emit openFinished(opened, error);
}

void EditPlayerWorker::publishPlaybackState(bool playing) {
    emit transportStateChanged(transport_command_, request_generation_, playing);
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

void EditPlayerWorker::publishPosition() {
    emit positionAdvancedForCommand(transport_command_, request_generation_, position_us_);
    emit positionAdvancedUs(position_us_);
    emit positionAdvanced(position_ms_);
}

void EditPlayerWorker::syncClock() {
    sink_->publishClock(-1);
}

void EditPlayerWorker::onTick() {
    if (const auto consumed = sink_->sceneGraphConsumption();
        timeline_playing_ && (!consumed || consumed->revision != workspace_->revision()))
        ++late_video_mapping_ticks_;
    if (pending_revision_)
        adoptPreparedRevision();
    if (timeline_mode_)
        tickRenderTimeline();
}

void EditPlayerWorker::setTimeline(const edit::Workspace& workspace) {
    setTimeline(std::make_shared<EditPlaybackSnapshot>(workspace), workspace.playhead());
}

engine::TimelineRenderSnapshot EditPlayerWorker::makeRenderSnapshot(const EditPlaybackSnapshot& value) {
    engine::TimelineRenderSnapshot snapshot;
    snapshot.timeline = value.timeline();
    for (const auto& asset : value.assets()) {
        if (asset.state != edit::AssetState::Available)
            continue;
        const std::filesystem::path path(asset.path);
        const auto admitted = std::find_if(value.sources().begin(), value.sources().end(),
                                           [&](const auto& source) { return source.id == asset.id; });
        if (admitted != value.sources().end()) {
            metadata_cache_.insert_or_assign(asset.id, *admitted);
        } else {
            const auto cached = metadata_cache_.find(asset.id);
            if (cached == metadata_cache_.end() || cached->second.path != path) {
                engine::TimelineMediaSource source;
                source.id = asset.id;
                source.path = path;
                source.metadata.duration_us = asset.duration;
                source.metadata.width = asset.width;
                source.metadata.height = asset.height;
                source.metadata.fps = asset.fps;
                source.metadata.fps_num = asset.fps_num;
                source.metadata.fps_den = asset.fps_den;
                source.metadata.has_audio = asset.audio;
                metadata_cache_.insert_or_assign(asset.id, std::move(source));
            }
        }
        auto source = metadata_cache_.at(asset.id);
        source.monitor_selection = asset.monitor_selection;
        snapshot.sources.push_back(std::move(source));
    }
    return snapshot;
}

void EditPlayerWorker::setTimeline(std::shared_ptr<const EditPlaybackSnapshot> workspace, qint64 position_us,
                                   const std::function<bool()>& may_play) {
    ++timeline_updates_;
    const bool resume = may_play ? may_play() : timeline_playing_;
    const auto effective = timeline_mode_ && timeline_playing_ ? timelinePositionUs() : position_us;
    if (timeline_mode_ && timeline_playing_ && resume && workspace->duration() > effective && audio_producer_ &&
        !audio_start_pending_ && !workspace_->retainsMedia(*workspace)) {
        prepareRevision(std::move(workspace), may_play);
        return;
    }
    cancelPreparedRevision();
    audible_controls_timeline_.reset();
    audible_control_revision_ = 0;
    if (resume && effective >= workspace->duration()) {
        applyTimeline(std::move(workspace), effective, [] { return false; });
        emit transportEnded(transport_command_, request_generation_);
        emit reachedEnd();
        return;
    }
    applyTimeline(std::move(workspace), effective, may_play);
}

void EditPlayerWorker::cancelPreparedRevision() {
    if (pending_revision_ && standby_audio_producer_)
        standby_audio_producer_->Cancel();
    pending_revision_.reset();
}

void EditPlayerWorker::updateAudibleControls(const EditPlaybackSnapshot& requested) {
    auto audible = workspace_->timeline();
    std::unordered_map<uint64_t, const engine::TimelineClip*> requested_clips;
    for (const auto& clip : requested.timeline().clips)
        if (clip.audio)
            requested_clips.emplace(clip.id, &clip);
    for (auto& clip : audible.clips) {
        const auto found = requested_clips.find(clip.id);
        if (!clip.audio || found == requested_clips.end())
            continue;
        const auto& next = *found->second;
        if (clip.asset == next.asset && clip.stream == next.stream && clip.start_us == next.start_us &&
            clip.source_in_us == next.source_in_us && clip.source_out_us == next.source_out_us) {
            clip.enabled = next.enabled;
            clip.gain_db = next.gain_db;
        }
    }
    for (auto& track : audible.tracks) {
        if (!track.audio)
            continue;
        const auto next = std::find_if(requested.timeline().tracks.begin(), requested.timeline().tracks.end(),
                                       [&](const auto& row) { return row.audio && row.id == track.id; });
        if (next != requested.timeline().tracks.end()) {
            track.enabled = next->enabled;
            track.gain_db = next->gain_db;
        }
    }
    audible_controls_timeline_ = std::move(audible);
    audible_control_revision_ = requested.revision();
}

void EditPlayerWorker::prepareRevision(std::shared_ptr<const EditPlaybackSnapshot> workspace,
                                       const std::function<bool()>& may_play) {
    sink_->setRevision(request_generation_, workspace_->revision());
    if (pending_revision_ && pending_revision_->workspace->retainsMedia(*workspace)) {
        pending_revision_->workspace = std::move(workspace);
        pending_revision_->stamp.revision = pending_revision_->workspace->revision();
        pending_revision_->may_play = may_play;
        pending_revision_->command = transport_command_;
        pending_revision_->generation = request_generation_;
        updateAudibleControls(*pending_revision_->workspace);
        publishAudioControls();
        return;
    }
    auto render = std::make_shared<const engine::TimelineRenderSnapshot>(makeRenderSnapshot(*workspace));
    if (NeedsComposition(*workspace) &&
        std::all_of(render->sources.begin(), render->sources.end(),
                    [](const auto& source) { return !source.metadata.streams.empty(); })) {
        const auto plan =
            engine::ClassifyEditExport(*render, {engine::TimelineExportEligibility::RenderRequired, {}}, false);
        if (plan.path != engine::EditExportPath::Render) {
            publishOpenResult(true, QString::fromStdString(plan.reason));
            return;
        }
    }
    cancelPreparedRevision();
    if (!standby_audio_producer_)
        standby_audio_producer_ = std::make_shared<engine::TimelineAudioProducer>();
    auto pending = std::make_unique<PendingRevision>();
    pending->workspace = std::move(workspace);
    pending->render = std::move(render);
    pending->command = transport_command_;
    pending->generation = request_generation_;
    pending->may_play = may_play;
    next_audio_stream_generation_ = (std::max)(next_audio_stream_generation_, audio_stream_generation_) + 1;
    pending->stamp = {audio_session_generation_, pending->workspace->revision(), next_audio_stream_generation_};
    const auto duration = pending->workspace->duration();
    auto region =
        playback_bounds_.end_us > playback_bounds_.start_us ? playback_bounds_ : edit::PlaybackInterval{0, duration};
    region.end_us = (std::min)(region.end_us, duration);
    if (region.end_us <= region.start_us)
        region = {0, duration};
    const auto first =
        std::clamp<int64_t>(SampleAt(timelinePositionUs()), SampleAt(region.start_us), SampleAt(region.end_us) - 1);
    pending->first_sample = first;
    pending->old_clock_anchor = timelineOutputFrames();
    standby_audio_producer_->Prepare(
        pending->render, pending->stamp, first, audio_before_read_,
        engine::TimelineAudioRegion{SampleAt(region.start_us), SampleAt(region.end_us), repeat_});
    pending_revision_ = std::move(pending);
    updateAudibleControls(*pending_revision_->workspace);
    publishAudioControls();
    ensureTimer();
    tick_timer_->start();
}

bool EditPlayerWorker::adoptPreparedRevision() {
    if (!pending_revision_)
        return false;
    auto& pending = *pending_revision_;
    if (pending.command != transport_command_ || pending.generation != request_generation_) {
        cancelPreparedRevision();
        return false;
    }
    if (pending.may_play && !pending.may_play()) {
        pause();
        return false;
    }
    const auto state = standby_audio_producer_->Measurements();
    if (state.failed) {
        if (!pending.failed)
            publishOpenResult(true, QString::fromStdString(standby_audio_producer_->Error()));
        pending.failed = true;
        return false;
    }
    if (pending.failed || state.ready_generation != pending.stamp.stream_generation)
        return false;
    const auto clock = timelineOutputFrames();
    const auto delta = clock >= pending.old_clock_anchor ? clock - pending.old_clock_anchor : 0;
    if (delta > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)() - pending.first_sample))
        return false;
    const auto output_sample = pending.first_sample + static_cast<int64_t>(delta);
    const auto position = standby_audio_producer_->PositionAt(pending.stamp.stream_generation, output_sample);
    if (!position)
        return false;
    if (position->ended) {
        const auto requested = pending.workspace;
        const auto end = position->project_sample / 48 * 1000 + position->project_sample % 48 * 1000 / 48;
        cancelPreparedRevision();
        pause();
        applyTimeline(requested, end, [] { return false; });
        emit transportEnded(transport_command_, request_generation_);
        emit reachedEnd();
        return true;
    }
    if (!standby_audio_producer_->AdvancePreparation(pending.stamp.stream_generation, output_sample))
        return false;
    if (pending.may_play && !pending.may_play()) {
        pause();
        return false;
    }
    const auto requested = pending.workspace;
    const auto stamp = pending.stamp;
    const auto may_play = pending.may_play;
    if (render_audio_) {
        revision_lead_before_reset_frames_ += render_audio_->Measurements().submitted_lead_frames;
        render_audio_->Stop();
    }
    audio_producer_->Cancel();
    std::swap(audio_producer_, standby_audio_producer_);
    pending_revision_.reset();
    audio_stream_generation_ = stamp.stream_generation;
    audio_sample_ = output_sample;
    render_origin_us_ = position->project_sample / 48 * 1000 + position->project_sample % 48 * 1000 / 48;
    audio_start_pending_ = false;
    audible_controls_timeline_.reset();
    audible_control_revision_ = 0;
    sink_->setRevision(request_generation_, requested->revision());
    const bool has_audio = std::any_of(requested->timeline().clips.begin(), requested->timeline().clips.end(),
                                       [](const auto& clip) { return clip.audio; });
    if (!has_audio) {
        audio_producer_->Cancel();
        audio_producer_.reset();
        if (render_audio_)
            render_audio_->Shutdown();
        render_audio_.reset();
    }
    if (render_audio_) {
        render_audio_->StartTimeline([producer = audio_producer_, generation = audio_stream_generation_,
                                      origin = audio_sample_](uint64_t first_frame, std::span<float> samples) {
            producer->Mix(generation, origin + static_cast<int64_t>(first_frame), samples);
        });
    }
    elapsed_.restart();
    adopting_audio_revision_ = true;
    const auto restore = qScopeGuard([&] { adopting_audio_revision_ = false; });
    ++revision_handoffs_;
    applyTimeline(requested, render_origin_us_, may_play ? may_play : [] { return true; });
    adopting_audio_revision_ = false;
    if (may_play && !may_play())
        pause();
    return true;
}
void EditPlayerWorker::applyTimeline(std::shared_ptr<const EditPlaybackSnapshot> workspace,
                                     qint64 requested_position_us, const std::function<bool()>& may_play) {
    const bool resume = may_play ? may_play() : timeline_playing_;
    const auto effective =
        !adopting_audio_revision_ && timeline_mode_ && timeline_playing_ ? timelinePositionUs() : requested_position_us;
    const bool retained = timeline_mode_ && workspace_->retainsMedia(*workspace);
    if (!retained && !adopting_audio_revision_)
        pause();
    standalone_probe_pending_ = false;
    snapshot_position_us_ = std::clamp<qint64>(effective, 0, workspace->duration());
    sink_->setRevision(request_generation_, workspace->revision());
    workspace_ = std::move(workspace);
    timeline_mode_ = true;
    render_mode_ = NeedsComposition(*workspace_);
    render_snapshot_ = makeRenderSnapshot(*workspace_);
    ensureVideoPump();
    if (retained) {
        publishAudioControls();
        publishMonitoring();
        publishOpenResult(true, {});
        return;
    }
    video_snapshot_ = std::make_shared<const engine::TimelineRenderSnapshot>(render_snapshot_);
    render_plan_ = {};
    render_plan_.path = engine::EditExportPath::Render;
    render_plan_.output = BlankGeometry(*workspace_);
    render_plan_.fps_num = workspace_->timeline().output_fps_num;
    render_plan_.fps_den = workspace_->timeline().output_fps_den;
    if (render_plan_.fps_num <= 0)
        for (const auto& source : render_snapshot_.sources)
            if (source.metadata.fps_num > 0) {
                render_plan_.fps_num = source.metadata.fps_num;
                render_plan_.fps_den = source.metadata.fps_den;
                break;
            }
    seekTimeline(snapshot_position_us_, resume && (!may_play || may_play()));
}

void EditPlayerWorker::ensureVideoPump() {
    if (video_pump_)
        return;
    video_pump_ = std::make_unique<VideoPump>([this](VideoPump::Result result) {
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable {
                if (result.kind == VideoPump::Kind::Monitoring) {
                    const auto* asset = workspace_->asset(result.asset);
                    const auto source = metadata_cache_.find(result.asset);
                    if (!asset || result.session != audio_session_generation_ ||
                        result.monitor_request != latest_monitor_request_ ||
                        result.source_version != asset->source_version || source == metadata_cache_.end() ||
                        (result.error.empty() && source->second.access != result.bound_access))
                        return;
                } else if (result.epoch != video_epoch_ ||
                           (result.kind != VideoPump::Kind::Probe && result.generation != request_generation_)) {
                    return;
                }
                if (result.kind == VideoPump::Kind::Probe) {
                    standalone_probe_pending_ = false;
                    if (!result.error.empty() || !result.workspace) {
                        publishOpenResult(false, QString::fromStdString(result.error));
                        return;
                    }
                    applyTimeline(std::move(result.workspace), standalone_position_us_,
                                  [this] { return standalone_play_requested_; });
                    return;
                }
                if (!timeline_mode_)
                    return;
                if (!result.error.empty()) {
                    if (result.kind == VideoPump::Kind::Monitoring) {
                        rejectMonitoring(QString::fromStdString(result.error));
                        publishMonitoring();
                    } else {
                        sink_->clear();
                        publishOpenResult(false, QString::fromStdString(result.error));
                    }
                    return;
                }
                if (result.resolved && result.resolved != video_snapshot_) {
                    bool resolved_source = false;
                    for (const auto& source : result.resolved->sources) {
                        if (const auto* asset = workspace_->asset(source.id);
                            asset && std::filesystem::path(asset->path) == source.path) {
                            metadata_cache_.insert_or_assign(source.id, source);
                            for (auto& current : render_snapshot_.sources)
                                if (current.id == source.id && !current.access && source.access) {
                                    current = source;
                                    resolved_source = true;
                                }
                        }
                    }
                    if (resolved_source)
                        video_snapshot_ = std::make_shared<const engine::TimelineRenderSnapshot>(render_snapshot_);
                }
                if (result.metadata_available) {
                    const auto found = metadata_cache_.find(result.asset);
                    if (found != metadata_cache_.end())
                        found->second.metadata.audio_metadata = result.monitoring_metadata;
                }
                if (result.kind == VideoPump::Kind::Monitoring && result.selection) {
                    workspace_ = workspace_->withMonitorSelection(result.asset, *result.selection);
                    for (auto& source : render_snapshot_.sources)
                        if (source.id == result.asset) {
                            source.monitor_selection = result.selection;
                            source.metadata.audio_metadata = result.monitoring_metadata;
                            metadata_cache_.insert_or_assign(source.id, source);
                        }
                    video_snapshot_ = std::make_shared<const engine::TimelineRenderSnapshot>(render_snapshot_);
                    emit monitorSelectionConfirmed(result.monitor_request, result.asset, *result.selection);
                    renderTimeline(position_us_, engine::VideoReadMode::Sequential);
                    publishMonitoring();
                    return;
                }
                sink_->publishClock(-1);
                if (result.raw_offset && result.frame.layers.size() == 1)
                    sink_->deliver(std::move(result.frame.layers.front().frame), result.generation, *result.raw_offset);
                else
                    sink_->deliverTimeline(std::move(result.frame), result.generation);
                publishMonitoring();
            },
            Qt::QueuedConnection);
    });
}
void EditPlayerWorker::rejectMonitoring(const QString& error) {
    emit monitoringRejectedForCommand(transport_command_, request_generation_, error);
    emit monitoringRejected(error);
}

void EditPlayerWorker::publishMonitoringState(QVariantList tracks, bool confirmation_required) {
    emit monitoringStateChangedForGeneration(request_generation_, tracks, confirmation_required);
    emit monitoringChanged(std::move(tracks), confirmation_required);
}

void EditPlayerWorker::publishMonitoring() {
    const auto* clip = workspace_->clip(active_clip_);
    const auto* asset = clip ? workspace_->asset(clip->asset) : nullptr;
    const auto found = asset ? metadata_cache_.find(asset->id) : metadata_cache_.end();
    if (!asset || found == metadata_cache_.end()) {
        publishMonitoringState({}, false);
        return;
    }
    const auto& metadata = found->second.metadata.audio_metadata;
    QVariantList tracks;
    for (const auto& actual : metadata.actual_tracks) {
        bool selected = false;
        if (asset->input_mix_selection && asset->source_descriptor) {
            size_t index = 0;
            for (const auto& described : asset->source_descriptor->streams) {
                if (described.ref.kind != engine::TimelineStreamKind::Audio)
                    continue;
                if (index++ == actual.stream_index.value) {
                    const auto& chosen = asset->input_mix_selection->selected_stream_identities;
                    selected = std::find(chosen.begin(), chosen.end(), described.ref.stream_identity) != chosen.end();
                    break;
                }
            }
        } else if (asset->monitor_selection) {
            selected =
                std::any_of(asset->monitor_selection->tracks.begin(), asset->monitor_selection->tracks.end(),
                            [&](const auto& choice) { return choice.track_id == actual.track_id && !choice.muted; });
        } else if (metadata.status == engine::AudioMetadataStatus::Verified && metadata.manifest &&
                   !metadata.monitoring_confirmation_required) {
            const auto& defaults = metadata.manifest->default_monitor_track_ids;
            selected = std::find(defaults.begin(), defaults.end(), actual.track_id) != defaults.end();
        }
        tracks.push_back(QVariantMap{{"id", QString::fromStdString(engine::ToUuid(actual.track_id))},
                                     {"name", QString::fromStdString(actual.name)},
                                     {"role", QString::fromStdString(actual.role)},
                                     {"selected", selected},
                                     {"provenanceVerified", metadata.status == engine::AudioMetadataStatus::Verified},
                                     {"available", true}});
    }
    publishMonitoringState(std::move(tracks), asset->source_descriptor ? asset->mix_decision_required
                                                                       : metadata.monitoring_confirmation_required);
}

void EditPlayerWorker::confirmMonitoring(const QStringList& track_ids, bool overlap_confirmed, qulonglong request_id) {
    pause();
    const auto* clip = workspace_->clip(active_clip_);
    const auto* asset = clip ? workspace_->asset(clip->asset) : nullptr;
    const auto found = asset ? metadata_cache_.find(asset->id) : metadata_cache_.end();
    if (!asset || found == metadata_cache_.end() || !video_snapshot_)
        return;
    const auto& metadata = found->second.metadata.audio_metadata;
    engine::MonitorSelectionSnapshot selection;
    selection.media_id = metadata.selection_media_id;
    monitor_revision_ =
        std::max(monitor_revision_, asset->monitor_selection ? asset->monitor_selection->revision.value : 0);
    selection.revision = {++monitor_revision_};
    selection.overlap_confirmed = overlap_confirmed;
    selection.unverified_asset_binding = metadata.unverified_asset_binding;
    for (const auto& text : track_ids) {
        const auto id = engine::FromUuid<engine::AudioTrackId>(text.toStdString());
        if (!id) {
            rejectMonitoring(tr("The audio monitoring track identity is invalid."));
            return;
        }
        selection.tracks.push_back({*id, 1.0f, false});
    }
    auto request = std::make_shared<VideoPump::Request>();
    request->kind = VideoPump::Kind::Monitoring;
    request->snapshot = video_snapshot_;
    request->plan = render_plan_;
    request->position_us = position_us_;
    request->asset = asset->id;
    request->selection = std::move(selection);
    request->monitor_request = request_id;
    latest_monitor_request_ = request_id;
    request->session = audio_session_generation_;
    request->source_version = asset->source_version;
    request->epoch = video_epoch_;
    request->generation = request_generation_;
    request->composed = render_mode_;
    request->before_open = video_before_open_;
    video_pump_->Queue(std::move(request));
}

void EditPlayerWorker::setVolume(double volume) {
    volume_ = volume;
    publishAudioControls();
}

void EditPlayerWorker::seekTimeline(qint64 target_us, bool resume, bool preserve_audio) {
    if (!preserve_audio && !adopting_audio_revision_)
        pause();
    position_us_ = std::clamp<qint64>(target_us, 0, workspace_->duration());
    position_ms_ = position_us_ / 1000;
    if (!adopting_audio_revision_ && !preserve_audio) {
        render_origin_us_ = position_us_;
        audio_sample_ = SampleAt(position_us_);
    }
    const auto* clip = EvaluatedClip(*workspace_, position_us_);
    active_clip_ = clip ? clip->id : 0;
    publishMonitoring();
    ensureVideoPump();
    renderTimeline(position_us_, engine::VideoReadMode::RandomAccess);
    timeline_playing_ = resume;
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(
        render_plan_.fps_den > 0 ? static_cast<double>(render_plan_.fps_num) / render_plan_.fps_den : 0, screen_hz_));
    if (!adopting_audio_revision_ && !preserve_audio)
        elapsed_.restart();
    if (resume) {
        if (!preserve_audio && !startTimelineAudio(position_us_)) {
            pause();
            return;
        }
        tick_timer_->start();
    } else {
        tick_timer_->stop();
    }
    publishOpenResult(workspace_->duration() > 0, {});
    publishPlaybackState(timeline_playing_);
}

bool EditPlayerWorker::renderTimeline(qint64 position_us, engine::VideoReadMode mode) {
    if (!video_pump_ || !video_snapshot_)
        return false;
    if (mode == engine::VideoReadMode::RandomAccess) {
        ++video_seeks_;
        ++video_epoch_;
        video_pump_->CancelVideo();
    }
    auto request = std::make_shared<VideoPump::Request>();
    request->snapshot = video_snapshot_;
    request->plan = render_plan_;
    request->position_us = position_us;
    request->mode = mode;
    request->epoch = video_epoch_;
    request->generation = request_generation_;
    request->composed = render_mode_;
    request->before_read = video_before_read_;
    request->before_open = video_before_open_;
    video_pump_->Queue(std::move(request));
    return true;
}

void EditPlayerWorker::presentBlank(qint64 position_us) {
    sink_->publishClock(-1);
    sink_->deliverTimeline({position_us, BlankGeometry(*workspace_), {}}, request_generation_);
}

uint64_t EditPlayerWorker::timelineOutputFrames() const {
    uint64_t output_sample = 0;
    if (render_audio_ && render_audio_->SampleRate() > 0) {
        const auto rate = render_audio_->SampleRate();
        const auto played = render_audio_->FramesPlayed();
        output_sample = played / rate * 48000 + played % rate * 48000 / rate;
    } else {
        output_sample = static_cast<uint64_t>(std::max<qint64>(0, elapsed_.nsecsElapsed())) / 1000 * 48 / 1000;
    }
    return output_sample;
}

qint64 EditPlayerWorker::timelinePositionUs() const {
    if (audio_start_pending_)
        return render_origin_us_;
    const auto output_sample = timelineOutputFrames();
    if (audio_producer_) {
        if (const auto position = audio_producer_->PositionAt(audio_stream_generation_,
                                                              audio_sample_ + static_cast<int64_t>(output_sample)))
            return position->project_sample / 48 * 1000 + (position->project_sample % 48 * 1000 + 24) / 48;
    }
    const auto region = playback_bounds_.end_us > playback_bounds_.start_us
                            ? playback_bounds_
                            : edit::PlaybackInterval{0, workspace_->duration()};
    const auto elapsed_us = static_cast<qint64>(output_sample / 48 * 1000 + output_sample % 48 * 1000 / 48);
    const auto through = render_origin_us_ + elapsed_us;
    if (repeat_ && region.end_us > region.start_us && through >= region.end_us)
        return region.start_us + (through - region.start_us) % (region.end_us - region.start_us);
    return std::min(through, region.end_us);
}

bool EditPlayerWorker::publishAudioControls() {
    const auto revision = ++audio_control_revision_;
    const auto publish = [&](const std::shared_ptr<engine::TimelineAudioProducer>& producer,
                             const engine::TimelineSnapshot& timeline, const engine::TimelineAudioStamp& stamp) {
        if (!producer)
            return true;
        const auto controls =
            engine::BuildTimelineAudioControls(timeline, stamp, static_cast<float>(volume_), solo_tracks_, revision);
        if (!controls.succeeded()) {
            publishOpenResult(true, QString::fromStdString(controls.error()->context));
            return false;
        }
        if (!producer->SetControls(*controls.value())) {
            publishOpenResult(true, tr("Audio controls are waiting for an earlier output block to finish."));
            return false;
        }
        return true;
    };
    const engine::TimelineAudioStamp current{audio_session_generation_, workspace_->revision(),
                                             audio_stream_generation_};
    bool result = publish(audio_producer_,
                          audible_controls_timeline_ ? *audible_controls_timeline_ : workspace_->timeline(), current);
    if (pending_revision_)
        result = publish(standby_audio_producer_, pending_revision_->workspace->timeline(), pending_revision_->stamp) &&
                 result;
    return result;
}
void EditPlayerWorker::setPlaybackBounds(edit::PlaybackInterval interval, bool repeat) {
    playback_bounds_ = interval;
    repeat_ = repeat;
}

void EditPlayerWorker::setSoloTracks(std::vector<uint64_t> tracks) {
    solo_tracks_ = std::move(tracks);
    publishAudioControls();
}

bool EditPlayerWorker::startTimelineAudio(qint64 position_us) {
    if (adopting_audio_revision_)
        return true;
    if (render_audio_)
        render_audio_->Stop();
    render_origin_us_ = position_us;
    audio_sample_ = SampleAt(position_us);
    audio_stream_generation_ = next_audio_stream_generation_ =
        (std::max)(next_audio_stream_generation_, audio_stream_generation_) + 1;
    last_loop_iteration_ = 0;
    audio_start_pending_ = false;
    audio_output_error_.clear();
    const auto& timeline = workspace_->timeline();
    const bool has_audio =
        std::any_of(timeline.clips.begin(), timeline.clips.end(), [](const auto& clip) { return clip.audio; });
    if (!has_audio) {
        if (audio_producer_)
            audio_producer_->Cancel();
        if (render_audio_)
            render_audio_->Shutdown();
        render_audio_.reset();
        elapsed_.restart();
        return true;
    }
    if (!audio_producer_)
        audio_producer_ = std::make_shared<engine::TimelineAudioProducer>();
    const engine::TimelineAudioStamp stamp{audio_session_generation_, workspace_->revision(), audio_stream_generation_};
    const auto region = playback_bounds_.end_us > playback_bounds_.start_us
                            ? playback_bounds_
                            : edit::PlaybackInterval{0, workspace_->duration()};
    audio_producer_->Prepare(std::make_shared<const engine::TimelineRenderSnapshot>(render_snapshot_), stamp,
                             audio_sample_, audio_before_read_,
                             engine::TimelineAudioRegion{SampleAt(region.start_us), SampleAt(region.end_us), repeat_});
    if (!publishAudioControls())
        return false;
    if (!render_audio_) {
        std::string error;
        auto output = std::make_unique<engine::WasapiAudioRenderer>();
        if (output->InitTimeline(error))
            render_audio_ = std::move(output);
        else
            audio_output_error_ = QString::fromStdString(error);
    }
    audio_start_pending_ = true;
    elapsed_.restart();
    return true;
}

bool EditPlayerWorker::fillTimelineAudio() {
    if (!audio_producer_)
        return true;
    const auto state = audio_producer_->Measurements();
    if (state.failed) {
        publishOpenResult(false, QString::fromStdString(audio_producer_->Error()));
        return false;
    }
    if (!audio_start_pending_) {
        if (render_audio_ && !render_audio_->Running() && timeline_playing_) {
            if (render_audio_->Measurements().timeline_recovery_pending) {
                const auto current = timelinePositionUs();
                const auto end = playback_bounds_.end_us > playback_bounds_.start_us ? playback_bounds_.end_us
                                                                                     : workspace_->duration();
                if (!repeat_ && current >= end) {
                    render_audio_->Stop();
                    audio_producer_->Cancel();
                    return true;
                }
                return startTimelineAudio(current);
            }
            publishOpenResult(false, tr("The audio output stopped unexpectedly."));
            return false;
        }
        return true;
    }
    if (state.ready_generation != audio_stream_generation_)
        return true;
    if (render_audio_) {
        render_audio_->StartTimeline([producer = audio_producer_, generation = audio_stream_generation_,
                                      origin = audio_sample_](uint64_t first_frame, std::span<float> samples) {
            producer->Mix(generation, origin + static_cast<int64_t>(first_frame), samples);
        });
        if (!render_audio_->Running()) {
            publishOpenResult(false, tr("The audio output could not start."));
            return false;
        }
    }
    audio_start_pending_ = false;
    elapsed_.restart();
    return true;
}

void EditPlayerWorker::tickRenderTimeline() {
    if (!timeline_playing_ || !video_pump_)
        return;
    const auto previous = position_us_;
    position_us_ = timelinePositionUs();
    position_us_ = std::min<qint64>(position_us_, workspace_->duration());
    position_ms_ = position_us_ / 1000;
    snapshot_position_us_ = position_us_;
    const auto* visible = EvaluatedClip(*workspace_, position_us_);
    active_clip_ = visible ? visible->id : 0;
    if (!fillTimelineAudio())
        return;
    if (!renderTimeline(position_us_, repeat_ && position_us_ < previous ? engine::VideoReadMode::RandomAccess
                                                                         : engine::VideoReadMode::Sequential))
        return;
    publishPosition();
    const auto end =
        playback_bounds_.end_us > playback_bounds_.start_us ? playback_bounds_.end_us : workspace_->duration();
    if (!pending_revision_ && !repeat_ && position_us_ >= end) {
        pause();
        emit transportEnded(transport_command_, request_generation_);
        emit reachedEnd();
    }
}

} // namespace exosnap::quick
