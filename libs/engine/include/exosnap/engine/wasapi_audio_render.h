#pragma once

// Shared-mode output through the default render endpoint (eRender/eConsole).
// PushSamples accepts 48 kHz stereo Float32 PCM from any thread through a
// bounded ring. InitTimeline instead pulls prepared PCM with a short final
// lead. Both paths convert to the endpoint's actual mix format as needed.
// Playback time uses the device clock with a normalized monotonic fallback,
// independently of buffered or submitted PCM.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct IMMDevice;
struct IAudioClient;
struct IAudioClock;
struct IAudioRenderClient;
// Forward-declared at GLOBAL scope deliberately (not inside namespace
// engine below): the real definition lives in libswresample's
// swresample.h, which this public header must not include (keeps FFmpeg out
// of the public API surface). Writing `struct SwrContext* resampler_` INSIDE
// the engine namespace block instead would declare a distinct
// exosnap::engine::SwrContext type via elaborated-type-specifier injection --
// a different type from the real ::SwrContext the .cpp needs to match.
// Declaring it here, before the namespace, ensures both this header and
// wasapi_audio_render.cpp refer to the same global type.
struct SwrContext;

namespace exosnap::engine {

class TimelineAudioClock;
enum class RenderClockMode { Stopped, Device, MonotonicFallback };
struct WasapiRenderMeasurements {
    bool initialized = false;
    bool running = false;
    bool timeline_pull = false;
    uint32_t sample_rate = 0;
    uint32_t buffer_frames = 0;
    uint32_t requested_buffer_ms = 0;
    std::optional<int64_t> device_period_ns;
    uint64_t endpoint_submissions = 0;
    uint64_t submitted_lead_frames = 0;
    uint64_t peak_submitted_lead_frames = 0;
    uint64_t clock_reanchors = 0;
    bool timeline_recovery_pending = false;
    uint64_t timeline_reanchors = 0;
    uint64_t timeline_skipped_frames = 0;
    uint64_t recovery_played_frames = 0;
    RenderClockMode clock_mode = RenderClockMode::Stopped;
};

// Ring capacity used unless a caller overrides it. 200 ms @ 48 kHz stereo is
// the fixed backpressure point that paces the playback AUDIO decode thread
// (docs/architecture/edit-and-export.md). Kept deliberately small: it bounds
// how far ahead of the audio clock that thread can race before PushSamples()
// blocks it.
//
// Video decodes on its own thread, so blocking here no
// longer holds video up -- and a video hitch no longer starves this ring,
// which is the whole point of that split. Video's own decode-ahead bound is
// the demux thread's clock-based read-ahead gate (ShouldDemuxMorePackets,
// edit_playback_pacing.h); the frame queue this comment used to point at was
// removed with the 2026-08-03 GPU render path.
inline constexpr uint32_t kDefaultRingCapacityFrames = 9600;

class WasapiAudioRenderer {
  public:
    explicit WasapiAudioRenderer(uint32_t ring_capacity_frames = kDefaultRingCapacityFrames);
    ~WasapiAudioRenderer();

    WasapiAudioRenderer(const WasapiAudioRenderer&) = delete;
    WasapiAudioRenderer& operator=(const WasapiAudioRenderer&) = delete;

    // Opens the system default render endpoint in shared mode. Returns false
    // with a message in out_error on failure (no default device, format
    // negotiation failure).
    bool Init(std::string& out_error);
    // Refuses an endpoint whose reported period cannot fit the control budget.
    bool InitTimeline(std::string& out_error);
    using TimelineAudioPull = std::function<void(uint64_t first_engine_frame, std::span<float> stereo)>;
    // The pull must service prepared memory only. Stop joins before releasing it.
    void StartTimeline(TimelineAudioPull pull);
    [[nodiscard]] WasapiRenderMeasurements Measurements() const;

    // Starts/stops the render callback thread. Init() must have succeeded.
    // No-op (not an error) if not initialized or already in the requested state.
    void Start(std::span<const float> initial_samples = {}, bool timeline = false);
    // Timeline writes are nonblocking and discard samples already replaced by
    // endpoint silence. The sample origin is the most recent Start().
    void PushTimelineSamples(uint64_t first_frame, std::span<const float> samples);
    [[nodiscard]] uint64_t TimelineConsumedFrames();
    [[nodiscard]] bool Running() const noexcept {
        return running_.load();
    }
    void Stop();

    // Appends `frame_count` stereo frames (frame_count * 2 floats,
    // interleaved L,R) to the internal ring buffer, blocking the caller
    // until enough room is available (this is the playback pacing point --
    // see the class doc comment above). A blocked call is woken early by
    // Stop(), which drops whatever had not yet been inserted rather than
    // writing it. Safe to call from any thread. No-op if frame_count is 0.
    void PushSamples(const float* interleaved_stereo, uint32_t frame_count);

