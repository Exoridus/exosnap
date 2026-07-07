#pragma once

#include <QImage>
#include <recorder_core/recorder_session.h>

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
// loss-recovery policy is unit-pinned, mirroring recorder_core's
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

struct WebcamDeviceInfo {
    std::string id;
    std::string name;
};

struct WebcamFormat {
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
};

// Captures from a webcam via Media Foundation IMFSourceReader.
// Also implements WebcamFrameProvider so VideoThread can composite frames.
class WebcamService : public recorder_core::WebcamFrameProvider {
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

    // Set callback invoked on main thread with each new QImage frame.
    void SetFrameCallback(FrameCallback cb);

    // Start capture; stops any existing capture first.
    // device_id: MF symbolic link (from EnumerateDevices). Empty = first available.
    bool Start(const std::string& device_id, int width, int height, int fps);

    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;

    // WebcamFrameProvider — called by VideoThread (thread-safe).
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra) override;

  private:
    void ThreadMain(const std::string& device_id, int width, int height, int fps, std::stop_token stop);
    void StoreFrame(int width, int height, std::vector<uint8_t> bgra);
    void PostFrame(QImage img);

    FrameCallback frame_callback_;
    std::jthread thread_;
    std::atomic<bool> running_{false};

    // Latest captured frame — written by capture thread, read by VideoThread or main thread.
    mutable std::mutex frame_mutex_;
    std::vector<uint8_t> latest_bgra_;
    int frame_width_ = 0;
    int frame_height_ = 0;
    bool has_frame_ = false;
};

} // namespace exosnap
