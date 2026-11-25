#pragma once

#include <exosnap/engine/recorder_session.h>

#include <QImage>
#include <QObject>
#include <QString>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>

namespace exosnap {

// Still images for the capture targets the source picker currently has on
// screen.
//
// Load is bounded by construction rather than by a per-card timer: the service
// grabs ONE target every `kGrabIntervalMs` and walks the visible set round
// robin, so the cost stays flat at four grabs a second whether two cards or
// forty are on screen, and the perceived refresh rate scales with how much the
// user can actually see.
class CaptureTargetStillService final : public QObject {
    Q_OBJECT

  public:
    struct Request {
        QString identity;
        exosnap::engine::CaptureTarget target;
        // Invoked on the worker and on the owning Qt thread. Must be thread-safe.
        std::function<bool()> valid;
    };

    static constexpr int kGrabIntervalMs = 250;
    // Longest edge of a delivered still. The picker's thumbnail box is 84 px
    // tall inside a card of at most ~320 px, so this covers a 2x display
    // without ever reading back a full 4K surface.
    static constexpr int kMaxStillEdge = 320;

    explicit CaptureTargetStillService(QObject* parent = nullptr);
    ~CaptureTargetStillService() override;

    CaptureTargetStillService(const CaptureTargetStillService&) = delete;
    CaptureTargetStillService& operator=(const CaptureTargetStillService&) = delete;

    // Called on the owning Qt thread. stop() synchronously releases the worker and its resources.
    void start();
    void stop();

    // Called on the owning Qt thread. An empty set suspends capture and releases the D3D
    // device. Validated window identities may retain an item for the next opening.
    void setVisibleTargets(std::vector<Request> targets);

  signals:
    void stillReady(QString identity, QImage image);
    // A target that has failed twice in a row. One failure is not reported:
    // a single lost frame is normal, and acting on it makes the card flicker.
    void stillUnavailable(QString identity);

  private:
    void workerProc(std::stop_token stop_token);
    void publish(Request request, std::uint64_t revision, QImage still);
    [[nodiscard]] bool isCurrent(std::uint64_t revision) const;
    void wakeWorker();

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    winrt::handle activity_event_;
    std::vector<Request> visible_;
    std::size_t cursor_ = 0;
    std::uint64_t targets_revision_ = 0;
    std::unordered_map<QString, int> consecutive_failures_;

    std::jthread worker_;
};

} // namespace exosnap
