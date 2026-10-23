#pragma once

#include <exosnap/engine/edit_timeline_render.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace exosnap::engine {

struct TimelineAudioStamp {
    uint64_t session = 0;
    uint64_t revision = 0;
    uint64_t stream_generation = 0;
};

struct TimelineAudioRegion {
    int64_t in_sample = 0;
    int64_t out_sample = 0;
    bool repeat = false;
};

struct TimelineAudioPosition {
    int64_t project_sample = 0;
    uint64_t loop_iteration = 0;
    bool ended = false;
};

struct TimelineAudioRecovery {
    uint64_t next_frame = 0;
    uint64_t skipped_frames = 0;
};

// All positions are engine-rate frames. Startup has no prior output to recover.
[[nodiscard]] inline std::optional<TimelineAudioRecovery> PlanTimelineAudioRecovery(uint64_t pulled_frames,
                                                                                    uint64_t played_frames,
                                                                                    uint64_t tolerance_frames,
                                                                                    bool has_output) noexcept {
    if (!has_output || played_frames <= pulled_frames || played_frames - pulled_frames <= tolerance_frames)
        return std::nullopt;
    return TimelineAudioRecovery{played_frames, played_frames - pulled_frames};
}

struct TimelineAudioControl {
    uint64_t clip = 0;
    bool audible = true;
    AudioIntervalContribution evaluation;
};

struct TimelineAudioControlInterval {
    int64_t first_sample = 0;
    int64_t end_sample = 0;
    std::vector<TimelineAudioControl> contributions;
};

struct TimelineAudioControls {
    TimelineAudioStamp stamp;
    uint64_t control_revision = 0;
    float monitor_gain = 1;
    std::vector<TimelineAudioControlInterval> intervals;
};

[[nodiscard]] Outcome<std::shared_ptr<const TimelineAudioControls>>
BuildTimelineAudioControls(const TimelineSnapshot& timeline, const TimelineAudioStamp& stamp, float monitor_gain = 1,
                           std::span<const uint64_t> solo_tracks = {}, uint64_t control_revision = 0);

struct TimelineAudioMeasurements {
    uint64_t requested_revision = 0;
    uint64_t applied_revision = 0;
    uint64_t requested_control_revision = 0;
    uint64_t applied_control_revision = 0;
    uint64_t stream_generation = 0;
    uint64_t ready_generation = 0;
    uint64_t underrun_frames = 0;
    uint64_t source_buffer_frames = 0;
    uint64_t source_capacity_frames = 0;
    uint64_t source_bytes = 0;
    uint64_t loop_head_frames = 0;
    uint64_t loop_head_bytes = 0;
    uint64_t loop_head_reuses = 0;
    uint64_t peak_decoders = 0;
    uint64_t stale_blocks = 0;
    uint64_t lookahead_deferred = 0;
    TimelineAudioReadMeasurements source_reads;
    uint64_t retired_control_tables = 0;
    uint64_t peak_retired_control_tables = 0;
    uint64_t rejected_controls = 0;
    bool endpoint_publication_lock_free = false;
    uint64_t publication_misses = 0;
    bool failed = false;
};

// The producer exclusively owns audio decoders. Mix has one endpoint caller and
// never performs I/O or waits for producer work. Stop endpoint calls before destruction.
class TimelineAudioProducer {
  public:
    using Admission = std::function<bool(const std::atomic<bool>& cancelled)>;

    TimelineAudioProducer();
    ~TimelineAudioProducer();
    TimelineAudioProducer(const TimelineAudioProducer&) = delete;
    TimelineAudioProducer& operator=(const TimelineAudioProducer&) = delete;

