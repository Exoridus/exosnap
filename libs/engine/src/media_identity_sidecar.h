#pragma once

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/background_io.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace exosnap::engine {

struct MediaIdentitySnapshot {
    FileIdentity file_identity;
    uint64_t file_size = 0;
    uint64_t last_write_time = 0;
    std::array<uint8_t, 32> sha256{};
};

struct MediaIdentityReferenceCopyGuard {
    std::vector<std::filesystem::path> created_paths;
    bool committed = false;

    MediaIdentityReferenceCopyGuard() = default;
    MediaIdentityReferenceCopyGuard(const MediaIdentityReferenceCopyGuard&) = delete;
    MediaIdentityReferenceCopyGuard& operator=(const MediaIdentityReferenceCopyGuard&) = delete;
    ~MediaIdentityReferenceCopyGuard();
    std::vector<std::filesystem::path> Commit();
};

std::filesystem::path MediaIdentitySidecarPath(const std::filesystem::path& media);
Outcome<std::shared_ptr<void>> AcquireMediaReadLease(const std::filesystem::path& media);
Outcome<MediaIdentitySnapshot> HashFinalMedia(const std::filesystem::path& media,
                                              const std::function<bool()>& continue_io = {},
                                              const BackgroundIoAdmission& before_io = {},
                                              const std::atomic<bool>* cancelled = nullptr);
AudioMetadataReadResult ReadMediaIdentitySidecar(const std::filesystem::path& media,
                                                 const std::function<bool()>& continue_io = {},
                                                 const BackgroundIoAdmission& before_io = {},
                                                 const std::atomic<bool>* cancelled = nullptr);
Outcome<std::monostate> CopyMediaIdentityReferenceFiles(const std::filesystem::path& source,
                                                        const std::filesystem::path& output,
                                                        const MediaIdentitySidecarReferences& references,
                                                        MediaIdentityReferenceCopyGuard* guard = nullptr,
                                                        const BackgroundIoAdmission& before_io = {},
                                                        const std::atomic<bool>* cancelled = nullptr);

} // namespace exosnap::engine
