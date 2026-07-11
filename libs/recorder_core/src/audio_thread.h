#pragma once

// AudioThread: capture source -> audio encode -> mux routing.

#include "session_internal.h"

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <memory>
#include <thread>

namespace recorder_core {

// Ownership contract (shared by all session workers): the object must be owned
// by a std::shared_ptr, and Start() hands the running thread a shared_ptr to
// the worker — which in turn holds the SessionState alive. If the session gives
// up on a stalled worker (finalize stall, producer hang) and drops its handle,
// the thread keeps everything it touches alive until it has observed the stop
// flag and run out; nothing is ever detached without that ownership handoff.
class AudioThread : public std::enable_shared_from_this<AudioThread> {
  public:
    AudioThread(std::shared_ptr<SessionState> state, std::unique_ptr<IAudioCaptureSource> source, uint32_t track_id);
    ~AudioThread();

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    // Requires shared_ptr ownership of this object (see class comment).
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

    std::shared_ptr<SessionState> m_state_ptr;
    SessionState& m_state; // = *m_state_ptr (kept as a reference for Run())
    std::unique_ptr<IAudioCaptureSource> source_;
    uint32_t track_id_ = 0;
    float m_smoothed_rms_ = 0.0f;
    std::thread m_thread;
};

} // namespace recorder_core
