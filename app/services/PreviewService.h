#pragma once

#include <QImage>
#include <QObject>
#include <QPointer>

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

    // Legacy registration: frames are delivered on the main thread but bound to
    // the application's lifetime, so the callback must own no receiver it cannot
    // outlive (see WebcamPage, which captures a QPointer guard only).
    void SetFrameCallback(FrameCallback cb);

    // Preferred registration: binds queued frame delivery to `receiver`'s
    // lifetime. Because PostFrame enqueues onto `receiver`, Qt drops any in-flight
    // frame the moment the receiver is destroyed, so the callback is never invoked
    // against a freed receiver. This is the fix for the latent use-after-free where
    // a frame posted just before the receiving page died was still delivered.
    void SetFrameCallback(QObject* receiver, FrameCallback cb);

    // Starts preview capture for the given target. Stops any existing thread first.
    bool Start(const recorder_core::CaptureTarget& target);

    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept;

  protected:
    // Marshals `frame` to the callback on the main thread. Protected (not private)
    // so the delivery-lifetime contract can be exercised directly by a unit test
    // without a live capture device.
    void PostFrame(QImage frame);

  private:
    void ThreadMain(recorder_core::CaptureTarget target, std::stop_token stop_token);

    FrameCallback frame_callback_;
    // When receiver_bound_ is true, delivery is scoped to receiver_: a null
    // receiver_ means the receiver has been destroyed and the frame is dropped.
    QPointer<QObject> receiver_;
    bool receiver_bound_ = false;
    std::jthread thread_;
    std::atomic<bool> running_{false};
};

} // namespace exosnap
