#pragma once

// WasapiAudioRenderer -- the Edit-page video player's audio-out path and
// playback master clock (docs/superpowers/specs/2026-07-14-edit-video-player-
// design.md). No WASAPI render path existed anywhere in this codebase before
// this class; only capture (WasapiCaptureSrc, wasapi_loopback.cpp) did.
//
// Opens the system default render endpoint (eRender/eConsole -- no in-app
// device picker, matching the design's scope decision) in shared mode.
// PushSamples() accepts 48 kHz stereo interleaved float32 PCM (the same fixed
// format EditPlayerEngine's playback decode produces) from any thread; an
// internal ring buffer plus an event-driven render callback thread write it
// to the device, resampling to the device's own mix format first if it
// differs from 48kHz/stereo/float32.
//
// FramesPlayed() (frames the endpoint has actually played out, read from
// IAudioClock) is the playback master clock -- feed it to AudioClockMs()
// (playback_clock.h). Deliberately NOT the count of frames written into the
// endpoint buffer: that buffer is 200 ms deep, so a write-counting clock runs
// ahead of the sound by the whole buffer depth and makes video lead audio.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct IMMDevice;
struct IAudioClient;
struct IAudioClock;
struct IAudioRenderClient;
// Forward-declared at GLOBAL scope deliberately (not inside namespace
// recorder_core below): the real definition lives in libswresample's
// swresample.h, which this public header must not include (keeps FFmpeg out
// of the public API surface). Writing `struct SwrContext* resampler_` INSIDE
// the recorder_core namespace block instead would declare a distinct
// recorder_core::SwrContext type via elaborated-type-specifier injection --
// a different type from the real ::SwrContext the .cpp needs to match.
// Declaring it here, before the namespace, ensures both this header and
// wasapi_audio_render.cpp refer to the same global type.
struct SwrContext;

namespace recorder_core {

// Ring capacity used unless a caller overrides it. 200 ms @ 48 kHz stereo is
// the fixed backpressure point that paces the playback AUDIO decode thread --
// see docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md.
// Kept deliberately small: it bounds how far ahead of the audio clock that
// thread can race before PushSamples() blocks it, and the same decode-ahead
// window is what EditPlayerSession's video frame queue is sized to hold
// (VideoQueueCapacityForFrameRate, playback_clock.h).
//
// Since 2026-08-01 video decodes on its own thread and is paced by that frame
// queue instead (docs/superpowers/specs/
// 2026-08-01-edit-player-decoupled-decode-design.md), so blocking here no
// longer holds video up -- and a video hitch no longer starves this ring,
// which is the whole point of that split.
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

    // Starts/stops the render callback thread. Init() must have succeeded.
    // No-op (not an error) if not initialized or already in the requested state.
    void Start();
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
    // Read from IAudioClock; falls back to the count of frames written into the
    // endpoint buffer only if that service was unavailable at Init().
    //
    // Same single-caller-thread contract as the rest of this class's control
    // API (Start/Stop/Shutdown): call it from the owner's thread -- in-app,
    // EditPlayerSession's UI thread. It is NOT internally synchronized against
    // a concurrent Stop()/Shutdown(), which releases the clock object this
    // reads through. (PushSamples() is the one deliberate exception: it is
    // callable from any thread, because the decode thread pushes into the ring.)
    [[nodiscard]] uint64_t FramesPlayed() const noexcept;

    // The render endpoint's actual sample rate (post-Init; 0 before Init()).
    [[nodiscard]] uint32_t SampleRate() const noexcept;

    // Stops the render thread, releases the COM device/client objects, and
    // clears the ring buffer. Safe to call multiple times / without Init().
    void Shutdown();

  private:
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

    // Frames written into the endpoint buffer. Only the fallback clock now --
    // see the class comment on why it is not the playback clock.
    std::atomic<uint64_t> frames_rendered_{0};

    bool initialized_ = false;
};

} // namespace recorder_core
