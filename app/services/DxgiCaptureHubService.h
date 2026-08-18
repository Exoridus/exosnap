#pragma once

// The DXGI capture hub: one Output Duplication, owned by a hub, feeding the
// Record page's idle display preview over the same NT-handle + keyed-mutex
// transport the engine's WYSIWYG tap uses. The preview renderer consumes it in
// pushed-only mode and never opens a capture of its own.
//
// Why a hub at all: an output can be duplicated once per process, and only the
// owner of that one capture can tell "the desktop is quiet" apart from "the
// source is gone" — and therefore hold the last good frame through a hot-plug
// instead of blanking (capture_hub_policy.h). Strictly refcounted: at most one
// duplication ever (the subscribed monitor), none while nothing is subscribed.
//
// Threading: Subscribe/Unsubscribe are called on the UI thread and only post a
// command; RequestEngineLease/ReturnEngineLease are posted from the recording
// coordinator's prepare/record worker thread. Those two producers race, which
// is why the channel is an ordered queue and not a slot (CaptureHubCommandQueue.h)
// and why the lease lives in a service-level state machine (CaptureHubGate.h).
// The registry, the hub, the producer and the publisher all live on this
// service's own pump thread (~60 Hz — a live preview, not a thumbnail
// cadence). The handle sink is invoked ON THE PUMP THREAD; the subscriber
// marshals to the UI thread itself (the same contract as the engine's
// PreviewSharedHandleReadyCallback).

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>

#include <recorder_core/preview_tap.h>

#include "services/CaptureHubCommandQueue.h"

namespace exosnap {

class DxgiCaptureHubService {
  public:
    struct PreviewPublishStats {
        uint64_t attempts = 0;
        uint64_t published = 0;
        uint64_t dropped_on_contention = 0;
    };
    // Fired on the pump thread whenever the shared texture is (re)created: once
    // after the first frame, and again if the desktop changes size or format.
    // Ownership of the NT handle transfers to the sink (open, then CloseHandle).
    using HandleSink =
        std::function<void(void* nt_handle, uint32_t width, uint32_t height, recorder_core::PreviewTapDesc tap)>;
    // Fired on the pump thread after every frame that actually reached the
    // shared texture — the "there is something new to take" edge the keyed-mutex
    // transport itself cannot express. A consumer without it can only poll, and
    // a poll-driven redraw re-renders on a quiet desktop forever. Carries no
    // payload: the shared texture always holds the newest frame. Same threading
    // contract as HandleSink; must return fast and must not touch D3D here.
    using FramePublishedSink = std::function<void()>;

    DxgiCaptureHubService();
    ~DxgiCaptureHubService();

    DxgiCaptureHubService(const DxgiCaptureHubService&) = delete;
    DxgiCaptureHubService& operator=(const DxgiCaptureHubService&) = delete;

    // Start feeding the preview for `monitor`. Fails fast — without opening any
    // duplication — when the monitor's adapter is not the preview renderer's
    // (cross-GPU NT-handle opens fail; the caller falls back to WGC, and a
    // second duplication is never opened), or when the monitor has no stable
    // GDI device name. On success the pump thread opens the duplication and
    // starts calling `sink`. Replaces any previous subscription.
    bool Subscribe(HMONITOR monitor, HandleSink sink, FramePublishedSink frame_sink);

    // Close the capture (refcount to zero -> the duplication closes). Safe to
    // call when nothing is subscribed. Frames already in flight to the sink may
    // still arrive; the sink owner guards its own lifetime (QPointer marshal).
    void Unsubscribe();

    // The recording engine is about to duplicate the previewed output. Takes
    // the hub's lease — the subscription and held frame stay alive, but the
    // duplication is closed — and BLOCKS until the pump thread has actually
    // closed it: an output can only be duplicated once per process, so the
    // close must have happened before StartRecording proceeds, not merely be
    // queued. The acknowledgement carries this request's own serial, so no
    // later command can stand in for it, and the service opens nothing at all
    // until ReturnEngineLease(). Bounded wait; logs and returns anyway if the
    // pump thread is wedged (the recording then fails to open its capture
    // rather than the UI hanging).
    void RequestEngineLease();

    // The recording ended: return the lease. The hub reopens its duplication
    // and the sink re-announces a fresh shared texture with the first frame
    // (the producer's device is recreated per open). Asynchronous.
    void ReturnEngineLease();

    [[nodiscard]] PreviewPublishStats GetPreviewPublishStats() const noexcept;
    void ResetPreviewPublishStats() noexcept;

  private:
    // Only a Subscribe carries one; the other three commands are their own
    // instruction.
    struct SubscribePayload {
        std::wstring device_name;
        HandleSink sink;
        FramePublishedSink frame_sink;
    };

    void WorkerProc(std::stop_token stop_token);

    CaptureHubCommandQueue<SubscribePayload> commands_;

    std::jthread worker_;
    std::atomic<uint64_t> publish_attempts_{0};
    std::atomic<uint64_t> published_frames_{0};
    std::atomic<uint64_t> publish_drops_{0};
};

} // namespace exosnap
