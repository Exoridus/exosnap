#pragma once

#include "services/CaptureSourceKey.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <recorder_core/preview_tap.h>

namespace exosnap {

// GPU-only idle preview transport for WGC monitor/window sources. It mirrors the
// DXGI hub's NT-handle + keyed-mutex contract so Qt Quick consumes every source
// through the same scene-graph bridge.
class WgcCaptureHubService {
  public:
    using HandleSink =
        std::function<void(void* nt_handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap)>;
    // Per-frame publish edge, mirroring DxgiCaptureHubService::FramePublishedSink
    // (see there for why the transport needs one). Pump thread; no payload.
    using FramePublishedSink = std::function<void()>;

    WgcCaptureHubService();
    ~WgcCaptureHubService();

    WgcCaptureHubService(const WgcCaptureHubService&) = delete;
    WgcCaptureHubService& operator=(const WgcCaptureHubService&) = delete;

    bool Subscribe(CaptureSourceKey key, HandleSink sink, FramePublishedSink frame_sink);
    void Unsubscribe();
    void RequestEngineLease();
    void ReturnEngineLease();

  private:
    struct Command {
        enum class Op { Subscribe, Unsubscribe, LeaseRequest, LeaseReturn };
        Op op = Op::Unsubscribe;
        CaptureSourceKey key;
        HandleSink sink;
        FramePublishedSink frame_sink;
        uint64_t serial = 0;
    };

    void WorkerProc(std::stop_token stop_token);
    uint64_t PostCommand(Command command);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable ack_cv_;
    std::optional<Command> pending_;
    uint64_t next_serial_ = 1;
    uint64_t processed_serial_ = 0;
    std::jthread worker_;
};

} // namespace exosnap
