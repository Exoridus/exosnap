#include "preview_staging_ring.h"

namespace recorder_core {

bool PreviewStagingRing::Initialize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source_desc) {
    if (device == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc = source_desc;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;

    for (auto& slot : m_slots) {
        slot = nullptr;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, slot.put());
        if (FAILED(hr)) {
            m_slots[0] = nullptr;
            m_slots[1] = nullptr;
            return false;
        }
    }
    Reset();
    return true;
}

void PreviewStagingRing::Reset() noexcept {
    m_next_write_slot = 0;
    m_submit_count = 0;
    m_read_pending = false;
    m_pending_read_slot = -1;
    m_slot_timestamps_ns[0] = 0;
    m_slot_timestamps_ns[1] = 0;
}

void PreviewStagingRing::Submit(ID3D11DeviceContext* context, ID3D11Texture2D* source, uint64_t timestamp_ns) {
    if (context == nullptr || source == nullptr)
        return;
    if (m_slots[0] == nullptr || m_slots[1] == nullptr)
        return;
    if (m_read_pending)
        return; // caller contract violation: a read is still outstanding

    context->CopyResource(m_slots[m_next_write_slot].get(), source);
    m_slot_timestamps_ns[m_next_write_slot] = timestamp_ns;
    // The slot we just overwrote held the OLDEST live copy (see class
    // comment); next_write_slot now names that same oldest-remaining slot
    // again -- i.e. exactly the slot TryReadReady() should read next.
    m_next_write_slot = (m_next_write_slot + 1) % 2;
    ++m_submit_count;
}

bool PreviewStagingRing::TryReadReady(ID3D11DeviceContext* context, const uint8_t** out_data, uint32_t* out_row_pitch,
                                      uint64_t* out_timestamp_ns) {
    if (context == nullptr || out_data == nullptr || out_row_pitch == nullptr || out_timestamp_ns == nullptr)
        return false;
    if (m_read_pending)
        return false; // caller must FinishRead() before reading again
    if (m_submit_count < 2)
        return false; // ring not primed yet -- avoid reading stale/uninitialized data

    // next_write_slot currently names the slot that was submitted one
    // Submit() call ago (the one not about to be overwritten by the next
    // Submit()) -- that is the safely-readable, one-tick-old slot.
    const int read_slot = m_next_write_slot;

    // DO_NOT_WAIT: at the normal ~30 Hz cadence the one-tick-old copy has
    // long completed and this behaves exactly like a plain Map. But when
    // publish ticks arrive back-to-back in wall time (CFR catch-up after a
    // process stall throttles on PTS, not wall clock), the copy may still
    // be in flight -- in that case skip this tick instead of hard-blocking
    // the video thread (DXGI_ERROR_WAS_STILL_DRAWING).
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = context->Map(m_slots[read_slot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (FAILED(hr))
        return false;

    *out_data = static_cast<const uint8_t*>(mapped.pData);
    *out_row_pitch = mapped.RowPitch;
    *out_timestamp_ns = m_slot_timestamps_ns[read_slot];
    m_read_pending = true;
    m_pending_read_slot = read_slot;
    return true;
}

void PreviewStagingRing::FinishRead(ID3D11DeviceContext* context) noexcept {
    if (!m_read_pending || context == nullptr)
        return;
    context->Unmap(m_slots[m_pending_read_slot].get(), 0);
    m_read_pending = false;
    m_pending_read_slot = -1;
}

} // namespace recorder_core
