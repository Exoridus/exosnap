#include <capability/translatable.h>
#include <exosnap/engine/edit_timeline_audio.h>

#include "brickwall_limiter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace exosnap::engine {
namespace {
constexpr uint32_t kQuantum = 480;
constexpr size_t kLanes = 8;
constexpr size_t kSlots = 64;
static_assert(std::atomic<uint64_t>::is_always_lock_free);

EngineError AudioError(std::string message) {
    EngineError error;
    error.code = EngineErrorCode::InvalidConfig;
    error.owner = EngineModule::Editor;
    error.context = std::move(message);
    return error;
}

const TimelineAudioControlInterval* IntervalAt(const TimelineAudioControls& controls, int64_t sample) {
    const auto after = std::upper_bound(controls.intervals.begin(), controls.intervals.end(), sample,
                                        [](int64_t at, const auto& interval) { return at < interval.first_sample; });
    if (after == controls.intervals.begin())
        return nullptr;
    const auto& interval = *std::prev(after);
    return sample < interval.end_sample ? &interval : nullptr;
}

template <typename T> bool PinPublished(const std::atomic<T*>& published, std::atomic<T*>& hazard, T*& value) noexcept {
    static_assert(std::atomic<T*>::is_always_lock_free);
    // Retirement and pin validation need one total order across the two atomic pointers.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        value = published.load(std::memory_order_seq_cst);
        hazard.store(value, std::memory_order_seq_cst);
        if (value == published.load(std::memory_order_seq_cst))
            return true;
    }
    hazard.store(nullptr, std::memory_order_seq_cst);
    value = nullptr;
    return false;
}

void Maximum(std::atomic<uint64_t>& value, uint64_t candidate) {
    auto previous = value.load(std::memory_order_relaxed);
    while (previous < candidate && !value.compare_exchange_weak(previous, candidate, std::memory_order_relaxed)) {
    }
}
} // namespace

Outcome<std::shared_ptr<const TimelineAudioControls>>
BuildTimelineAudioControls(const TimelineSnapshot& timeline, const TimelineAudioStamp& stamp, float monitor_gain,
                           std::span<const uint64_t> solo_tracks, uint64_t control_revision) {
    if (!std::isfinite(monitor_gain) || monitor_gain < 0 || monitor_gain > 1)
        return AudioError(EXOSNAP_TRANSLATABLE("EditRender", "Invalid audio monitor gain."));
    auto included = timeline;
    for (auto& clip : included.clips)
        if (clip.audio)
            clip.enabled = true;
    for (auto& track : included.tracks)
        if (track.audio)
            track.enabled = true;
    const auto evaluation = EvaluateAudioIntervals(included, 0, included.duration_us);
    if (!evaluation.error.empty())
        return AudioError(evaluation.error);
    auto result = std::make_shared<TimelineAudioControls>();
    result->stamp = stamp;
    result->control_revision = control_revision ? control_revision : stamp.revision;
    result->monitor_gain = monitor_gain;
    const auto tracks = timeline_detail::Tracks(timeline);
    for (const auto& interval : evaluation.intervals) {
        TimelineAudioControlInterval row{AudioSampleIndex(interval.in_us), AudioSampleIndex(interval.out_us), {}};
        if (row.end_sample <= row.first_sample)
            continue;
        if (interval.contributions.size() > kLanes)
            return AudioError(EXOSNAP_TRANSLATABLE("EditRender", "Too many simultaneous timeline audio sources."));
        for (const auto& contribution : interval.contributions) {
            const auto clip = std::find_if(timeline.clips.begin(), timeline.clips.end(),
                                           [&](const auto& value) { return value.id == contribution.clip; });
            const auto track = std::find_if(tracks.begin(), tracks.end(),
                                            [&](const auto& value) { return value.id == contribution.track; });
            const bool solo = solo_tracks.empty() || std::find(solo_tracks.begin(), solo_tracks.end(),
                                                               contribution.track) != solo_tracks.end();
            const bool audible =
                clip != timeline.clips.end() && clip->enabled && track != tracks.end() && track->enabled && solo;
            row.contributions.push_back({contribution.clip, audible, contribution});
        }
        result->intervals.push_back(std::move(row));
    }
    return std::shared_ptr<const TimelineAudioControls>(std::move(result));
}

