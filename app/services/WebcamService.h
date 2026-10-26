#pragma once
#include "models/WebcamControls.h"
#include "models/WebcamFormatPolicy.h"
#include "models/WebcamFrameMailbox.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <exosnap/engine/recorder_session.h>

#include <windows.h> // HRESULT / DWORD for ClassifyWebcamReadResult

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace exosnap {

// ---------------------------------------------------------------------------
// Webcam read-result classification (loss detection) policy
// ---------------------------------------------------------------------------
// How the capture loop must react to one IMFSourceReader::ReadSample() result.
// Pure and MF-call-free (only inspects the returned HRESULT + reader flags) so the
// loss-recovery policy is unit-pinned, mirroring the engine's
// ClassifyOdAcquireFailure. Any result that means the reader is dead maps to
// Reconnect: the capture thread tears the reader down and polls to reopen the
// device, while TryGetFrame keeps serving the last captured frame (frozen).
enum class WebcamReadAction {
    Deliver,   // A valid sample was produced: store it and composite.
    Skip,      // No sample this read but the stream is healthy (streaming tick /
               // spurious wake): keep the last frame and read again.
    Reconnect, // Reader failure / device error / device removed / end-of-stream:
               // the reader is dead. Reopen the device, holding the last frame.
};

// Classify a ReadSample() result. has_sample is (sample != nullptr). A failed
// HRESULT, an MF_SOURCE_READERF_ERROR, or MF_SOURCE_READERF_ENDOFSTREAM all mean
// the reader is gone (Reconnect); a healthy read with no sample is Skip; a healthy
// read with a sample is Deliver.
WebcamReadAction ClassifyWebcamReadResult(HRESULT hr, DWORD reader_flags, bool has_sample) noexcept;

// Returns true if a freshly-read sample timestamp should be delivered: only
// strictly-newer samples pass, so stale frames replayed by a reopened MF reader
// (the "snap back several frames" glitch) are dropped. last_delivered_100ns is
// the last delivered sample time (LONGLONG, MF 100ns units); a first frame
// (last < 0) always passes; a non-positive/absent sample time (sample_100ns <= 0)
// passes (no basis to reject).
bool ShouldDeliverWebcamSample(long long last_delivered_100ns, long long sample_100ns) noexcept;

// Waits up to `delay` between reopen attempts. Returns false as soon as a stop
// is requested, so Stop() never waits out a reconnect delay.
bool WaitForWebcamReconnect(std::stop_token stop, std::chrono::milliseconds delay);

struct WebcamDeviceInfo {
    std::string id;
    std::string name;
};

// True when the webcam setup preview should open the camera: only when the webcam
// is enabled AND a device is available. Couples the live preview to the enable
// state so the camera never springs on merely from opening the Webcam page.
bool ShouldOpenWebcamPreview(bool webcam_enabled, bool has_device) noexcept;

// True when the Record dock's webcam toggle accepts a click. With no camera
// attached there is nothing to turn on, so the control reads as unavailable
// rather than failing after the fact. A locked transport (failed or blocked
// session) disables it regardless.
[[nodiscard]] bool ShouldEnableWebcamToggle(bool has_device, bool transport_locked) noexcept;

// Maps a raw webcam open-failure reason (as produced by the reader-open path — a
// short step tag plus HRESULT / pixel-format diagnostics) to a short, user-facing
// sentence for the dock tooltip. Returns the raw string UNCHANGED when there is no
// known mapping, so the caller can append the "(<raw>)" technical suffix only for
// recognised reasons. Pure string logic (no MF calls), so it is unit-pinned.
[[nodiscard]] QString FriendlyWebcamOpenFailure(const QString& raw_reason);

// Chooses the webcam device id to select given the currently-configured id and
// the available devices. An explicit choice (non-empty id) is always kept — even
// if that device is momentarily absent — so it reconnects when plugged back in.
// When nothing is chosen (empty id) and at least one camera exists, the first
// device is returned so a fresh setup pre-selects a camera instead of leaving an
// empty selection (which used to make capture silently grab the first device).
std::string ResolveWebcamDeviceId(const std::string& configured_id, const std::vector<WebcamDeviceInfo>& devices);

// The pixel encodings a camera can deliver, in the order the reader prefers
// them once size and rate are equal. NV12 and YUY2 reach the GPU untouched and
// are converted there; anything else (MJPEG, ...) needs a CPU decoder first.
enum class WebcamNativeEncoding : uint8_t {
    Nv12,
    Yuy2,
    Other,
};

// One native media type Media Foundation enumerated for a device (as returned
// by IMFSourceReader::GetNativeMediaType), reduced to the fields the selection
// policy below needs. Same shape as WebcamFormat; kept as a separate type so
// this header has no MF dependency for callers that only need the pure policy.
struct WebcamNativeFormat {
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
    WebcamNativeEncoding encoding = WebcamNativeEncoding::Other;
};

// Selects which entry of `formats` (a device's native types, in MF enumeration
// order) the reader should negotiate for a request of (want_w, want_h,
// want_fps). Requires an exact width/height match -- MF native types are not
// scaled. Among the width/height matches, the entry whose frame rate is
// closest to want_fps wins: an exact match if the camera offers one at that
// resolution, otherwise the nearest available rate (a camera's native type
// list is a fixed discrete set -- e.g. a UVC device offering 1080p only at
// 30/60 fps cannot produce 1080p50). Equally close rates prefer the encoding
// that needs no CPU decode (NV12, then YUY2), then the first (lowest-index)
// match, mirroring MF's own enumeration order. Size and rate always outrank
// the encoding: a camera that reaches the requested mode only as MJPEG gets
// MJPEG. want_fps <= 0 means "no frame-rate preference". Returns -1 when no
// entry matches (want_w, want_h) at all. Pure/MF-call-free so the selection
// policy is unit-pinned, like ClassifyWebcamReadResult above.
[[nodiscard]] int SelectBestWebcamNativeFormat(const std::vector<WebcamNativeFormat>& formats, int want_w, int want_h,
                                               int want_fps) noexcept;

// Same width/height match + fps-closeness + encoding policy as
// SelectBestWebcamNativeFormat, but returns every matching index in preference
// order instead of only the single best one. Used to retry SetCurrentMediaType
// against progressively-less-ideal candidates: the closest-fps native type can
// fail to negotiate for a reason unrelated to frame rate (e.g. a pixel format
// this particular native type advertises that the reader can't convert), even
// though a same-resolution neighbor a little further from the requested fps
// would succeed. SelectBestWebcamNativeFormat(formats, w, h, fps) is exactly
// RankWebcamNativeFormats(formats, w, h, fps).front() (or -1 if empty).
[[nodiscard]] std::vector<int> RankWebcamNativeFormats(const std::vector<WebcamNativeFormat>& formats, int want_w,
                                                       int want_h, int want_fps) noexcept;

// Repacks one locked camera buffer into the tightly packed layout
// WebcamFrameSnapshot documents. `pitch` is the byte distance between luma (or
// packed) rows as IMF2DBuffer::Lock2D reports it; a negative pitch is a
// bottom-up image and is only legal for BGRA. NV12's chroma plane starts
// `pitch * height` bytes after `scan0`, as in a system-memory 2D buffer.
// Returns false, leaving `out` unspecified, when the layout or the pitch cannot
// describe a frame of this size.
[[nodiscard]] bool PackWebcamFrame(exosnap::engine::WebcamPixelFormat format, int width, int height,
                                   const uint8_t* scan0, long pitch, std::vector<uint8_t>& out);

// What one camera offers: its native capture modes and the UVC controls it
// reports, with their ranges and the state they were in when probed.
struct WebcamDeviceProbe {
    std::vector<WebcamFormat> formats;
    std::vector<WebcamControlRange> controls;
};

// Captures from a webcam via Media Foundation IMFSourceReader.
// Also implements WebcamFrameProvider so VideoThread can composite frames.
class WebcamService : public exosnap::engine::WebcamFrameProvider {
  public:
    // Announces that Snapshot() now serves this generation. Carries no pixels:
    // consumers that draw the camera pull the snapshot themselves.
    using FrameCallback = std::function<void(uint64_t generation)>;

    WebcamService() = default;
    ~WebcamService() override;

    WebcamService(const WebcamService&) = delete;
    WebcamService& operator=(const WebcamService&) = delete;

    // Returns true if Media Foundation (mfplat.dll) is present on this system.
    // Safe to call at any time; uses LoadLibraryW and caches the result.
    // When false, all other methods return safe empty results / false without
    // touching any MF entry point.
    static bool IsMfPresent() noexcept;

    // Enumerate available webcam devices (main thread only, one-shot).
    // Returns empty when IsMfPresent() is false.
    static std::vector<WebcamDeviceInfo> EnumerateDevices();

    // Enumerate supported formats for a given device id. Opens the camera
    // briefly, so call it off the GUI thread; any thread works.
    // Returns empty when IsMfPresent() is false.
    static std::vector<WebcamFormat> EnumerateFormats(const std::string& device_id);
    // Formats and UVC controls in one opening of the device. Same threading as
    // EnumerateFormats.
    static WebcamDeviceProbe ProbeDevice(const std::string& device_id);

    // The UVC controls to hold the camera at. Any thread. Applied when the
    // device opens and, while it runs, before the next frame is read; a
    // control dropped from the list returns to the camera's default.
    void SetControls(std::vector<WebcamControlSetting> controls);

    // Set callback invoked on the main thread after each new frame.
    // Legacy registration: delivery is bound to the application's lifetime, so the
    // callback must own no receiver it cannot outlive (a consumer should capture a
    // QPointer guard only).
    //
    // Callable from any thread, at any time, including while the capture thread is
    // delivering: the registration is an immutable snapshot published under
    // delivery_mutex_, so replacing or clearing it never destroys a closure a
    // worker is holding (see PostFrame).
    void SetFrameCallback(FrameCallback cb);

    // Preferred registration: binds queued frame delivery to `receiver`'s
    // lifetime. The receiver is validated on the delivery thread, so the callback
    // is never invoked against a destroyed receiver. Same any-thread contract as
    // the overload above.
    void SetFrameCallback(QObject* receiver, FrameCallback cb);

    using StatusCallback = std::function<void(bool ok, QString reason)>;
    // Fires on open-reader transitions only (a failure streak begins → ok=false
    // with the reason; recovery to a successful open → ok=true, reason empty). The
    // first successful open of a run also fires ok=true. It is delivered on the
    // main thread and dropped if the receiver died — same delivery contract as
    // SetFrameCallback(receiver, cb). Stop() resets the streak state without
    // firing.
    void SetStatusCallback(QObject* receiver, StatusCallback cb);

    // Start capture; stops any existing capture first.
    // device_id: MF symbolic link (from EnumerateDevices). Empty opens no device
    // (see ResolveWebcamDeviceId — callers must resolve a concrete id first).
    bool Start(const std::string& device_id, int width, int height, int fps);

    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;

    // WebcamFrameProvider — called by VideoThread (thread-safe).
    std::shared_ptr<const exosnap::engine::WebcamFrameSnapshot> Snapshot() override;
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override;

  protected:
    // Marshals the frame announcement to the callback on the main thread.
    // Protected (not private) so the delivery-lifetime contract can be exercised
    // directly by a unit test without a live capture device.
    void PostFrame(uint64_t generation);

    // Marshals an open-reader status transition to the status callback. Same
    // snapshot-and-post delivery as PostFrame.
    void PostStatus(bool ok, QString reason);

  private:
    void ThreadMain(const std::string& device_id, int width, int height, int fps, std::stop_token stop);
    // Returns the generation the frame was published under.
    uint64_t StoreFrame(const WebcamFrameLayout& layout, const uint8_t* data, size_t size);
    // Body of Stop(); caller must hold control_mutex_.
    void StopLocked();

    // One registration, immutable once published. The capture thread copies the
    // shared_ptr under delivery_mutex_ and touches nothing else: it never reads
    // the QPointer (QPointer is a GUI-thread guard, not a synchronization
    // primitive) and it cannot have a closure destroyed underneath it, because a
    // replacement installs a *new* snapshot rather than mutating this one. The
    // in-flight post keeps its own snapshot alive until it has run.
    //
    // `receiver` is read only on the delivery thread, inside the posted lambda.
    // When receiver_bound is true a null receiver means the receiver has been
    // destroyed and the event is dropped.
    struct FrameDelivery {
        FrameCallback callback;
        QPointer<QObject> receiver;
        bool receiver_bound = false;
    };
    // Status delivery is always receiver-scoped (no legacy unbound path).
    struct StatusDelivery {
        StatusCallback callback;
        QPointer<QObject> receiver;
    };

    mutable std::mutex delivery_mutex_;
    std::shared_ptr<const FrameDelivery> frame_delivery_;   // guarded by delivery_mutex_
    std::shared_ptr<const StatusDelivery> status_delivery_; // guarded by delivery_mutex_

    // Start()/Stop() are called from more than one thread (RecordingCoordinator's
    // UI-thread SyncWebcamService() and its background recording_thread_ both call
    // in directly around a recording's start/finish) with no other synchronization
    // between them once is_recording_ has settled. Guards thread_ against
    // concurrent Start()/Stop() calls racing each other's request_stop()/join()/
    // reassignment, which is undefined behavior on a plain std::jthread member.
    std::mutex control_mutex_;
    std::jthread thread_;
    std::atomic<bool> running_{false};

    // Latest captured frame — written by capture thread, read by VideoThread or main thread.
    std::mutex controls_mutex_;
    std::vector<WebcamControlSetting> controls_; // guarded by controls_mutex_
    std::atomic<uint64_t> controls_generation_{1};

    mutable std::mutex frame_mutex_;
    WebcamFrameMailbox frame_mailbox_;
    uint64_t frame_generation_ = 0;
};

} // namespace exosnap
