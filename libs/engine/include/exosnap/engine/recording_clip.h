#pragma once

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/recording_media_types.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <windows.h>

namespace exosnap::engine {

[[nodiscard]] Outcome<size_t>
VisitRecordingClipDescriptors(const MediaIndexSnapshot& snapshot,
                              const std::function<bool(const RecordingMediaDescriptor&)>& visitor,
                              const std::function<bool(uint64_t)>& before_index_read = {});

struct ClipRapBoundary {
    SegmentId segment_id;
    uint64_t writer_generation = 0;
    uint64_t descriptor_revision = 0;
    uint64_t cluster_offset = 0;
    uint64_t block_offset = 0;
    int64_t pts_r_ns = 0;
    int64_t dts_r_ns = 0;
};

struct ClipPlannedSegment {
    SegmentId segment_id;
    uint64_t writer_generation = 0;
    uint64_t descriptor_revision = 0;
    FileIdentity file_identity;
    int64_t origin_r_ns = 0;
    uint64_t readable_end_offset = 0;
};

struct ClipBoundaryPlan {
    IndexSnapshotToken snapshot;
    RecordingId recording_id;
    int64_t requested_begin_r_ns = 0;
    int64_t requested_end_r_ns = 0;
    int64_t actual_begin_r_ns = 0;
    int64_t actual_end_r_ns = 0;
    int64_t read_begin_r_ns = 0;
    uint64_t required_audio_preroll_ns = 0;
    ClipRapBoundary start_rap;
    std::optional<ClipRapBoundary> end_rap;
    bool end_at_sealed_eos = false;
    std::vector<ClipPlannedSegment> segments;
};

// A frozen journal may extend the requested start backward and shorten its end to a proven GOP boundary.
// EOS is usable only with a matching successful close seal already present in the frozen journal.
[[nodiscard]] Outcome<ClipBoundaryPlan> PlanClipBoundaries(const MediaIndexSnapshot& snapshot, RecordingId recording_id,
                                                           int64_t requested_begin_r_ns, int64_t requested_end_r_ns,
                                                           std::span<const RecordingSegmentSeal> closed_segments = {},
                                                           const std::function<bool(uint64_t)>& before_index_read = {});

// Estimates retained descriptor storage, including allocated container capacity.
// Saturates on overflow so callers can reject before copying metadata.
[[nodiscard]] uint64_t EstimateRecordingClipDescriptorBytes(const RecordingMediaDescriptor& descriptor) noexcept;

struct ClipPinnedSource {
    std::shared_ptr<const RecordingMediaDescriptor> descriptor;
    HANDLE borrowed_handle = INVALID_HANDLE_VALUE;
    std::optional<AudioManifest1> audio_manifest;
};

struct ClipAudioBoundaryPlan {
    AudioTrackId track_id;
    std::optional<int64_t> first_copied_packet_r_ns;
    std::optional<int64_t> last_copied_packet_end_r_ns;
    std::optional<int64_t> first_audible_r_ns;
    std::optional<int64_t> last_audible_r_ns;
    std::optional<uint64_t> packet_duration_ns;
    std::optional<uint32_t> decoder_discard_samples;
};

struct ClipCopiedSourceRange {
    RecordingId recording_id;
    SegmentId segment_id;
    MediaId media_id;
    int64_t begin_r_ns = 0;
    int64_t end_r_ns = 0;
};

struct RecordingClipDraft {
    std::filesystem::path staging_path;
    uint64_t video_packets = 0;
    std::vector<uint64_t> audio_packets;
    int64_t actual_begin_r_ns = 0;
    int64_t actual_end_r_ns = 0;
    std::optional<AudioManifest1> output_manifest;
    std::vector<ClipAudioBoundaryPlan> audio_boundaries;
    uint64_t peak_packet_bytes = 0;
    uint64_t peak_writer_window_bytes = 0;
    uint64_t peak_cluster_payload_bytes = 0;
    std::vector<ClipCopiedSourceRange> source_ranges;
};

enum class ClipJobIoPhase { Reading, Remuxing, Finalizing };

struct ClipJobIoControl {
    std::function<bool(ClipJobIoPhase, uint64_t upcoming_bytes)> before_io;
    std::function<void(ClipJobIoPhase, uint64_t source_bytes_read, uint64_t staging_bytes_written)> on_progress;
    // Confirms that this attempt exclusively created and opened its staging file.
    std::function<void()> on_staging_created;
};

// Writes a finite, unpublished MKV from identity-pinned sources. The caller keeps the
// borrowed handles and staging reservation alive through return, then verifies and publishes.
[[nodiscard]] Outcome<RecordingClipDraft>
RunRecordingClipJob(const ClipBoundaryPlan& plan, const MediaIndexSnapshot& snapshot,
                    std::span<const ClipPinnedSource> sources, const std::filesystem::path& staging_path,
                    const std::atomic<bool>* cancelled = nullptr, const ClipJobIoControl* control = nullptr);

} // namespace exosnap::engine