    // Frames the render endpoint has actually played since the current Start()
    // -- the playback master clock. 0 before Init()/Start(), and reset by every
    // Start() (it measures time-since-resume, not absolute clip position).
    // Uses IAudioClock when available and a monotonic estimate on loss/stall,
    // reanchoring when the device clock recovers. Never counts queued PCM as played.
    //
    // Same single-caller-thread contract as the rest of this class's control
    // API (Start/Stop/Shutdown): call it from the owner's thread. It is not synchronized against
    // a concurrent Stop()/Shutdown(), which releases the clock object this
    // reads through. (PushSamples() is the one deliberate exception: it is
    // callable from any thread, because the decode thread pushes into the ring.)
    [[nodiscard]] uint64_t FramesPlayed() const noexcept;

    // The render endpoint's actual sample rate (post-Init; 0 before Init()).
    [[nodiscard]] uint32_t SampleRate() const noexcept;

    // 48 kHz frames the render thread had to replace with silence because the
    // ring ran short, counted from the first audio of the current Start(). Each is
    // an audible gap; any steady growth is an underrun the listener hears as
    // crackle. Safe from any thread.
    [[nodiscard]] uint64_t ShortfallFrames() const noexcept {
        return shortfall_frames_.load(std::memory_order_relaxed);
    }

    // Stops the render thread, releases the COM device/client objects, and
    // clears the ring buffer. Safe to call multiple times / without Init().
    void Shutdown();

  private:
    bool InitInternal(std::string& out_error, uint32_t buffer_ms);
    void RenderThreadMain();

    // Live play-cursor read (only meaningful while the stream is running).
    [[nodiscard]] uint64_t ReadPlayedFramesLive() const noexcept;

    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    IAudioClock* audio_clock_ = nullptr; // play cursor; null == fall back to frames_rendered_
    void* buffer_event_ = nullptr;       // HANDLE, opaque here to keep windows.h out of this header
    SwrContext* resampler_ = nullptr;    // opaque (::SwrContext, forward-declared above), only used if
                                         // the device format != 48k/stereo/float32

    uint32_t device_sample_rate_ = 0;
    uint32_t device_channels_ = 0;
    uint32_t device_bytes_per_sample_ = 4; // bytes/sample in device_data buffers (float32 default, 2 if int16)
    uint32_t buffer_frame_count_ = 0;

    std::thread render_thread_;
    std::atomic<bool> running_{false};

    std::mutex ring_mutex_;
    std::deque<float> ring_;          // interleaved stereo float32 @ 48kHz, pre-device-resample
    std::condition_variable ring_cv_; // paired with ring_mutex_: signaled when ring space frees up
                                      // or a Stop() is in progress
    bool stop_requested_ = false;     // guarded by ring_mutex_; wakes+drops any blocked PushSamples
    uint32_t ring_capacity_floats_;   // ring_ capacity in interleaved floats (frames * channels)
    bool timeline_mode_ = false;
    uint64_t timeline_consumed_frames_ = 0; // guarded by ring_mutex_, includes underrun silence

    // Units of the IAudioClock stream position per second (IAudioClock::
    // GetFrequency). 0 == no usable clock service.
    uint64_t clock_frequency_ = 0;
    // Stream position captured at the last Start(). IAudioClient::Reset()
    // normally zeroes the position, but a Reset that fails (or is skipped
    // because a buffer was still outstanding) would otherwise make the clock
    // resume mid-clip; subtracting the baseline keeps "0 at every Start()"
    // true either way.
    std::atomic<uint64_t> clock_baseline_{0};
    // Play position frozen at the last Stop(). Stop() flushes the endpoint, so
    // the live cursor rewinds to 0 there; the Edit page's position readout must
    // hold at where playback was paused instead of snapping back to the start.
    std::atomic<uint64_t> frames_played_latched_{0};

    // Submitted device frames, including silence. This is not the playback clock.
    std::atomic<uint64_t> frames_rendered_{0};
    std::atomic<uint64_t> shortfall_frames_{0};
    bool audio_arrived_ = false; // render thread only; reset by Start()

    bool initialized_ = false;
    bool low_latency_timeline_ = false;
    uint32_t requested_buffer_ms_ = 0;
    std::optional<int64_t> device_period_ns_;
    TimelineAudioPull timeline_pull_;
    std::atomic<uint64_t> pull_consumed_frames_{0};
    mutable std::atomic<uint64_t> normalized_played_frames_{0};
    std::atomic<bool> timeline_recovery_pending_{false};
    std::atomic<uint64_t> timeline_reanchors_{0};
    std::atomic<uint64_t> timeline_skipped_frames_{0};
    std::atomic<uint64_t> recovery_played_frames_{0};
    std::atomic<uint64_t> endpoint_submissions_{0};
    std::atomic<uint64_t> submitted_lead_frames_{0};
    std::atomic<uint64_t> peak_submitted_lead_frames_{0};
    std::unique_ptr<TimelineAudioClock> normalized_clock_;
    std::vector<float> engine_buffer_;
};

} // namespace exosnap::engine
