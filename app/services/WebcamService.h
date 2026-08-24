#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <exosnap/engine/recorder_session.h>

#include <windows.h> // HRESULT / DWORD for ClassifyWebcamReadResult

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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

struct WebcamFormat {
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
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
};

// Selects which entry of `formats` (a device's native types, in MF enumeration
// order) the reader should negotiate for a request of (want_w, want_h,
// want_fps). Requires an exact width/height match -- MF native types are not
// scaled. Among the width/height matches, the entry whose frame rate is
// closest to want_fps wins: an exact match if the camera offers one at that
// resolution, otherwise the nearest available rate (a camera's native type
// list is a fixed discrete set -- e.g. a UVC device offering 1080p only at
// 30/60 fps cannot produce 1080p50). Ties keep the first (lowest-index) match,
// mirroring MF's own enumeration order. want_fps <= 0 means "no frame-rate
// preference": the first width/height match wins outright, same as when the
// caller does not care about frame rate. Returns -1 when no entry matches
// (want_w, want_h) at all. Pure/MF-call-free so the selection policy is
// unit-pinned, like ClassifyWebcamReadResult above.
[[nodiscard]] int SelectBestWebcamNativeFormat(const std::vector<WebcamNativeFormat>& formats, int want_w, int want_h,
                                               int want_fps) noexcept;

// Same width/height match + fps-closeness policy as SelectBestWebcamNativeFormat,
// but returns every matching index ordered from closest to farthest from
// want_fps (ties keep the earlier/lower index), instead of only the single
// best one. want_fps <= 0 returns every width/height match in original
// enumeration order (no fps preference). Used to retry SetCurrentMediaType
// against progressively-less-ideal candidates: the closest-fps native type can
// fail to negotiate for a reason unrelated to frame rate (e.g. a pixel format
// this particular native type advertises that the reader can't convert), even
// though a same-resolution neighbor a little further from the requested fps
// would succeed. SelectBestWebcamNativeFormat(formats, w, h, fps) is exactly
// RankWebcamNativeFormats(formats, w, h, fps).front() (or -1 if empty).
[[nodiscard]] std::vector<int> RankWebcamNativeFormats(const std::vector<WebcamNativeFormat>& formats, int want_w,
                                                       int want_h, int want_fps) noexcept;

// Rounds a rational frame rate (fps_num/fps_den) to the nearest whole fps --
// never truncates. Common NTSC rates are the reason this matters: 30000/1001
// (~29.97 fps) must round to 30, not truncate to 29 (a plain fps_num/fps_den
// integer division). fps_den <= 0 is treated as 1. Shared by every place that
// turns a native MF frame rate into the plain-int fps WebcamFormat/
// WebcamSettings/the negotiated-fps report use, so the resolution combo's
// label, the stored setting, and the log field always agree with each other.
[[nodiscard]] int RoundWebcamFps(int fps_num, int fps_den) noexcept;

// Captures from a webcam via Media Foundation IMFSourceReader.
// Also implements WebcamFrameProvider so VideoThread can composite frames.
class WebcamService : public exosnap::engine::WebcamFrameProvider {
  public:
    using FrameCallback = std::function<void(QImage)>;

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

    // Enumerate supported formats for a given device id.
    // Returns empty when IsMfPresent() is false.
    static std::vector<WebcamFormat> EnumerateFormats(const std::string& device_id);

    // Set callback invoked on the main thread with each new QImage frame.
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
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override;

  protected:
    // Marshals `img` to the callback on the main thread. Protected (not private)
    // so the delivery-lifetime contract can be exercised directly by a unit test
    // without a live capture device.
    void PostFrame(QImage img);

    // Marshals an open-reader status transition to the status callback. Same
    // snapshot-and-post delivery as PostFrame.
    void PostStatus(bool ok, QString reason);

  private:
    void ThreadMain(const std::string& device_id, int width, int height, int fps, std::stop_token stop);
    void StoreFrame(int width, int height, std::vector<uint8_t> bgra);
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
    mutable std::mutex frame_mutex_;
    std::vector<uint8_t> latest_bgra_;
    int frame_width_ = 0;
    int frame_height_ = 0;
    bool has_frame_ = false;
    uint64_t frame_generation_ = 0;
};

} // namespace exosnap
