#pragma once

// AudioThread: WASAPI loopback capture + Media Foundation AAC encode.

#include "session_internal.h"

#include <thread>

namespace recorder_core {

class AudioThread {
public:
    explicit AudioThread(SessionState& state);
    ~AudioThread();

    AudioThread(const AudioThread&)            = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    void Start();

    // Join with up to timeout_ms.  Returns true if joined cleanly.
    bool Join(unsigned timeout_ms = 10000);

    // Returns the native HANDLE for use with WaitForMultipleObjects before joining.
    // Not valid after Join() completes.  Not const: std::thread::native_handle is not const.
    HANDLE NativeHandle() noexcept { return m_thread.joinable() ? m_thread.native_handle() : nullptr; }

private:
    void Run();

    SessionState& m_state;
    std::thread   m_thread;
};

} // namespace recorder_core
