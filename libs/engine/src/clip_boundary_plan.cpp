#include "recording_media_index.h"
#include <exosnap/engine/recording_clip.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {

using SegmentKey = std::tuple<SegmentId, uint64_t, uint64_t>;
constexpr size_t kPlanSummaryBudgetBytes = 1024 * 1024;

struct SegmentSummary {
    SegmentKey key;
    FileIdentity identity;
    int64_t origin_r_ns = 0;
    uint64_t continuity_group = 0;
    uint64_t readable_end = 0;
    uint64_t last_commit = 0;
    uint64_t max_audio_preroll_ns = 0;
    size_t track_count = 0;
    bool audio_authority_complete = true;
    std::array<uint64_t, 9> opus_start_gap_ns{};
    std::array<uint64_t, 9> opus_packet_duration_ns{};
    std::array<std::optional<int64_t>, 9> first_covered_r;
    std::array<std::optional<int64_t>, 9> last_covered_r;
};

constexpr size_t kSummaryAllocationBytes = 2 * sizeof(SegmentSummary) + 2 * sizeof(ClipPlannedSegment) +
                                           sizeof(std::map<SegmentKey, size_t>::value_type) + 4 * sizeof(void*);

EngineError PlanError(EngineErrorCode code, const char* message) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Clip;
    error.context = message;
    return error;
}

// Whole priming packets can precede the first audible packet at the source origin.
// A gap is admissible only when indexed coverage agrees with decoder priming authority.
bool IsProvenOpusStartupGap(const SegmentSummary& segment, size_t track, int64_t r_ns) {
    return r_ns == segment.origin_r_ns && segment.first_covered_r[track] && *segment.first_covered_r[track] > r_ns &&
           segment.opus_packet_duration_ns[track] != 0 &&
           static_cast<uint64_t>(*segment.first_covered_r[track]) - static_cast<uint64_t>(r_ns) ==
               segment.opus_start_gap_ns[track];
}

bool Covers(const SegmentSummary& segment, int64_t r_ns) {
    for (size_t track = 0; track < segment.track_count; ++track) {
        if (!segment.first_covered_r[track] || !segment.last_covered_r[track] || *segment.last_covered_r[track] < r_ns)
            return false;
        if (*segment.first_covered_r[track] > r_ns &&
            (!IsProvenOpusStartupGap(segment, track, r_ns) ||
             segment.opus_start_gap_ns[track] > segment.opus_packet_duration_ns[track]))
            return false;
    }
    return true;
}

bool HasUnsupportedOpusStartupGap(const SegmentSummary& segment, int64_t r_ns) {
    for (size_t track = 1; track < segment.track_count; ++track)
        if (IsProvenOpusStartupGap(segment, track, r_ns) &&
            segment.opus_start_gap_ns[track] > segment.opus_packet_duration_ns[track])
            return true;
    return false;
}

bool CoversThrough(const SegmentSummary& segment, int64_t r_ns) {
    for (size_t track = 0; track < segment.track_count; ++track)
        if (!segment.last_covered_r[track] || *segment.last_covered_r[track] < r_ns)
            return false;
    return true;
}

ClipRapBoundary Boundary(const SegmentSummary& segment, const MediaRandomAccessPoint& rap, int64_t pts_r_ns,
                         int64_t dts_r_ns) {
    return {std::get<0>(segment.key),
            std::get<1>(segment.key),
            std::get<2>(segment.key),
            rap.cluster_offset,
            rap.block_offset,
            pts_r_ns,
            dts_r_ns};
}

} // namespace

