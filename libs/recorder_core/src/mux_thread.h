#pragma once

// MuxThread: streaming (constant-RAM) Matroska writer.
//
// Sequence:
//   1. Waits until video codec private is ready and all expected audio tracks
//      report codec private readiness (av1_ready && AudioAllReady(audio_track_count)).
//   2. Opens a MatroskaStreamWriter and writes the container preamble.
//   3. Drains premux buffers, then streams encoded packets from the mux_queue
//      into a small bounded reorder window, emitting Matroska clusters to disk as
//      packets pass the window horizon. Peak RAM is O(reorder window seconds),
//      NOT O(entire session) — this is the fix for the previous batch-collect
//      muxer which retained the whole recording in memory (latent OOM on long
//      recordings).
//   4. A/V epoch alignment (audio rebased to the video epoch) is resolved early
//      and applied per packet; H.264 Annex-B -> AVCC conversion is per packet.
//   5. On EOS, finalizes: drains the window, writes Cues, back-patches Duration
//      and the Segment size, and replaces the SeekHead placeholder.

#include "session_internal.h"

#include <thread>

namespace recorder_core {

class MuxThread {
  public:
    explicit MuxThread(SessionState& state);
    ~MuxThread();

    MuxThread(const MuxThread&) = delete;
    MuxThread& operator=(const MuxThread&) = delete;

    void Start();

    // Join with up to timeout_ms.  Returns true if joined cleanly.
    bool Join(unsigned timeout_ms = 10000);

    // Returns the native HANDLE for use with WaitForMultipleObjects before joining.
    // Not valid after Join() completes.  Not const: std::thread::native_handle is not const.
    HANDLE NativeHandle() noexcept {
        return m_thread.joinable() ? m_thread.native_handle() : nullptr;
    }

  private:
    void Run();

    SessionState& m_state;
    std::thread m_thread;
};

} // namespace recorder_core
