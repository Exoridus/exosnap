#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace exosnap::engine {

struct MediaIndexSnapshot {
    IndexSnapshotToken token;
    std::filesystem::path journal_path;
    FileIdentity journal_identity;
    uint64_t journal_end = 0;
    std::shared_ptr<const RecordingMediaDescriptor> descriptor;
};

// One producer publishes metadata only. The self-owned worker performs all writes.
// Snapshots iterate journal records with bounded memory and remain finite after growth.
class RecordingMediaIndex {
  public:
    explicit RecordingMediaIndex(std::filesystem::path journal_path);
    ~RecordingMediaIndex();
    RecordingMediaIndex(const RecordingMediaIndex&) = delete;
    RecordingMediaIndex& operator=(const RecordingMediaIndex&) = delete;

    ReceiptAcceptance RegisterDescriptor(std::shared_ptr<const RecordingMediaDescriptor> descriptor) noexcept;
    ReceiptAcceptance PublishCommitted(CommittedMediaReceipt receipt) noexcept;
    Outcome<MediaIndexSnapshot> Freeze() const;
    [[nodiscard]] bool needs_rebuild() const noexcept;
    [[nodiscard]] uint64_t journal_committed_through() const noexcept;
    [[nodiscard]] size_t queued_bytes() const noexcept;
    [[nodiscard]] size_t cache_bytes() const noexcept;

    static Outcome<size_t> Visit(const MediaIndexSnapshot& snapshot,
                                 const std::function<bool(const RecordingMediaDescriptor&)>& descriptor_visitor,
                                 const std::function<bool(const CommittedMediaReceipt&)>& receipt_visitor);
    // Incomplete trailing records are ignored. Invalid complete records are rejected.
    static Outcome<size_t> InspectJournal(const std::filesystem::path& path);

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace exosnap::engine
