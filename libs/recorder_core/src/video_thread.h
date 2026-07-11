#pragma once

// VideoThread: WGC capture + D3D11 color conversion + NVENC AV1 encode.
//
// D3D11 threading contract
// ========================
// The D3D11 device, its immediate context and the video context are created
// locally in Run() and owned for the lifetime of that call. They are used
// EXCLUSIVELY on VideoThread — no other thread may call any method on these
// interfaces. Nothing is borrowed from the session; the device is not shared.

#include "session_internal.h"

#include <memory>
#include <thread>

namespace recorder_core {

// Ownership contract: see audio_thread.h — the object must be owned by a
// std::shared_ptr, and Start() hands the running thread shared ownership of
// the worker and (through it) the SessionState, so a producer that misses the
// shutdown budget can never be left writing through freed session memory.
class VideoThread : public std::enable_shared_from_this<VideoThread> {
  public:
    explicit VideoThread(std::shared_ptr<SessionState> state);
    ~VideoThread();

    VideoThread(const VideoThread&) = delete;
    VideoThread& operator=(const VideoThread&) = delete;

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
    std::thread m_thread;
};

} // namespace recorder_core
