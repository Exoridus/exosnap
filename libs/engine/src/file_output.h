#pragma once

#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>

#include <atomic>
#include <memory>
#include <windows.h>

namespace exosnap::engine {

struct SessionState;
class IVideoEncoder;

using RuntimeQpc100ns = ScalarValue<struct RuntimeQpc100nsTag>;
enum class FileOutputPhase { Preparing, Attached, Closing, Closed, Failed };

class FileOutput {
  public:
    FileOutput(std::weak_ptr<SessionState> state, RecorderConfig config, OutputGeneration generation);
    ~FileOutput();
    FileOutput(const FileOutput&) = delete;
    FileOutput& operator=(const FileOutput&) = delete;

    [[nodiscard]] std::shared_ptr<SessionState> LockState() const;
    [[nodiscard]] const RecorderConfig& config() const noexcept;
    [[nodiscard]] OutputGeneration generation() const noexcept;
    [[nodiscard]] FileOutputPhase phase() const noexcept;
    bool Attach() noexcept;
    // The first endpoint wins. Closing an output never stops its capture runtime.
    void RequestStop(RuntimeQpc100ns end) noexcept;
    [[nodiscard]] RuntimeQpc100ns end_qpc() const noexcept;
    void VideoDrained() noexcept;
    [[nodiscard]] HANDLE VideoEndedHandle() const noexcept;
    // All encoder operations and ownership transitions occur on the GPU owner.
    IVideoEncoder* OwnEncoder(std::unique_ptr<IVideoEncoder> encoder);
    void ReleaseEncoder() noexcept;

  private:
    std::weak_ptr<SessionState> state_;
    const RecorderConfig config_;
    const OutputGeneration generation_;
    std::atomic<FileOutputPhase> phase_{FileOutputPhase::Preparing};
    std::atomic<uint64_t> end_qpc_{0};
    HANDLE video_ended_ = nullptr;
    std::unique_ptr<IVideoEncoder> encoder_;
};

} // namespace exosnap::engine
