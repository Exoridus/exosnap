#pragma once

// Two-slot staging-texture ring that lets a single D3D11-context-owning
// thread read back GPU frames without Map() stalling on the copy it just
// submitted (Strand 3 slice 1 -- WYSIWYG preview tap).
//
// Usage pattern (one Submit()/TryReadReady() pair per throttled preview
// tick, NOT per encoded frame): iteration N's Submit() issues CopyResource
// into the slot not currently holding the oldest live copy; TryReadReady()
// maps the slot submitted on the PREVIOUS Submit() call (one tick of
// latency, ~33 ms at the ~30 Hz preview cadence this is designed for). By
// the time that older slot is mapped the GPU has almost always already
// finished that copy, so Map() resolves immediately instead of blocking on
// the newest, possibly still in-flight one. Map() additionally uses
// D3D11_MAP_FLAG_DO_NOT_WAIT: if the GPU genuinely has not finished the
// one-tick-old copy (e.g. back-to-back CFR catch-up frames after a stall,
// where publish ticks are NOT ~33 ms apart in wall time), TryReadReady()
// reports not-ready instead of hard-blocking the video thread.
//
// Each slot carries the caller-supplied timestamp of the frame that was
// copied into it, and TryReadReady() returns that stored timestamp -- the
// frame handed back is one tick OLDER than the most recent Submit(), so the
// caller must never stamp it with the current tick's PTS.
//
// D3D11 threading contract: like the rest of VideoThread's device state
// (see video_thread.h), this class must only be used from the thread that
// owns the ID3D11DeviceContext passed to Submit()/TryReadReady()/FinishRead().

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>

namespace recorder_core {

class PreviewStagingRing {
  public:
    // source_desc describes the texture that will be copied (format and
    // dimensions must match what Submit() is later given). Initialize
    // forces STAGING usage + CPU_ACCESS_READ + no bind flags on the two
    // internal slots regardless of what source_desc specifies for those
    // fields.
    bool Initialize(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& source_desc);

    // Drops any pending read and resets the ring to its just-initialized
    // state (next TryReadReady() will report not-ready again until two more
    // Submit() calls have happened).
    void Reset() noexcept;

    // Copies `source` into this tick's slot and records `timestamp_ns` as
    // that slot's frame timestamp. Must not be called while a TryReadReady()
    // result is still pending a FinishRead() -- doing so is a caller
    // contract violation and is ignored (no-op) rather than corrupting the
    // in-flight read.
    void Submit(ID3D11DeviceContext* context, ID3D11Texture2D* source, uint64_t timestamp_ns);

    // Maps the slot submitted one Submit() call ago and returns its stored
    // timestamp. Returns false (leaving out params untouched) until at least
    // two Submit() calls have happened, if a read is already pending, if the
    // GPU has not finished that slot's copy yet (D3D11_MAP_FLAG_DO_NOT_WAIT;
    // the tick is skipped rather than stalling the calling thread), or if
    // Map() fails. On true, the caller must call FinishRead() once done
    // reading out_data.
    bool TryReadReady(ID3D11DeviceContext* context, const uint8_t** out_data, uint32_t* out_row_pitch,
                      uint64_t* out_timestamp_ns);

    // Unmaps the slot returned by the last successful TryReadReady(). No-op
    // if no read is pending. Safe to call unconditionally (idempotent).
    void FinishRead(ID3D11DeviceContext* context) noexcept;

  private:
    winrt::com_ptr<ID3D11Texture2D> m_slots[2];
    uint64_t m_slot_timestamps_ns[2] = {0, 0};
    int m_next_write_slot = 0;
    int m_submit_count = 0;
    bool m_read_pending = false;
    int m_pending_read_slot = -1;
};

// RAII helper: guarantees FinishRead() runs even if the code between
// TryReadReady() and FinishRead() throws (e.g. std::bad_alloc while sizing
// the destination buffer, or an exception escaping the app's preview
// callback). Without it a thrown exception would leave the slot mapped
// forever and silently kill the preview for the rest of the session.
class PreviewRingReadGuard {
  public:
    PreviewRingReadGuard(PreviewStagingRing& ring, ID3D11DeviceContext* context) noexcept
        : m_ring(ring), m_context(context) {
    }
    ~PreviewRingReadGuard() {
        m_ring.FinishRead(m_context);
    }
    PreviewRingReadGuard(const PreviewRingReadGuard&) = delete;
    PreviewRingReadGuard& operator=(const PreviewRingReadGuard&) = delete;

  private:
    PreviewStagingRing& m_ring;
    ID3D11DeviceContext* m_context;
};

} // namespace recorder_core