Outcome<ClipBoundaryPlan> PlanClipBoundaries(const MediaIndexSnapshot& snapshot, RecordingId recording_id,
                                             int64_t requested_begin_r_ns, int64_t requested_end_r_ns,
                                             std::span<const RecordingSegmentSeal> closed_segments,
                                             const std::function<bool(uint64_t)>& before_index_read) {
    if (recording_id == RecordingId{} || snapshot.token.recording_id != recording_id || requested_begin_r_ns < 0 ||
        requested_end_r_ns <= requested_begin_r_ns || snapshot.journal_end == 0)
        return PlanError(EngineErrorCode::InvalidConfig, "Invalid frozen clip request or recording identity");

    std::vector<SegmentSummary> segments;
    std::map<SegmentKey, size_t> by_key;
    std::optional<size_t> current;
    bool invalid = false;
    bool budget_exceeded = false;
    size_t summary_bytes = 0;
    const auto summarized = RecordingMediaIndex::Visit(
        snapshot,
        [&](const RecordingMediaDescriptor& descriptor) {
            if (descriptor.recording_id != recording_id || descriptor.audio_tracks.size() > 8) {
                invalid = true;
                return false;
            }
            SegmentSummary segment;
            segment.key = {descriptor.segment_id, descriptor.writer_generation, descriptor.descriptor_revision};
            segment.identity = descriptor.file_identity;
            segment.origin_r_ns = descriptor.segment_origin_r_ns;
            segment.continuity_group = descriptor.continuity_group;
            segment.track_count = descriptor.audio_tracks.size() + 1;
            for (size_t index = 0; index < descriptor.audio_tracks.size(); ++index) {
                const auto& track = descriptor.audio_tracks[index];
                if (track.codec.codec_id == "A_OPUS") {
                    if (track.encoder_reset && track.codec.sample_rate == 48000 && track.packet_duration_ns &&
                        *track.packet_duration_ns <= (std::numeric_limits<uint64_t>::max)() / 48000 &&
                        track.codec.codec_private.size() >= 19) {
                        const uint64_t samples = *track.packet_duration_ns * 48000 / 1000000000ULL;
                        const uint32_t delay = static_cast<uint32_t>(track.codec.codec_private[10]) |
                                               (static_cast<uint32_t>(track.codec.codec_private[11]) << 8);
                        if (samples != 0 && delay == track.codec.codec_delay_samples &&
                            samples * 1000000000ULL / 48000 == *track.packet_duration_ns) {
                            segment.opus_packet_duration_ns[index + 1] = *track.packet_duration_ns;
                            segment.opus_start_gap_ns[index + 1] = delay / samples * *track.packet_duration_ns;
                        }
                    }
                    if (!track.preroll_ns || !track.packet_duration_ns ||
                        (!track.encoder_reset && !track.first_decoded_sample_r))
                        segment.audio_authority_complete = false;
                    else
                        segment.max_audio_preroll_ns = (std::max)(segment.max_audio_preroll_ns, *track.preroll_ns);
                } else if (track.codec.codec_id == "A_AAC" && track.codec.codec_delay_samples != 0 &&
                           !track.first_decoded_sample_r) {
                    segment.audio_authority_complete = false;
                }
            }
            if (kSummaryAllocationBytes > kPlanSummaryBudgetBytes - summary_bytes) {
                budget_exceeded = true;
                return false;
            }
            try {
                auto [it, inserted] = by_key.emplace(segment.key, segments.size());
                if (!inserted) {
                    invalid = true;
                    return false;
                }
                current = segments.size();
                segments.push_back(std::move(segment));
            } catch (const std::bad_alloc&) {
                budget_exceeded = true;
                return false;
            }
            summary_bytes += kSummaryAllocationBytes;
            return true;
        },
        [&](const CommittedMediaReceipt& receipt) {
            if (!current || receipt.recording_id != recording_id ||
                receipt.segment_id != std::get<0>(segments[*current].key) ||
                receipt.writer_generation != std::get<1>(segments[*current].key) ||
                receipt.descriptor_revision != std::get<2>(segments[*current].key)) {
                invalid = true;
                return false;
            }
            auto& segment = segments[*current];
            segment.last_commit = receipt.commit_sequence;
            segment.readable_end = receipt.readable_end_offset;
            for (const auto& cluster : receipt.clusters) {
                for (const auto& coverage : cluster.coverage) {
                    if (coverage.track_number == 0 || coverage.track_number > segment.track_count) {
                        invalid = true;
                        return false;
                    }
                    const size_t track = static_cast<size_t>(coverage.track_number - 1);
                    const auto begin = SegmentLocalToRecordingNs(segment.origin_r_ns, coverage.begin_pts_ns);
                    const auto end = SegmentLocalToRecordingNs(segment.origin_r_ns, coverage.end_pts_ns);
                    if (!begin || !end || *end < *begin) {
                        invalid = true;
                        return false;
                    }
                    auto& first = segment.first_covered_r[track];
                    auto& last = segment.last_covered_r[track];
                    first = first ? (std::min)(*first, *begin) : *begin;
                    last = last ? (std::max)(*last, *end) : *end;
                }
            }
            return true;
        },
        before_index_read);
    if (budget_exceeded)
        return PlanError(EngineErrorCode::ResourceBudgetExceeded, "Clip plan summary budget exceeded");
    if (!summarized.succeeded())
        return *summarized.error();
    if (invalid || segments.empty())
        return PlanError(EngineErrorCode::SourceIntegrityFailure, "Invalid segment identity or coverage in snapshot");

    std::optional<std::pair<ClipRapBoundary, size_t>> start;
    bool unsupported_start_gap = false;
    std::optional<std::pair<ClipRapBoundary, size_t>> end;
    current.reset();
    invalid = false;
    const auto scanned = RecordingMediaIndex::Visit(
        snapshot,
        [&](const RecordingMediaDescriptor& descriptor) {
            const SegmentKey key{descriptor.segment_id, descriptor.writer_generation, descriptor.descriptor_revision};
            const auto found = by_key.find(key);
            if (found == by_key.end()) {
                invalid = true;
                return false;
            }
            current = found->second;
            return true;
        },
        [&](const CommittedMediaReceipt& receipt) {
            if (!current) {
                invalid = true;
                return false;
            }
            const auto& segment = segments[*current];
            for (const auto& cluster : receipt.clusters) {
                for (const auto& rap : cluster.random_access_points) {
                    if (rap.track_number != 1 || rap.kind != RandomAccessKind::ClosedGop)
                        continue;
                    const auto pts = SegmentLocalToRecordingNs(segment.origin_r_ns, rap.pts_ns);
                    const auto dts = SegmentLocalToRecordingNs(segment.origin_r_ns, rap.dts_ns);
                    if (!pts || !dts || *dts > *pts) {
                        invalid = true;
                        return false;
                    }
                    const auto boundary = Boundary(segment, rap, *pts, *dts);
                    if (*pts <= requested_begin_r_ns && HasUnsupportedOpusStartupGap(segment, *pts))
                        unsupported_start_gap = true;
                    if (*pts <= requested_begin_r_ns && Covers(segment, *pts) &&
                        (!start || start->first.pts_r_ns <= *pts))
                        start = std::pair{boundary, *current};
                    if (*pts <= requested_end_r_ns && CoversThrough(segment, *pts) &&
                        (!end || end->first.pts_r_ns <= *pts))
                        end = std::pair{boundary, *current};
                }
            }
            return true;
        },
        before_index_read);
    if (!scanned.succeeded())
        return *scanned.error();
    if (invalid)
        return PlanError(EngineErrorCode::SourceIntegrityFailure, "Invalid random-access timestamp in snapshot");
    if (!start && unsupported_start_gap)
        return PlanError(EngineErrorCode::UnsupportedFormat,
                         "Opus startup gap exceeds one packet at the first video access");
    if (!start)
        return PlanError(EngineErrorCode::IndexNotReady, "No proven closed access at or before requested start");

    bool sealed_eos = false;
    for (const auto& seal : closed_segments) {
        if (seal.recording_id != recording_id || seal.final_commit_sequence > snapshot.token.committed_through ||
            seal.measured_video_end_r_ns > requested_end_r_ns)
            continue;
        const SegmentKey key{seal.segment_id, seal.writer_generation, seal.descriptor_revision};
        const auto found = by_key.find(key);
        if (found == by_key.end())
            continue;
        const auto& segment = segments[found->second];
        if (segment.last_commit != seal.final_commit_sequence || segment.readable_end != seal.final_readable_end ||
            !segment.last_covered_r[0] || *segment.last_covered_r[0] != seal.measured_video_end_r_ns ||
            !CoversThrough(segment, seal.measured_video_end_r_ns))
            continue;
        if (!end || seal.measured_video_end_r_ns > end->first.pts_r_ns) {
            end.reset();
            sealed_eos = true;
            ClipRapBoundary eos;
            eos.segment_id = seal.segment_id;
            eos.writer_generation = seal.writer_generation;
            eos.descriptor_revision = seal.descriptor_revision;
            eos.pts_r_ns = seal.measured_video_end_r_ns;
            end = std::pair{eos, found->second};
        }
    }
    if (!end || end->first.pts_r_ns <= start->first.pts_r_ns || end->second < start->second)
        return PlanError(EngineErrorCode::IndexNotReady, "No complete GOP end is available in the frozen snapshot");

    const auto& start_segment = segments[start->second];
    if (!start_segment.audio_authority_complete)
        return PlanError(EngineErrorCode::MetadataIncomplete, "Audio priming authority is missing at clip start");
    const int64_t actual_begin = start->first.pts_r_ns;
    const int64_t actual_end = end->first.pts_r_ns;
    if (start_segment.max_audio_preroll_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        return PlanError(EngineErrorCode::InvalidConfig, "Audio preroll exceeds the signed timeline");
    const int64_t preroll = static_cast<int64_t>(start_segment.max_audio_preroll_ns);
    const int64_t read_begin =
        (std::max)(start_segment.origin_r_ns, actual_begin < preroll ? 0 : actual_begin - preroll);

    ClipBoundaryPlan plan;
    plan.snapshot = snapshot.token;
    plan.recording_id = recording_id;
    plan.requested_begin_r_ns = requested_begin_r_ns;
    plan.requested_end_r_ns = requested_end_r_ns;
    plan.actual_begin_r_ns = actual_begin;
    plan.actual_end_r_ns = actual_end;
    plan.read_begin_r_ns = read_begin;
    plan.required_audio_preroll_ns = start_segment.max_audio_preroll_ns;
    plan.start_rap = start->first;
    plan.end_at_sealed_eos = sealed_eos;
    if (!sealed_eos)
        plan.end_rap = end->first;

    const size_t last_included = end->second;
    const bool boundary_only =
        !sealed_eos && last_included > start->second && segments[last_included].first_covered_r[0] == actual_end;
    try {
        plan.segments.reserve(last_included - start->second + 1);
    } catch (const std::bad_alloc&) {
        return PlanError(EngineErrorCode::ResourceBudgetExceeded, "Clip segment plan allocation failed");
    }
    for (size_t index = start->second; index <= last_included; ++index) {
        const auto& segment = segments[index];
        if (segment.continuity_group != start_segment.continuity_group || !segment.audio_authority_complete)
            return PlanError(EngineErrorCode::UnsupportedFormat, "Selected segments have incompatible continuity");
        const int64_t boundary =
            index == last_included ? actual_end : segments[index + 1].first_covered_r[0].value_or(actual_end);
        if (!CoversThrough(segment, boundary))
            return PlanError(EngineErrorCode::IndexNotReady,
                             "A selected track has no committed coverage to the GOP end");
        plan.segments.push_back({std::get<0>(segment.key), std::get<1>(segment.key), std::get<2>(segment.key),
                                 segment.identity, segment.origin_r_ns, segment.readable_end,
                                 boundary_only && index == last_included});
    }
    return plan;
}

} // namespace exosnap::engine
