#include "file_output.h"
#include <atomic>
#include <cstdint>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>
#include <memory>
#include <utility>

#include <exosnap/engine/interfaces/IVideoEncoder.h>
#include <stdexcept>

namespace exosnap::engine {

FileOutput::FileOutput(std::weak_ptr<SessionState> state, RecorderConfig config, OutputGeneration generation)
    : state_(std::move(state)), config_(std::move(config)), generation_(generation),
      video_ended_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!video_ended_)
        throw std::runtime_error("Cannot create finite video endpoint event");
}
FileOutput::~FileOutput() {
    CloseHandle(video_ended_);
}
std::shared_ptr<SessionState> FileOutput::LockState() const {
    return state_.lock();
}
const RecorderConfig& FileOutput::config() const noexcept {
    return config_;
}
OutputGeneration FileOutput::generation() const noexcept {
    return generation_;
}
FileOutputPhase FileOutput::phase() const noexcept {
    return phase_.load();
}
bool FileOutput::Attach() noexcept {
    auto expected = FileOutputPhase::Preparing;
    return phase_.compare_exchange_strong(expected, FileOutputPhase::Attached, std::memory_order_acq_rel);
}
void FileOutput::RequestStop(RuntimeQpc100ns end) noexcept {
    uint64_t unset = 0;
    end_qpc_.compare_exchange_strong(unset, end.value, std::memory_order_acq_rel);
    auto current = phase_.load(std::memory_order_acquire);
    while (current == FileOutputPhase::Preparing || current == FileOutputPhase::Attached) {
        const auto next = current == FileOutputPhase::Preparing ? FileOutputPhase::Closed : FileOutputPhase::Closing;
        if (phase_.compare_exchange_weak(current, next, std::memory_order_acq_rel)) {
            if (next == FileOutputPhase::Closed)
                SetEvent(video_ended_);
            break;
        }
    }
}
RuntimeQpc100ns FileOutput::end_qpc() const noexcept {
    return {end_qpc_.load(std::memory_order_acquire)};
}
void FileOutput::VideoDrained() noexcept {
    phase_.store(FileOutputPhase::Closed, std::memory_order_release);
    SetEvent(video_ended_);
}
HANDLE FileOutput::VideoEndedHandle() const noexcept {
    return video_ended_;
}
IVideoEncoder* FileOutput::OwnEncoder(std::unique_ptr<IVideoEncoder> encoder) {
    if (encoder_)
        throw std::logic_error("Output already owns an encoder");
    encoder_ = std::move(encoder);
    return encoder_.get();
}
void FileOutput::ReleaseEncoder() noexcept {
    encoder_.reset();
}

} // namespace exosnap::engine
