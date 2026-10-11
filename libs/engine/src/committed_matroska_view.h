#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <windows.h>

struct AVIOContext;

namespace exosnap::engine {

// One finite, immutable demux input. Destroy demuxers before destroying the view.
// Read/seek have one worker owner. Cancel may run concurrently with that owner.
class CommittedMatroskaView {
  public:
    static Outcome<std::unique_ptr<CommittedMatroskaView>>
    Open(std::shared_ptr<const RecordingMediaDescriptor> descriptor, std::vector<ClusterExtent> extents,
         uint64_t readable_end);
    static Outcome<std::unique_ptr<CommittedMatroskaView>>
    OpenFromPinnedHandle(std::shared_ptr<const RecordingMediaDescriptor> descriptor, std::vector<ClusterExtent> extents,
                         uint64_t readable_end, HANDLE borrowed_source);
    ~CommittedMatroskaView();
    CommittedMatroskaView(const CommittedMatroskaView&) = delete;
    CommittedMatroskaView& operator=(const CommittedMatroskaView&) = delete;

    Outcome<size_t> ReadAt(uint64_t offset, std::span<uint8_t> destination);
    // Call before demux starts. Hooks run on the clip worker around each physical read.
    void SetIoControl(const std::atomic<bool>* cancelled, std::function<bool(uint64_t)> before_read,
                      std::function<void(uint64_t)> after_read);
    void Cancel() noexcept;
    [[nodiscard]] uint64_t size() const noexcept;
    [[nodiscard]] AVIOContext* avio() const noexcept;
    [[nodiscard]] const std::optional<EngineError>& last_error() const noexcept;

  private:
    struct State;
    explicit CommittedMatroskaView(std::unique_ptr<State> state);
    static Outcome<std::unique_ptr<CommittedMatroskaView>>
    OpenImpl(std::shared_ptr<const RecordingMediaDescriptor> descriptor, std::vector<ClusterExtent> extents,
             uint64_t readable_end, HANDLE borrowed_source);
    static int ReadPacket(void* opaque, uint8_t* buffer, int size);
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    std::unique_ptr<State> state_;
};

} // namespace exosnap::engine
