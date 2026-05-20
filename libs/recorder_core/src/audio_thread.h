#pragma once

// AudioThread: capture source -> audio encode -> mux routing.

#include "session_internal.h"

#include <recorder_core/interfaces/IAudioCaptureSource.h>

#include <memory>
#include <thread>

namespace recorder_core {

class AudioThread {
  public:
    AudioThread(SessionState& state, std::unique_ptr<IAudioCaptureSource> source, uint32_t track_id);
    ~AudioThread();

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

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
    std::unique_ptr<IAudioCaptureSource> source_;
    uint32_t track_id_ = 0;
    std::thread m_thread;
};

} // namespace recorder_core
