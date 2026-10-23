#pragma once

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/background_io.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace exosnap::engine {

std::shared_ptr<const AudioMetadataReadResult>
CachedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source, bool verified);
void RetainMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                              std::shared_ptr<const AudioMetadataReadResult> metadata, bool verified);
std::shared_ptr<const AudioMetadataReadResult>
CachedAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source);
void RetainAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                                     std::shared_ptr<const AudioMetadataReadResult> metadata);
std::shared_ptr<const EditMediaMetadata>
CachedEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>& source);
void RetainEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                             std::shared_ptr<const EditMediaMetadata> metadata);

// Each AVIO cursor owns its position while reads remain bound to the admitted object.
class LeasedMediaAvio {
  public:
    static std::unique_ptr<LeasedMediaAvio> Open(std::shared_ptr<const MediaReadCapability> source,
                                                 std::function<bool()> proceed = {},
                                                 BackgroundIoAdmission admission = {},
                                                 const std::atomic<bool>* cancelled = nullptr) {
        if (!source)
            return {};
        auto owner = std::unique_ptr<LeasedMediaAvio>(new LeasedMediaAvio());
        owner->source_ = std::move(source);
        owner->proceed_ = std::move(proceed);
        owner->admission_ = std::move(admission);
        owner->cancelled_ = cancelled;
        auto* buffer = static_cast<unsigned char*>(av_malloc(64 * 1024));
        if (!buffer)
            return {};
        owner->context_ = avio_alloc_context(buffer, 64 * 1024, 0, owner.get(), &Read, nullptr, &Seek);
        if (!owner->context_) {
            av_free(buffer);
            return {};
        }
        owner->context_->seekable = AVIO_SEEKABLE_NORMAL;
        return owner;
    }
    ~LeasedMediaAvio() {
        if (context_) {
            av_freep(&context_->buffer);
            avio_context_free(&context_);
        }
    }
    AVIOContext* context() const {
        return context_;
    }

  private:
    LeasedMediaAvio() = default;
    static int Read(void* opaque, uint8_t* bytes, int count) {
        auto& self = *static_cast<LeasedMediaAvio*>(opaque);
        if ((self.cancelled_ && self.cancelled_->load(std::memory_order_relaxed)) ||
            (self.proceed_ && !self.proceed_()) ||
            (self.admission_ && !self.admission_(static_cast<uint64_t>(count), self.cancelled_)))
            return AVERROR_EXIT;
        const auto read = self.source_->ReadAt(self.position_, {bytes, static_cast<size_t>(count)});
        if (!read.succeeded())
            return AVERROR(EIO);
        self.position_ += *read.value();
        return *read.value() ? static_cast<int>(*read.value()) : AVERROR_EOF;
    }
    static int64_t Seek(void* opaque, int64_t offset, int whence) {
        auto& self = *static_cast<LeasedMediaAvio*>(opaque);
        const auto size = self.source_->Identity().file_size;
        if (whence == AVSEEK_SIZE)
            return static_cast<int64_t>(size);
        whence &= ~AVSEEK_FORCE;
        const uint64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? self.position_ : size;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
            return AVERROR(EINVAL);
        if ((offset < 0 && static_cast<uint64_t>(-(offset + 1)) + 1 > base) ||
            (offset >= 0 &&
             static_cast<uint64_t>(offset) > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) - base))
            return AVERROR(EINVAL);
        self.position_ =
            offset < 0 ? base - (static_cast<uint64_t>(-(offset + 1)) + 1) : base + static_cast<uint64_t>(offset);
        return static_cast<int64_t>(self.position_);
    }
    std::shared_ptr<const MediaReadCapability> source_;
    std::function<bool()> proceed_;
    BackgroundIoAdmission admission_;
    const std::atomic<bool>* cancelled_ = nullptr;
    uint64_t position_ = 0;
    AVIOContext* context_ = nullptr;
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
Outcome<MediaIdentitySnapshot> HashFinalMedia(const std::filesystem::path& media,
                                              const std::function<bool()>& continue_io = {},
                                              const BackgroundIoAdmission& before_io = {},
                                              const std::atomic<bool>* cancelled = nullptr,
                                              MediaHashCause cause = MediaHashCause::Unspecified);
Outcome<std::monostate> CopyMediaIdentityReferenceFiles(const std::filesystem::path& source,
                                                        const std::filesystem::path& output,
                                                        const MediaIdentitySidecarReferences& references,
                                                        MediaIdentityReferenceCopyGuard* guard = nullptr,
                                                        const BackgroundIoAdmission& before_io = {},
                                                        const std::atomic<bool>* cancelled = nullptr);

} // namespace exosnap::engine
