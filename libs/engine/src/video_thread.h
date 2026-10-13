#pragma once

// Capture and D3D11 contexts belong to this worker. Capture may outlive a file
// attachment while raw readers remain. Configure, registration, encode, reap
// and encoder destruction all run on this same worker.

#include "session_internal.h"

#include <memory>
#include <thread>

namespace exosnap::engine {

class FileOutput;

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
    [[nodiscard]] HANDLE OutputEndedHandle() const noexcept;

    // Returns the native HANDLE for use with WaitForMultipleObjects before joining.
    // Not valid after Join() completes.  Not const: std::thread::native_handle is not const.
    HANDLE NativeHandle() noexcept {
        return m_thread.joinable() ? m_thread.native_handle() : nullptr;
    }

  private:
    struct NativeState;
    void Run();
    void RunAttachment(const std::shared_ptr<NativeState>& native, const std::shared_ptr<FileOutput>& output);
    void PumpRawReaders(NativeState& native);

    std::shared_ptr<SessionState> m_state_ptr;
    std::shared_ptr<FileOutput> m_initial_output;
    std::atomic<bool> m_started{false};
    std::thread m_thread;
};

} // namespace exosnap::engine
