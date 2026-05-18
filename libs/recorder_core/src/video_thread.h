#pragma once

// VideoThread: WGC capture + D3D11 color conversion + NVENC AV1 encode.
//
// D3D11 threading contract
// ========================
// ID3D11DeviceContext and ID3D11VideoContext are used EXCLUSIVELY on VideoThread.
// No other thread may call any method on these interfaces.
// The shared ID3D11Device lifetime is owned by RecorderSession::Impl; VideoThread
// borrows the raw pointer but does not extend its lifetime.

#include "session_internal.h"

#include <thread>

namespace recorder_core {

class VideoThread {
  public:
    explicit VideoThread(SessionState& state);
    ~VideoThread();

    VideoThread(const VideoThread&) = delete;
    VideoThread& operator=(const VideoThread&) = delete;

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