struct TimelineAudioProducer::Impl {
    struct Layer {
        std::array<uint64_t, kQuantum> clips{};
        std::array<float, kQuantum * 2> samples{};
    };
    struct Block {
        int64_t first_output_sample = 0;
        uint32_t count = 0;
        std::array<int64_t, kQuantum> project_samples{};
        std::array<Layer, kLanes> layers{};
    };
    struct Stream {
        TimelineAudioStamp stamp;
        int64_t origin = 0;
        int64_t timeline_end = 0;
        std::optional<TimelineAudioRegion> region;
        std::array<Block, kSlots> blocks{};
        std::atomic<uint64_t> written{0};
        std::atomic<uint64_t> read{0};
        std::atomic<int64_t> produced_through{0};
        std::atomic<int64_t> consumed_through{0};
        std::atomic<bool> revoked{false};
        std::atomic<bool> endpoint_started{false};
        std::vector<TimelineSourceAudio> loop_head;
        uint32_t loop_head_frames = 0;
        uint64_t loop_head_bytes = 0;
        std::atomic<uint64_t> loop_head_reuses{0};
    };
    struct Request {
        std::shared_ptr<const TimelineRenderSnapshot> snapshot;
        TimelineAudioStamp stamp;
        int64_t first_sample = 0;
        Admission before_read;
        std::optional<TimelineAudioRegion> region;
        std::atomic<bool> cancelled{false};
    };

    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::shared_ptr<Request> pending;
    std::shared_ptr<Request> active;
    std::thread worker;
    std::atomic<std::shared_ptr<Stream>> stream;
    std::atomic<std::shared_ptr<const TimelineAudioControls>> controls;
    std::atomic<Stream*> endpoint_stream{nullptr};
    std::atomic<const TimelineAudioControls*> endpoint_controls{nullptr};
    std::atomic<Stream*> stream_hazard{nullptr};
    std::atomic<const TimelineAudioControls*> controls_hazard{nullptr};
    std::atomic<uint64_t> publication_misses{0};
    struct EndpointRead {
        Impl& owner;
        ~EndpointRead() {
            owner.controls_hazard.store(nullptr, std::memory_order_seq_cst);
            owner.stream_hazard.store(nullptr, std::memory_order_seq_cst);
        }
    };
    std::vector<std::shared_ptr<Stream>> retired_streams;
    std::vector<std::shared_ptr<const TimelineAudioControls>> retired_controls;
    std::string error;
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> requested_revision{0};
    std::atomic<uint64_t> applied_revision{0};
    std::atomic<uint64_t> requested_control{0};
    std::atomic<uint64_t> applied_control{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> stale{0};
    std::atomic<uint64_t> peak_decoders{0};
    std::atomic<uint64_t> lookahead_deferred{0};
    std::atomic<uint64_t> decoder_opens{0};
    std::atomic<uint64_t> cursor_reuses{0};
    std::atomic<uint64_t> prepared_source_ranges{0};
    std::atomic<uint64_t> prepared_cache_hits{0};
    std::atomic<uint64_t> audio_seeks{0};
    std::atomic<uint64_t> decoded_frames{0};
    TimelineAudioReadMeasurements previous_reader_stats;
    std::atomic<uint64_t> peak_retired_controls{0};
    std::atomic<uint64_t> rejected_controls{0};
    BrickwallLimiter limiter{BrickwallLimiter::Config{}};
    uint64_t output_session = 0;
    uint64_t output_generation = 0;

    Impl() {
        worker = std::thread([this] { Run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            RevokeRequests();
        }
        changed.notify_all();
        worker.join();
    }
    void RevokeRequests() {
        for (const auto& request : {pending, active})
            if (request) {
                request->cancelled.store(true, std::memory_order_release);
                request->cancelled.notify_all();
            }
    }
    void Reclaim() {
        std::lock_guard lock(mutex);
        std::erase_if(retired_controls, [&](const auto& value) {
            return value.use_count() == 1 && controls_hazard.load(std::memory_order_seq_cst) != value.get();
        });
        std::erase_if(retired_streams, [&](const auto& value) {
            return value.use_count() == 1 && stream_hazard.load(std::memory_order_seq_cst) != value.get();
        });
    }
    void Fail(std::string message) {
        std::lock_guard lock(mutex);
        error = std::move(message);
        failed.store(true, std::memory_order_release);
    }
    static std::optional<TimelineAudioPosition> Position(const Stream& value, int64_t output) {
        if (output < value.origin)
            return std::nullopt;
        if (!value.region)
            return TimelineAudioPosition{(std::min)(output, value.timeline_end), 0, output >= value.timeline_end};
        const auto& region = *value.region;
        const auto delta = output - value.origin;
        const auto head = region.out_sample - value.origin;
        if (delta < head)
            return TimelineAudioPosition{value.origin + delta, 0, false};
        if (!region.repeat)
            return TimelineAudioPosition{region.out_sample, 0, true};
        const auto length = region.out_sample - region.in_sample;
        return TimelineAudioPosition{region.in_sample + (delta - head) % length,
                                     static_cast<uint64_t>(1 + (delta - head) / length), false};
    }

    void PublishReaderStats(const EditTimelineReader& reader) {
        const auto stats = reader.AudioReadStats();
        decoder_opens.fetch_add(stats.decoder_opens - previous_reader_stats.decoder_opens);
        cursor_reuses.fetch_add(stats.cursor_reuses - previous_reader_stats.cursor_reuses);
        prepared_source_ranges.fetch_add(stats.prepared_source_ranges - previous_reader_stats.prepared_source_ranges);
        prepared_cache_hits.fetch_add(stats.prepared_cache_hits - previous_reader_stats.prepared_cache_hits);
        audio_seeks.fetch_add(stats.seeks - previous_reader_stats.seeks);
        decoded_frames.fetch_add(stats.decoded_frames - previous_reader_stats.decoded_frames);
        previous_reader_stats = stats;
        Maximum(peak_decoders, reader.PeakDecoders());
    }
    bool Fill(EditTimelineReader& reader, const TimelineAudioControls& schedule, Request& request, Stream& destination,
              int64_t cursor, Block& block) {
        block = {};
        block.first_output_sample = cursor;
        std::vector<TimelineSourceAudio> decoded;
        uint32_t filled = 0;
        while (filled < kQuantum && !request.cancelled.load(std::memory_order_acquire)) {
            const auto position = Position(destination, cursor + filled);
            if (!position || position->ended)
                break;
            const auto* interval = IntervalAt(schedule, position->project_sample);
            auto boundary = interval ? interval->end_sample : destination.timeline_end;
            if (destination.region)
                boundary = (std::min)(boundary, destination.region->out_sample);
            const auto remaining = boundary - position->project_sample;
            if (remaining <= 0) {
                Fail(EXOSNAP_TRANSLATABLE("EditRender", "Invalid audio interval boundary."));
                return false;
            }
            auto count = static_cast<uint32_t>((std::min)(int64_t{kQuantum - filled}, remaining));
            const auto head_offset = destination.region ? position->project_sample - destination.region->in_sample : -1;
            const bool cached_head = head_offset >= 0 && head_offset < destination.loop_head_frames;
            if (cached_head)
                count = (std::min)(count, destination.loop_head_frames - static_cast<uint32_t>(head_offset));
            for (uint32_t frame = 0; frame < count; ++frame)
                block.project_samples[filled + frame] = position->project_sample + frame;
            if (cached_head) {
                const auto offset = static_cast<size_t>(head_offset);
                destination.loop_head_reuses.fetch_add(1, std::memory_order_relaxed);
                for (size_t lane = 0; lane < destination.loop_head.size(); ++lane) {
                    const auto& source = destination.loop_head[lane];
                    for (uint32_t frame = 0; frame < count; ++frame) {
                        block.layers[lane].clips[filled + frame] = source.clip;
                        for (size_t channel = 0; channel < 2; ++channel)
                            block.layers[lane].samples[(filled + frame) * 2 + channel] =
                                source.interleaved_stereo[(offset + frame) * 2 + channel];
                    }
                }
            } else {
                std::string message;
                if (!reader.SourceAudioAt(position->project_sample, count, decoded, message)) {
                    if (!request.cancelled.load())
                        Fail(std::move(message));
                    return false;
                }
                for (size_t lane = 0; lane < decoded.size(); ++lane) {
                    for (uint32_t frame = 0; frame < count; ++frame) {
                        block.layers[lane].clips[filled + frame] = decoded[lane].clip;
                        for (size_t channel = 0; channel < 2; ++channel)
                            block.layers[lane].samples[(filled + frame) * 2 + channel] =
                                decoded[lane].interleaved_stereo[frame * 2 + channel];
                    }
                }
            }
            filled += count;
        }
        block.count = filled;
        Maximum(peak_decoders, reader.PeakDecoders());
        return filled > 0;
    }

    void Produce(const std::shared_ptr<Request>& request) {
        if (!request->snapshot || request->first_sample < 0) {
            Fail(EXOSNAP_TRANSLATABLE("EditRender", "Invalid audio preparation request."));
            return;
        }
        const auto timeline_end = AudioSampleIndex(request->snapshot->timeline.duration_us);
        if (request->region &&
            (request->region->in_sample < 0 || request->region->out_sample <= request->region->in_sample ||
             request->region->out_sample > timeline_end || request->first_sample < request->region->in_sample ||
             request->first_sample >= request->region->out_sample)) {
            Fail(EXOSNAP_TRANSLATABLE("EditRender", "Invalid audio playback region."));
            return;
        }
        const auto schedule = BuildTimelineAudioControls(request->snapshot->timeline, request->stamp);
        if (!schedule.succeeded()) {
            Fail(schedule.error()->context);
            return;
        }
        previous_reader_stats = {};
        EditTimelineReader reader;
        EditRenderPlan plan;
        plan.path = EditExportPath::Render;
        std::string message;
        if (!reader.Open(*request->snapshot, plan, message, &request->cancelled)) {
            if (!request->cancelled.load())
                Fail(std::move(message));
            return;
        }
        auto prepared = std::make_shared<Stream>();
        prepared->stamp = request->stamp;
        prepared->origin = request->first_sample;
        prepared->timeline_end = timeline_end;
        prepared->region = request->region;
        prepared->produced_through.store(request->first_sample);
        prepared->consumed_through.store(request->first_sample);
        if (request->region && request->region->repeat) {
            auto end = (std::min)(request->region->out_sample, request->region->in_sample + kQuantum);
            if (const auto* interval = IntervalAt(**schedule.value(), request->region->in_sample))
                end = (std::min)(end, interval->end_sample);
            prepared->loop_head_frames = static_cast<uint32_t>(end - request->region->in_sample);
            if (request->before_read && !request->before_read(request->cancelled))
                return;
            if (request->cancelled.load())
                return;
            if (!reader.SourceAudioAt(request->region->in_sample, prepared->loop_head_frames, prepared->loop_head,
                                      message)) {
                if (!request->cancelled.load())
                    Fail(std::move(message));
                return;
            }
            for (const auto& source : prepared->loop_head)
                prepared->loop_head_bytes += source.interleaved_stereo.capacity() * sizeof(float);
            PublishReaderStats(reader);
        }
        bool published = false;
        bool warmed = false;
        auto cursor = request->first_sample;
        while (!request->cancelled.load(std::memory_order_acquire)) {
            Reclaim();
            const auto write = prepared->written.load(std::memory_order_relaxed);
            const auto read = prepared->read.load(std::memory_order_acquire);
            if (write - read >= kSlots) {
                if (!warmed) {
                    const auto position = Position(*prepared, cursor);
                    if (position && !position->ended) {
                        const auto through = (std::min)(timeline_end, position->project_sample + 96000);
                        uint64_t deferred = 0;
                        if (!reader.PrepareAudioLookahead(position->project_sample, through, message, &deferred) &&
                            !request->cancelled.load()) {
                            Fail(std::move(message));
                            return;
                        }
                        lookahead_deferred.fetch_add(deferred, std::memory_order_relaxed);
                        PublishReaderStats(reader);
                    }
                    warmed = true;
                }
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            warmed = false;
            cursor = (std::max)(cursor, prepared->consumed_through.load(std::memory_order_acquire));
            if (request->before_read && !request->before_read(request->cancelled))
                return;
            if (request->cancelled.load())
                return;
            auto& block = prepared->blocks[write % kSlots];
            if (!Fill(reader, **schedule.value(), *request, *prepared, cursor, block)) {
                if (request->cancelled.load() || failed.load())
                    return;
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            if (request->cancelled.load(std::memory_order_acquire))
                return;
            cursor += block.count;
            prepared->produced_through.store(cursor, std::memory_order_release);
            prepared->written.store(write + 1, std::memory_order_release);
            if (!published) {
                auto previous = stream.exchange(prepared, std::memory_order_acq_rel);
                // Publish before retirement. The endpoint either pins the old owner or retries without dereferencing
                // it.
                endpoint_stream.store(prepared.get(), std::memory_order_seq_cst);
                if (previous)
                    retired_streams.push_back(std::move(previous));
                published = true;
            }
        }
    }

    void Run() {
        for (;;) {
            Reclaim();
            std::shared_ptr<Request> request;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(5), [&] { return stopping || pending; });
                if (stopping)
                    return;
                request = std::move(pending);
                if (!request)
                    continue;
                active = request;
            }
            try {
                Produce(request);
            } catch (const std::exception& exception) {
                Fail(exception.what());
            } catch (...) {
                Fail(EXOSNAP_TRANSLATABLE("EditRender", "Audio preparation failed."));
            }
            {
                std::lock_guard lock(mutex);
                if (active == request)
                    active.reset();
            }
        }
    }
};

TimelineAudioProducer::TimelineAudioProducer() : impl_(std::make_unique<Impl>()) {
}
TimelineAudioProducer::~TimelineAudioProducer() {
    Cancel();
}
void TimelineAudioProducer::Prepare(std::shared_ptr<const TimelineRenderSnapshot> snapshot, TimelineAudioStamp stamp,
                                    int64_t first_sample, Admission before_read,
                                    std::optional<TimelineAudioRegion> region) {
    auto request = std::make_shared<Impl::Request>();
    request->snapshot = std::move(snapshot);
    request->stamp = stamp;
    request->first_sample = first_sample;
    request->before_read = std::move(before_read);
    request->region = region;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->RevokeRequests();
        impl_->pending = std::move(request);
        impl_->error.clear();
        impl_->failed.store(false, std::memory_order_release);
    }
    impl_->requested_revision.store(stamp.revision, std::memory_order_release);
    impl_->changed.notify_all();
}
bool TimelineAudioProducer::SetControls(std::shared_ptr<const TimelineAudioControls> controls) {
    if (!controls)
        return false;
    std::lock_guard lock(impl_->mutex);
    std::erase_if(impl_->retired_controls, [&](const auto& value) {
        return value.use_count() == 1 && impl_->controls_hazard.load(std::memory_order_seq_cst) != value.get();
    });
    if (impl_->retired_controls.size() >= 4) {
        impl_->rejected_controls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    impl_->requested_revision.store(controls->stamp.revision, std::memory_order_release);
    impl_->requested_control.store(controls->control_revision, std::memory_order_release);
    const auto* published = controls.get();
    auto previous = impl_->controls.exchange(std::move(controls), std::memory_order_acq_rel);
    impl_->endpoint_controls.store(published, std::memory_order_seq_cst);
    if (previous &&
        (previous.use_count() > 1 || impl_->controls_hazard.load(std::memory_order_seq_cst) == previous.get())) {
        impl_->retired_controls.push_back(std::move(previous));
        Maximum(impl_->peak_retired_controls, impl_->retired_controls.size());
    }
    impl_->changed.notify_all();
    return true;
}
bool TimelineAudioProducer::AdvancePreparation(uint64_t generation, int64_t first_sample, uint32_t required_frames) {
    const auto stream = impl_->stream.load(std::memory_order_acquire);
    if (!stream || stream->stamp.stream_generation != generation || stream->revoked.load(std::memory_order_acquire) ||
        stream->endpoint_started.load(std::memory_order_acquire) || first_sample < stream->origin || !required_frames ||
        required_frames > kSlots * kQuantum || first_sample > (std::numeric_limits<int64_t>::max)() - required_frames)
        return false;
    const auto consumed = stream->consumed_through.load(std::memory_order_relaxed);
    if (first_sample < consumed)
        return false;
    const auto position = Impl::Position(*stream, first_sample);
    if (!position || position->ended)
        return false;
    if (!stream->region || !stream->region->repeat) {
        const auto end = stream->region ? stream->region->out_sample : stream->timeline_end;
        required_frames = static_cast<uint32_t>((std::min)(int64_t{required_frames}, end - first_sample));
    }
    stream->consumed_through.store(first_sample, std::memory_order_release);
    auto read = stream->read.load(std::memory_order_relaxed);
    const auto written = stream->written.load(std::memory_order_acquire);
    while (read < written) {
        const auto& block = stream->blocks[read % kSlots];
        if (first_sample < block.first_output_sample + block.count)
            break;
        ++read;
    }
    stream->read.store(read, std::memory_order_release);
    impl_->changed.notify_all();
    return read < written && stream->blocks[read % kSlots].first_output_sample <= first_sample &&
           stream->blocks[(written - 1) % kSlots].first_output_sample + stream->blocks[(written - 1) % kSlots].count >=
               first_sample + required_frames;
}
void TimelineAudioProducer::Cancel() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->RevokeRequests();
        impl_->pending.reset();
    }
    if (auto stream = impl_->stream.load(std::memory_order_acquire))
        stream->revoked.store(true, std::memory_order_release);
    impl_->changed.notify_all();
}
std::optional<TimelineAudioPosition> TimelineAudioProducer::PositionAt(uint64_t generation,
                                                                       int64_t output_sample) const {
    const auto stream = impl_->stream.load(std::memory_order_acquire);
    if (!stream || stream->stamp.stream_generation != generation || stream->revoked.load())
        return std::nullopt;
    return Impl::Position(*stream, output_sample);
}
std::string TimelineAudioProducer::Error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}
void TimelineAudioProducer::Mix(uint64_t generation, int64_t first_sample, std::span<float> output) noexcept {
    std::fill(output.begin(), output.end(), 0.0f);
    if (output.size() % 2 || first_sample < 0 ||
        output.size() / 2 > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)() - first_sample))
        return;
    const Impl::EndpointRead read_guard{*impl_};
    Impl::Stream* stream = nullptr;
    const TimelineAudioControls* controls = nullptr;
    if (!PinPublished(impl_->endpoint_stream, impl_->stream_hazard, stream) ||
        !PinPublished(impl_->endpoint_controls, impl_->controls_hazard, controls)) {
        impl_->publication_misses.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!stream || !controls || stream->revoked.load(std::memory_order_acquire) ||
        stream->stamp.stream_generation != generation || controls->stamp.stream_generation != generation ||
        stream->stamp.session != controls->stamp.session || controls->stamp.revision < stream->stamp.revision) {
        impl_->stale.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    stream->endpoint_started.store(true, std::memory_order_release);
    if (impl_->output_generation != generation || impl_->output_session != stream->stamp.session) {
        impl_->limiter.Reset();
        impl_->output_generation = generation;
        impl_->output_session = stream->stamp.session;
    }
    bool applied = false;
    const auto frames = output.size() / 2;
    for (size_t frame = 0; frame < frames; ++frame) {
        const auto at = first_sample + static_cast<int64_t>(frame);
        auto read = stream->read.load(std::memory_order_relaxed);
        const auto write = stream->written.load(std::memory_order_acquire);
        while (read < write) {
            const auto& block = stream->blocks[read % kSlots];
            if (at < block.first_output_sample + block.count)
                break;
            stream->read.store(++read, std::memory_order_release);
        }
        if (read == write) {
            const auto position = Impl::Position(*stream, at);
            if (position && !position->ended)
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto& block = stream->blocks[read % kSlots];
        if (at < block.first_output_sample) {
            impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const auto offset = static_cast<size_t>(at - block.first_output_sample);
        const auto project = block.project_samples[offset];
        const auto* interval = IntervalAt(*controls, project);
        applied = true;
        if (!interval)
            continue;
        for (const auto& layer : block.layers) {
            const auto clip = layer.clips[offset];
            if (!clip)
                continue;
            const auto control = std::find_if(interval->contributions.begin(), interval->contributions.end(),
                                              [&](const auto& value) { return value.clip == clip; });
            if (control == interval->contributions.end() || !control->audible)
                continue;
            const auto gain = EvaluateAudioGain(control->evaluation, project);
            if (!gain)
                continue;
            for (size_t channel = 0; channel < 2; ++channel)
                output[frame * 2 + channel] +=
                    layer.samples[offset * 2 + channel] * static_cast<float>(*gain) * controls->monitor_gain;
        }
    }
    stream->consumed_through.store(first_sample + static_cast<int64_t>(frames), std::memory_order_release);
    for (size_t offset = 0; offset < frames; offset += kQuantum)
        impl_->limiter.Process(output.data() + offset * 2,
                               static_cast<uint32_t>((std::min)(size_t{kQuantum}, frames - offset)));
    if (applied) {
        impl_->applied_revision.store(controls->stamp.revision, std::memory_order_release);
        impl_->applied_control.store(controls->control_revision, std::memory_order_release);
    }
}
TimelineAudioMeasurements TimelineAudioProducer::Measurements() const {
    TimelineAudioMeasurements result;
    result.endpoint_publication_lock_free =
        impl_->endpoint_stream.is_lock_free() && impl_->stream_hazard.is_lock_free() &&
        impl_->endpoint_controls.is_lock_free() && impl_->controls_hazard.is_lock_free();
    result.publication_misses = impl_->publication_misses.load(std::memory_order_relaxed);
    result.requested_revision = impl_->requested_revision.load();
    result.applied_revision = impl_->applied_revision.load();
    result.requested_control_revision = impl_->requested_control.load();
    result.applied_control_revision = impl_->applied_control.load();
    result.underrun_frames = impl_->underruns.load();
    result.stale_blocks = impl_->stale.load();
    result.peak_decoders = impl_->peak_decoders.load();
    result.lookahead_deferred = impl_->lookahead_deferred.load();
    result.source_reads = {
        impl_->decoder_opens.load(),       impl_->cursor_reuses.load(), impl_->prepared_source_ranges.load(),
        impl_->prepared_cache_hits.load(), impl_->audio_seeks.load(),   impl_->decoded_frames.load()};
    result.peak_retired_control_tables = impl_->peak_retired_controls.load();
    result.rejected_controls = impl_->rejected_controls.load();
    {
        std::lock_guard lock(impl_->mutex);
        result.retired_control_tables = impl_->retired_controls.size();
    }
    result.failed = impl_->failed.load();
    if (const auto stream = impl_->stream.load()) {
        result.stream_generation = stream->stamp.stream_generation;
        result.ready_generation = stream->revoked.load() ? 0 : stream->stamp.stream_generation;
        result.source_capacity_frames = kSlots * kQuantum;
        result.source_bytes = sizeof(stream->blocks) + stream->loop_head_bytes;
        result.loop_head_frames = stream->loop_head_frames;
        result.loop_head_bytes = stream->loop_head_bytes;
        result.loop_head_reuses = stream->loop_head_reuses.load(std::memory_order_relaxed);
        result.source_buffer_frames = static_cast<uint64_t>(
            (std::max)(int64_t{0}, stream->produced_through.load() - stream->consumed_through.load()));
    }
    return result;
}

} // namespace exosnap::engine
