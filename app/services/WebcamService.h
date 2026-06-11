#pragma once

#include <QImage>
#include <recorder_core/recorder_session.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace exosnap {

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

    // Enumerate available webcam devices (main thread only, one-shot).
    static std::vector<WebcamDeviceInfo> EnumerateDevices();

    // Enumerate supported formats for a given device id.
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