    void Prepare(std::shared_ptr<const TimelineRenderSnapshot> snapshot, TimelineAudioStamp stamp, int64_t first_sample,
                 Admission before_read = {}, std::optional<TimelineAudioRegion> region = std::nullopt);
    // Returns false if four retired tables remain in use. Retry after old readers release them.
    bool SetControls(std::shared_ptr<const TimelineAudioControls> controls);
    // Single-owner preparation only, before the stream's first Mix. Never performs I/O.
    // Skips obsolete prepared samples and returns whether the requested span is ready.
    bool AdvancePreparation(uint64_t stream_generation, int64_t first_sample, uint32_t required_frames = 480);
    // Output positions start at Prepare's first_sample and increase across loop wraps.
    // Returns silence for stale/canceled state or eight consecutive publication conflicts.
    void Mix(uint64_t stream_generation, int64_t first_sample, std::span<float> interleaved_stereo) noexcept;
    // Revokes output immediately. Producer teardown stays on its own execution context.
    void Cancel();
    [[nodiscard]] TimelineAudioMeasurements Measurements() const;
    [[nodiscard]] std::optional<TimelineAudioPosition> PositionAt(uint64_t stream_generation,
                                                                  int64_t output_sample) const;
    [[nodiscard]] std::string Error() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TimelineAudioClock {
  public:
    void Reset(uint32_t rate, int64_t now_ns) noexcept {
        rate_ = rate;
        output_ = 0;
        last_ns_ = now_ns;
        changed_ns_ = now_ns;
        changed_output_ = 0;
        device_anchor_ = 0;
        output_anchor_ = 0;
        last_device_ = 0;
        fallback_ns_ = now_ns;
        fallback_output_ = 0;
        device_seen_ = false;
        fallback_ = false;
        reanchors_ = 0;
    }
    uint64_t Observe(std::optional<uint64_t> device_frames, int64_t now_ns) noexcept {
        now_ns = (std::max)(now_ns, last_ns_);
        const auto advance = [&](int64_t anchor_ns, uint64_t anchor_frame) {
            const auto elapsed = static_cast<uint64_t>(now_ns - anchor_ns);
            const auto whole = elapsed / 1'000'000'000;
            const auto remainder = elapsed % 1'000'000'000;
            const auto maximum = (std::numeric_limits<uint64_t>::max)();
            if (rate_ && whole > (maximum - anchor_frame) / rate_)
                return maximum;
            const auto base = anchor_frame + whole * rate_;
            const auto tail = remainder * rate_ / 1'000'000'000;
            return tail > maximum - base ? maximum : base + tail;
        };
        if (!device_frames) {
            if (!fallback_) {
                fallback_ns_ = last_ns_;
                fallback_output_ = output_;
                fallback_ = true;
            }
            output_ = (std::max)(output_, advance(fallback_ns_, fallback_output_));
        } else if (fallback_ && device_seen_ && *device_frames == last_device_) {
            output_ = (std::max)(output_, advance(fallback_ns_, fallback_output_));
        } else if (fallback_ || (device_seen_ && *device_frames < last_device_)) {
            output_ =
                (std::max)(output_, fallback_ ? advance(fallback_ns_, fallback_output_) : advance(last_ns_, output_));
            device_anchor_ = *device_frames;
            output_anchor_ = output_;
            last_device_ = *device_frames;
            changed_ns_ = now_ns;
            changed_output_ = output_;
            fallback_ = false;
            device_seen_ = true;
            ++reanchors_;
        } else if (!device_seen_) {
            device_seen_ = true;
            last_device_ = *device_frames;
            output_ = (std::max)(output_, *device_frames);
            changed_ns_ = now_ns;
            changed_output_ = output_;
        } else if (*device_frames == last_device_ && now_ns - changed_ns_ > 100'000'000) {
            fallback_ns_ = changed_ns_;
            fallback_output_ = changed_output_;
            fallback_ = true;
            output_ = (std::max)(output_, advance(fallback_ns_, fallback_output_));
        } else {
            output_ = (std::max)(output_, output_anchor_ + (*device_frames - device_anchor_));
            if (*device_frames != last_device_) {
                changed_ns_ = now_ns;
                changed_output_ = output_;
            }
            last_device_ = *device_frames;
        }
        last_ns_ = now_ns;
        return output_;
    }
    uint64_t Reanchors() const noexcept {
        return reanchors_;
    }
    bool UsingFallback() const noexcept {
        return fallback_;
    }

  private:
    uint32_t rate_ = 0;
    uint64_t output_ = 0;
    int64_t last_ns_ = 0;
    int64_t changed_ns_ = 0;
    uint64_t changed_output_ = 0;
    uint64_t device_anchor_ = 0;
    uint64_t output_anchor_ = 0;
    uint64_t last_device_ = 0;
    int64_t fallback_ns_ = 0;
    uint64_t fallback_output_ = 0;
    bool device_seen_ = false;
    bool fallback_ = false;
    uint64_t reanchors_ = 0;
};

} // namespace exosnap::engine
