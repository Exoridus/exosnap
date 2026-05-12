#pragma once

// MuxThread: streaming Matroska writer.
//
// Startup sequence:
//   1. Waits until both codec-private data are ready (av1_ready && aac_ready).
//   2. Flushes premux buffers (video + audio) into the segment in PTS order.
//   3. Continues consuming the live mux_queue by PTS interleaving (32 ms lookahead window).
//   4. After both EOS sentinels are received, writes remaining queued packets, finalizes.

#include "session_internal.h"

#include <thread>

namespace recorder_core {

class MuxThread {
public:
    explicit MuxThread(SessionState& state);
    ~MuxThread();

    MuxThread(const MuxThread&)            = delete;
    MuxThread& operator=(const MuxThread&) = delete;

    void Start();

    // Join with up to timeout_ms.  Returns true if joined cleanly.
    bool Join(unsigned timeout_ms = 10000);

private:
    void Run();

    SessionState& m_state;
    std::thread   m_thread;
};

} // namespace recorder_core
