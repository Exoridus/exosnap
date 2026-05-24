#pragma once

#include <QImage>

#include <atomic>
#include <functional>
#include <thread>

#include <recorder_core/recorder_session.h>

namespace exosnap {

class PreviewService {
  public:
    using FrameCallback = std::function<void(QImage)>;

    PreviewService() = default;
    ~PreviewService();

    PreviewService(const PreviewService&) = delete;
    PreviewService& operator=(const PreviewService&) = delete;

    void SetFrameCallback(FrameCallback cb);

    // Starts preview capture for the given target. Stops any existing thread first.
    bool Start(const recorder_core::CaptureTarget& target);

    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;

  private:
    void ThreadMain(recorder_core::CaptureTarget target, std::stop_token stop_token);
    void PostFrame(QImage frame);

    FrameCallback frame_callback_;
    std::jthread thread_;
    std::atomic<bool> running_{false};
};

} // namespace exosnap
