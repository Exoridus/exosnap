#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <atomic>
#include <memory>
#include <span>

struct AVIOContext;

namespace exosnap::engine {

// One finite, immutable demux input. Destroy demuxers before destroying the view.
// Read/seek have one worker owner. Cancel may run concurrently with that owner.
class CommittedMatroskaView {
  public:
    static Outcome<std::unique_ptr<CommittedMatroskaView>>
    Open(std::shared_ptr<const RecordingMediaDescriptor> descriptor, std::vector<ClusterExtent> extents,
         uint64_t readable_end);
    ~CommittedMatroskaView();
    CommittedMatroskaView(const CommittedMatroskaView&) = delete;
    CommittedMatroskaView& operator=(const CommittedMatroskaView&) = delete;

    Outcome<size_t> ReadAt(uint64_t offset, std::span<uint8_t> destination);
    void Cancel() noexcept;
    [[nodiscard]] uint64_t size() const noexcept;
    [[nodiscard]] AVIOContext* avio() const noexcept;
    [[nodiscard]] const std::optional<EngineError>& last_error() const noexcept;

  private:
    struct State;
    explicit CommittedMatroskaView(std::unique_ptr<State> state);
    static int ReadPacket(void* opaque, uint8_t* buffer, int size);
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    std::unique_ptr<State> state_;
};

} // namespace exosnap::engine
