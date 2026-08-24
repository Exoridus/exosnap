#pragma once

// Thin, reusable D3D11 timestamp-query wrapper for measuring real GPU execution
// time of a single render pass, without ever stalling the capture/encode path.
//
// One instance measures one GPU stage (composite, tone-map, RGB->YUV blit, webcam
// upload, ...). The owner brackets the pass with Begin()/End() on the same
// immediate context that issues the pass, then calls Poll() once per tick to
// harvest whichever earlier frame's result has become ready. Results lag by a few
// frames — that latency is absorbed by a small internal ring, never by blocking.
//
// Design contract (see gpu_timestamp_math.h for the pure resolution logic):
//   - Strictly additive: Begin/End only issue cheap query markers; they never read
//     back, never flush, never wait. If the device does not support timestamp
//     queries, or a ring slot is unavailable, the pass is simply not measured.
//   - Non-blocking harvest: Poll() reads with DONOTFLUSH and returns nullopt when
//     the oldest in-flight result is not ready yet. A not-ready or disjoint frame
//     is dropped, never awaited.
//   - Threading: an instance is single-thread, owned by the thread that owns the
//     context (VideoThread per ADR-0009). It does not own the device/context.

#include <exosnap/engine/gpu_timestamp_math.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <optional>

namespace exosnap::engine {

class GpuStageTimer {
  public:
    GpuStageTimer() = default;

    GpuStageTimer(const GpuStageTimer&) = delete;
    GpuStageTimer& operator=(const GpuStageTimer&) = delete;

    // Create the query ring. Returns false (and leaves the timer inert) if the
    // device cannot create timestamp queries; callers treat an inert timer as
    // "this stage is not measured" and carry on. Never throws.
    bool Init(ID3D11Device* device) noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr;
    }

    // Bracket the GPU pass on `context`. Begin() opens the disjoint query and
    // stamps the start; End() stamps the end and closes the disjoint query. Both
    // are no-ops when the timer is inert or no free ring slot is available (the
    // frame is dropped rather than stalling). Begin()/End() must be paired.
    void Begin(ID3D11DeviceContext* context) noexcept;
    void End(ID3D11DeviceContext* context) noexcept;

    // Harvest the oldest completed measurement, if any. Non-blocking: returns
    // nullopt when nothing is ready yet or the ready frame was disjoint/invalid
    // (that sample is dropped). Call once per tick. Never throws.
    [[nodiscard]] std::optional<double> Poll(ID3D11DeviceContext* context) noexcept;

  private:
    // Ring depth: tolerates a handful of frames of CPU->GPU->readback latency.
    static constexpr std::size_t kSlots = 6;

    struct Slot {
        winrt::com_ptr<ID3D11Query> disjoint;
        winrt::com_ptr<ID3D11Query> begin_ts;
        winrt::com_ptr<ID3D11Query> end_ts;
        bool in_flight = false;
    };

    ID3D11Device* device_ = nullptr;
    std::array<Slot, kSlots> slots_{};
    std::size_t write_ = 0;   // next slot Begin() will use
    std::size_t harvest_ = 0; // oldest in-flight slot Poll() will inspect
    std::size_t in_flight_ = 0;
    bool pass_open_ = false; // guards against unpaired Begin/End
};

} // namespace exosnap::engine
