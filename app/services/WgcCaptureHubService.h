#pragma once

#include "services/CaptureHubCommandQueue.h"
#include "services/CaptureSourceKey.h"

#include <cstdint>
#include <functional>
#include <thread>

#include <exosnap/engine/preview_tap.h>

namespace exosnap {

// GPU-only idle preview transport for WGC monitor/window sources. It mirrors the
// DXGI hub's NT-handle + keyed-mutex contract so Qt Quick consumes every source
// through the same scene-graph bridge.
class WgcCaptureHubService {
  public:
    using HandleSink =
        std::function<void(void* nt_handle, uint32_t width, uint32_t height, exosnap::engine::PreviewTapDesc tap)>;
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
    // Only a Subscribe carries one; the other three commands are their own
    // instruction.
    struct SubscribePayload {
        CaptureSourceKey key;
        HandleSink sink;
        FramePublishedSink frame_sink;
    };

    void WorkerProc(std::stop_token stop_token);

    // Same ordered channel and same service-level lease gate as the DXGI hub:
    // the two services share one semantic contract by construction. See
    // CaptureHubCommandQueue.h and CaptureHubGate.h.
    CaptureHubCommandQueue<SubscribePayload> commands_;
    std::jthread worker_;
};

} // namespace exosnap
