#pragma once

#include "frame_luminance.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace exosnap::engine {

// Per-frame luminance analysis as a single compute dispatch over the pre-encode
// surface. One pass produces every number both consumers need: the exact
// extremes that become HDR10 MaxCLL/MaxFALL/MinCLL, and a coarse histogram whose
// high percentile becomes the tone-map knee (see frame_luminance.h, which owns
// everything that interprets these numbers).
//
// Shader Model 5.0 / Direct3D 11 throughout. Wave intrinsics would express the
// reduction in a fraction of the instructions but require SM 6.0, which is bound
// to D3D12; the reduction therefore runs on groupshared memory plus per-group
// atomics, which every D3D11 feature-level-11 device supports.
//
// Readback is asynchronous by construction and never blocks the capture thread.
// Dispatch() copies the result into a ring of staging buffers, each fenced by an
// event query; TryTakeResult() hands back the oldest completed one and returns
// false while none is ready, so a result lags its frame by a few frames. That
// lag is irrelevant to both consumers: MaxCLL is a stream-wide maximum, and the
// knee is smoothed over hundreds of milliseconds anyway.
//
// Threading: single-thread, like every other pass here. Every method runs on the
// thread that owns the device context. The class does not own device or context.
class FrameLuminanceAnalyzer {
  public:
    // `pq_source` selects the decode applied before the luminance projection: by
    // default the source is linear scRGB FP16, and with the flag it is the
    // PQ-encoded BT.2020 R10G10B10A2 surface duplication hands out when it offers
    // no FP16 surface on an HDR desktop.
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, UINT width, UINT height, bool pq_source,
              std::string& err);

    [[nodiscard]] bool Initialised() const noexcept {
        return compute_shader_ != nullptr;
    }

    // Analyse one frame. Records the measurement for a later TryTakeResult().
    // Returns false only on a real GPU error; a full readback ring is not one --
    // the frame is skipped silently, which is the correct response to a consumer
    // that is not collecting results.
    bool Dispatch(ID3D11Texture2D* src, std::string& err);

    // Hands back the oldest completed measurement, or returns false when none has
    // landed yet. Never blocks and never flushes.
    bool TryTakeResult(FrameLuminanceStats* out);

    // Drops every view cache and every in-flight readback. The device the
    // measurements were taken on is gone, so the pending results describe
    // surfaces that no longer exist and must not reach the file's metadata.
    void Reset() noexcept;

  private:
    ID3D11UnorderedAccessView* ResultUav() const noexcept {
        return result_uav_.get();
    }
    ID3D11ShaderResourceView* SrvFor(ID3D11Texture2D* tex, std::string& err);

    // One in-flight readback: the GPU->CPU copy and the fence that says it landed.
    struct ReadbackSlot {
        winrt::com_ptr<ID3D11Buffer> staging;
        winrt::com_ptr<ID3D11Query> fence;
        bool pending = false;
    };

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    bool pq_source_ = false;
    // Divides each threadgroup's fixed-point luminance sum before it reaches the
    // single 32-bit global accumulator. Sized from the surface so a frame of
    // uniform maximum luminance still cannot overflow it; the CPU multiplies it
    // back out when it forms the mean.
    uint32_t sum_divisor_ = 1;

    winrt::com_ptr<ID3D11ComputeShader> compute_shader_;
    winrt::com_ptr<ID3D11Buffer> constants_;
    winrt::com_ptr<ID3D11Buffer> result_;
    winrt::com_ptr<ID3D11UnorderedAccessView> result_uav_;

    std::vector<ReadbackSlot> readback_;
    size_t readback_write_ = 0; // next slot Dispatch() will fill
    size_t readback_read_ = 0;  // oldest slot TryTakeResult() will examine
    size_t readback_inflight_ = 0;

    std::unordered_map<ID3D11Texture2D*, winrt::com_ptr<ID3D11ShaderResourceView>> srv_cache_;
};

} // namespace exosnap::engine
