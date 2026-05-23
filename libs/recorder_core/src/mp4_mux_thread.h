#pragma once

// MP4 mux thread: IMFSinkWriter-based muxer for H.264 + AAC output.
// Mirrors the interface of MuxThread; only used when Container::Mp4 is selected.

#include <thread>
#include <windows.h>

namespace recorder_core {

struct SessionState;

class Mp4MuxThread {
  public:
    explicit Mp4MuxThread(SessionState& state);
    ~Mp4MuxThread();

    Mp4MuxThread(const Mp4MuxThread&) = delete;
    Mp4MuxThread& operator=(const Mp4MuxThread&) = delete;

    void Start();
    bool Join(unsigned timeout_ms);

    HANDLE NativeHandle() noexcept {
        return m_thread.joinable() ? m_thread.native_handle() : nullptr;
    }

  private:
    void Run();

    SessionState& m_state;
    std::thread m_thread;
};

} // namespace recorder_core
