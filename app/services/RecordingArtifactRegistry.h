#pragma once

#include <exosnap/engine/recording_media_types.h>

#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <windows.h>

namespace exosnap {

struct RecordingArtifactKey {
    engine::RecordingId recording_id;
    engine::SegmentId segment_id;
    uint64_t writer_generation = 0;
    uint64_t descriptor_revision = 0;
    auto operator<=>(const RecordingArtifactKey&) const = default;
};

enum class ArtifactDisposition { Completed, DeferredByLease, Failure, NotFound };

class RecordingArtifactRegistry {
  private:
    struct State;

  public:
    class SourceLease {
      public:
        SourceLease(SourceLease&& other) noexcept;
        SourceLease& operator=(SourceLease&& other) noexcept;
        ~SourceLease();
        SourceLease(const SourceLease&) = delete;
        SourceLease& operator=(const SourceLease&) = delete;

        [[nodiscard]] HANDLE handle() const noexcept {
            return handle_;
        }
        [[nodiscard]] const engine::FileIdentity& identity() const noexcept {
            return identity_;
        }
        [[nodiscard]] const std::filesystem::path& path() const noexcept {
            return path_;
        }
        [[nodiscard]] const std::vector<engine::ClusterExtent>& extents() const noexcept {
            return extents_;
        }
        [[nodiscard]] uint64_t readable_end_offset() const noexcept {
            return readable_end_offset_;
        }

      private:
        friend class RecordingArtifactRegistry;
        SourceLease(std::shared_ptr<State> state, RecordingArtifactKey key, HANDLE handle,
                    engine::FileIdentity identity, std::filesystem::path path,
                    std::vector<engine::ClusterExtent> extents, uint64_t readable_end_offset) noexcept;
        void Release() noexcept;

        std::shared_ptr<State> state_;
        RecordingArtifactKey key_;
        HANDLE handle_ = INVALID_HANDLE_VALUE;
        engine::FileIdentity identity_;
        std::filesystem::path path_;
        std::vector<engine::ClusterExtent> extents_;
        uint64_t readable_end_offset_ = 0;
    };

    RecordingArtifactRegistry();
    ~RecordingArtifactRegistry() = default;
    RecordingArtifactRegistry(const RecordingArtifactRegistry&) = delete;
    RecordingArtifactRegistry& operator=(const RecordingArtifactRegistry&) = delete;

    [[nodiscard]] bool RegisterSource(const engine::RecordingMediaDescriptor& descriptor, bool writer_open);
    [[nodiscard]] std::optional<SourceLease> AcquireSourceLease(const RecordingArtifactKey& key,
                                                                const engine::FileIdentity& identity,
                                                                std::vector<engine::ClusterExtent> extents = {},
                                                                uint64_t readable_end_offset = 0);
    [[nodiscard]] bool MarkWriterClosed(const RecordingArtifactKey& key);
    [[nodiscard]] ArtifactDisposition RenameClosed(const RecordingArtifactKey& key,
                                                   const std::filesystem::path& destination,
                                                   bool defer_while_leased = false);
    // Updates the locator after a caller-owned move if the destination still names the registered file.
    // Existing leases remain valid; this does not move or delete the file.
    [[nodiscard]] ArtifactDisposition RebindClosedSource(const RecordingArtifactKey& key,
                                                         const std::filesystem::path& destination);
    [[nodiscard]] ArtifactDisposition RequestArtifactDeletion(const RecordingArtifactKey& key);

  private:
    std::shared_ptr<State> state_;
};

} // namespace exosnap
