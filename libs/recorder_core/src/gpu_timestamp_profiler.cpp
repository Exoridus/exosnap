#include <recorder_core/gpu_timestamp_profiler.h>

namespace recorder_core {

bool GpuStageTimer::Init(ID3D11Device* device) noexcept {
    device_ = nullptr;
    write_ = harvest_ = in_flight_ = 0;
    pass_open_ = false;
    if (device == nullptr) {
        return false;
    }

    D3D11_QUERY_DESC disjoint_desc{};
    disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC ts_desc{};
    ts_desc.Query = D3D11_QUERY_TIMESTAMP;

    for (Slot& s : slots_) {
        s.disjoint = nullptr;
        s.begin_ts = nullptr;
        s.end_ts = nullptr;
        s.in_flight = false;
        if (FAILED(device->CreateQuery(&disjoint_desc, s.disjoint.put())) ||
            FAILED(device->CreateQuery(&ts_desc, s.begin_ts.put())) ||
            FAILED(device->CreateQuery(&ts_desc, s.end_ts.put()))) {
            // Timestamp queries unsupported (or OOM) — leave the timer inert.
            for (Slot& clear : slots_) {
                clear.disjoint = nullptr;
                clear.begin_ts = nullptr;
                clear.end_ts = nullptr;
            }
            return false;
        }
    }

    device_ = device;
    return true;
}

void GpuStageTimer::Begin(ID3D11DeviceContext* context) noexcept {
    if (device_ == nullptr || context == nullptr || pass_open_) {
        return;
    }
    // No free slot: every slot is still awaiting readback. Drop this frame's
    // measurement rather than overwrite an in-flight query (which would corrupt a
    // pending result) or stall waiting for one.
    if (in_flight_ >= kSlots) {
        return;
    }
    Slot& s = slots_[write_];
    context->Begin(s.disjoint.get());
    context->End(s.begin_ts.get()); // TIMESTAMP queries record on End()
    pass_open_ = true;
}

void GpuStageTimer::End(ID3D11DeviceContext* context) noexcept {
    if (device_ == nullptr || context == nullptr || !pass_open_) {
        return;
    }
    Slot& s = slots_[write_];
    context->End(s.end_ts.get());
    context->End(s.disjoint.get());
    s.in_flight = true;
    ++in_flight_;
    write_ = (write_ + 1) % kSlots;
    pass_open_ = false;
}

std::optional<double> GpuStageTimer::Poll(ID3D11DeviceContext* context) noexcept {
    if (device_ == nullptr || context == nullptr || in_flight_ == 0) {
        return std::nullopt;
    }
    Slot& s = slots_[harvest_];
    if (!s.in_flight) {
        return std::nullopt;
    }

    // Non-blocking: DONOTFLUSH means "tell me only if it is already done".
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    HRESULT hr = context->GetData(s.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr != S_OK) {
        return std::nullopt; // not ready yet — do not advance, do not wait
    }
    uint64_t begin_ticks = 0;
    uint64_t end_ticks = 0;
    const HRESULT hb =
        context->GetData(s.begin_ts.get(), &begin_ticks, sizeof(begin_ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    const HRESULT he = context->GetData(s.end_ts.get(), &end_ticks, sizeof(end_ticks), D3D11_ASYNC_GETDATA_DONOTFLUSH);

    // The disjoint query is done, so the bracketed timestamps are done too; if a
    // read still somehow reports not-ready, drop the sample. Either way this slot
    // is now consumed and returns to the ring.
    s.in_flight = false;
    --in_flight_;
    harvest_ = (harvest_ + 1) % kSlots;

    if (hb != S_OK || he != S_OK) {
        return std::nullopt;
    }
    const GpuTimestampDisjoint resolved{dj.Frequency, dj.Disjoint != FALSE};
    return ResolveGpuSpanMs(begin_ticks, end_ticks, resolved);
}

} // namespace recorder_core
