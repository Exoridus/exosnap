#pragma once

#include <exosnap/engine/background_io.h>
#include <exosnap/engine/recording_media_types.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace exosnap::engine {

[[nodiscard]] std::optional<int64_t> SegmentLocalToRecordingNs(int64_t segment_origin_r_ns, int64_t local_ns) noexcept;
[[nodiscard]] std::optional<int64_t> RecordingToSegmentLocalNs(int64_t segment_origin_r_ns,
                                                               int64_t recording_ns) noexcept;

// One producer publishes metadata only. The self-owned worker performs all writes.
// Snapshots iterate journal records with bounded memory and remain finite after growth.
class RecordingMediaIndex {
  public:
    explicit RecordingMediaIndex(std::filesystem::path journal_path, BackgroundIoAdmission admission = {});
    ~RecordingMediaIndex();
    RecordingMediaIndex(const RecordingMediaIndex&) = delete;
    RecordingMediaIndex& operator=(const RecordingMediaIndex&) = delete;

    ReceiptAcceptance RegisterDescriptor(std::shared_ptr<const RecordingMediaDescriptor> descriptor) noexcept;
    ReceiptAcceptance PublishCommitted(CommittedMediaReceipt receipt) noexcept;
    Outcome<MediaIndexSnapshot> Freeze() const;
    [[nodiscard]] bool needs_rebuild() const noexcept;
    [[nodiscard]] bool has_closed_clip_interval() const noexcept;
    [[nodiscard]] uint64_t journal_committed_through() const noexcept;
    [[nodiscard]] size_t queued_bytes() const noexcept;
    [[nodiscard]] size_t cache_bytes() const noexcept;

    static Outcome<size_t> Visit(const MediaIndexSnapshot& snapshot,
                                 const std::function<bool(const RecordingMediaDescriptor&)>& descriptor_visitor,
                                 const std::function<bool(const CommittedMediaReceipt&)>& receipt_visitor,
                                 const std::function<bool(uint64_t)>& before_read = {});
    // Incomplete trailing records are ignored. Invalid complete records are rejected.
    static Outcome<size_t> InspectJournal(const std::filesystem::path& path);

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace exosnap::engine
