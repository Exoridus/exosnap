#pragma once

// MuxThread: batch-collect Matroska writer.
//
// Startup sequence (Stage 1 — correctness-first):
//   1. Waits until both codec-private data are ready (av1_ready && aac_ready).
//   2. Drains premux buffers into an in-memory batch collection.
//   3. Collects all encoded packets from the mux_queue until both EOS sentinels arrive.
//   4. Stable-sorts the entire collected packet set by PTS (global ordering).
//   5. Opens the output file, initializes tracks, writes all packets in sorted order.
//   6. Finalizes the segment.

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
