#pragma once

#include "exosnap/engine/audio_media_manifest.h"
#include "exosnap/engine/mp4_remuxer.h"
#include <exosnap/engine/edit_types.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace exosnap::engine {

struct EditSourceStream {
    uint32_t container_index = 0;
    TimelineStreamKind kind = TimelineStreamKind::Video;
    std::optional<AudioTrackId> stream_identity;
    MediaCodecDescriptor codec;
    int time_base_num = 0;
    int time_base_den = 1;
    std::optional<int64_t> start_us;
    std::optional<int64_t> duration_us;
    int width = 0;
    int height = 0;
    int fps_num = 0;
    int fps_den = 1;
};

struct EditMediaMetadata {
    int64_t duration_us = 0;
    int width = 0;
    int height = 0;
    double fps = 0;
    int fps_num = 0;
    int fps_den = 1;
    bool render_color_supported = false;
    std::string render_color_reason;
    bool has_audio = false;
    AudioMetadataReadResult audio_metadata;
    std::string video_codec;
    FileIdentity file_identity;
    uint64_t file_size = 0;
    std::array<uint8_t, 32> content_sha256{};
    bool content_identity_valid = false;
    // Common container origin. Stream start_us values are relative to it.
    std::optional<int64_t> timestamp_origin_us;
    std::vector<EditSourceStream> streams;
    std::string error;
};

// Provisional metadata has no content-addressed cache or export authority.
EditMediaMetadata ProbeEditMedia(const std::shared_ptr<const MediaReadCapability>& source,
                                 const std::function<bool()>& continue_io = {},
                                 const BackgroundIoAdmission& before_io = {},
                                 const std::atomic<bool>* cancelled = nullptr, bool verify_content = true);

EditMediaMetadata ProbeEditMedia(const std::filesystem::path& source, const std::function<bool()>& continue_io = {},
                                 const BackgroundIoAdmission& before_io = {},
                                 const std::atomic<bool>* cancelled = nullptr);

// Explicitly adopts independently verified bytes as a new source identity. Stale
// external metadata loses authority. Existing expected-identity probes stay unchanged.
EditMediaMetadata ProbeEditMediaForAdoption(const std::shared_ptr<const MediaReadCapability>& source,
                                            const std::shared_ptr<const VerifiedMediaIdentity>& observed_proof,
                                            const std::function<bool()>& continue_io = {},
                                            const BackgroundIoAdmission& before_io = {},
                                            const std::atomic<bool>* cancelled = nullptr);
struct TimelineExportClip {
    std::filesystem::path source;
    int64_t source_in_us = 0;
    int64_t source_out_us = 0;
    int64_t timeline_start_us = 0;
    std::shared_ptr<const MediaReadCapability> access;
    std::shared_ptr<const VerifiedMediaIdentity> identity_proof;
};

enum class TimelineExportEligibility { StreamCopy, RenderRequired, Invalid };

struct TimelineExportAssessment {
    TimelineExportEligibility eligibility = TimelineExportEligibility::Invalid;
    std::string reason;
};

// The recipe contains one video layer and its linked source audio. The caller
// must reject independent audio edits, compositing and transitions before calling.
// Single-source trims retain TrimRange's keyframe accuracy contract. Multiple
// sources currently require complete, compatible streams without gaps.
TimelineExportAssessment AssessTimelineExport(const std::vector<TimelineExportClip>& clips, bool to_mp4,
                                              const std::atomic<bool>* cancelled = nullptr);

// Synchronous and independent of mutable workspace state. Output must be a new
// staging path, never a source. Failed/cancelled exports remove that staging file.
RemuxResult ExportTimelineStreamCopy(const std::vector<TimelineExportClip>& clips, const std::filesystem::path& output,
                                     bool to_mp4, RemuxProgressCallback progress_cb = RemuxNoopCallback());

} // namespace exosnap::engine
