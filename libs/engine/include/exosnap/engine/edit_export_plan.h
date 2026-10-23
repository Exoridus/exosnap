#pragma once

#include <exosnap/engine/edit_timeline_render.h>

#include <array>
#include <atomic>
#include <memory>
#include <utility>

namespace exosnap::engine {

enum class EditExportMode { FinishedStereo, OriginalStreams };
enum class EditExportSpan { ResultRange, EntireTimeline, OccupiedContent, TimeRange, SelectedClips };
enum class EditExportContainer { Matroska, Mp4 };
enum class EditVideoProfile { MatchSourceAuto, H264, Hevc };
enum class EditAudioProfile { Pcm24Stereo, AacLc192Stereo };
enum class EditVideoPath { Copy, Render, Unsupported };
enum class EditAudioPath { Absent, CopyStereo, RenderStereo, CopyOriginalStreams, Unsupported };
enum class EditExportIssueCode {
    EmptySpan,
    InvalidTimeline,
    MissingSource,
    SourceChanged,
    MixSelectionRequired,
    OriginalRecipeChanged,
    UnsupportedProfile,
    UnsupportedAudioLayout,
    BoundaryConsentRequired,
    ResourceLimitExceeded,
    DestinationAliasesSource,
    InvalidSelection,
    PartialJunctionOwnership
};
enum class EditExportIssueScope { Timeline, Source, Video, Audio, Destination };
struct EditExportIssue {
    EditExportIssueCode code = EditExportIssueCode::InvalidTimeline;
    EditExportIssueScope scope = EditExportIssueScope::Timeline;
    std::vector<uint64_t> affected_ids;
};

using EditPlanHash = std::array<uint8_t, 32>;
struct EditBoundaryDeviation {
    uint64_t clip = 0;
    int64_t requested_in_us = 0;
    int64_t requested_out_us = 0;
    int64_t effective_in_us = 0;
    int64_t effective_out_us = 0;
    bool exposes_removed_content = false;
    bool operator==(const EditBoundaryDeviation&) const = default;
};
struct EditBoundaryConsent {
    EditPlanHash plan_hash{};
    std::vector<EditBoundaryDeviation> edges;
};
struct EditExportIntent {
    EditExportMode mode = EditExportMode::FinishedStereo;
    EditExportSpan span = EditExportSpan::ResultRange;
    EditExportContainer container = EditExportContainer::Matroska;
    EditVideoProfile video_profile = EditVideoProfile::MatchSourceAuto;
    EditAudioProfile audio_profile = EditAudioProfile::Pcm24Stereo;
    FrameSize geometry;
    uint32_t fps_num = 0;
    uint32_t fps_den = 1;
    uint32_t quality = 0;
    std::filesystem::path destination;
    bool replace_destination = false;
    int64_t selection_in_us = 0;
    int64_t selection_out_us = 0;
    std::vector<uint64_t> selected_clip_ids;
};
struct EditOriginalGroup {
    uint64_t group = 0;
    std::vector<TimelineStreamRef> streams;
};

// Supplied by source/capability assessment, never inferred from an extension or
// keyframe flag. An unqualified execution path remains unavailable.
struct EditExportFacts {
    bool video_copy_qualified = false;
    bool original_copy_qualified = false;
    bool stereo_copy_qualified = false;
    bool video_render_available = false;
    bool stereo_render_available = false;
    bool mp4_finished_available = false;
    bool input_mix_validated = false;
    bool destination_admitted = false;
    std::vector<EditOriginalGroup> original_groups;
    std::vector<EditBoundaryDeviation> deviations;
    std::vector<TimelineExportClip> copy_recipe;
};
struct EditExportPlan {
    uint64_t revision = 0;
    EditExportIntent intent;
    TimelineRenderSnapshot snapshot;
    EditExportFacts facts;
    EditPlanHash hash{};
    int64_t span_in_us = 0;
    int64_t span_out_us = 0;
    EditVideoPath video_path = EditVideoPath::Unsupported;
    EditAudioPath audio_path = EditAudioPath::Unsupported;
    uint32_t output_audio_streams = 0;
    std::vector<EditExportIssue> issues;
    [[nodiscard]] bool ready() const {
        return issues.empty();
    }
};

// Pure policy over independently validated facts. The returned snapshot owns
// its values and has no monitoring selection, solo state or preview volume.
[[nodiscard]] std::shared_ptr<const EditExportPlan> PrepareEditExport(TimelineRenderSnapshot snapshot,
                                                                      EditExportIntent intent, EditExportFacts facts);
[[nodiscard]] bool AcceptsBoundaryConsent(const EditExportPlan& plan, const EditBoundaryConsent& consent);

// Freezes selected contributions without changing source-time offsets. Partial
// junction ownership remains a blocking issue when the plan is prepared.
[[nodiscard]] TimelineRenderSnapshot SelectEditExportSnapshot(TimelineRenderSnapshot snapshot,
                                                              const EditExportIntent& intent);
[[nodiscard]] std::pair<int64_t, int64_t> EditExportBounds(const TimelineSnapshot& timeline,
                                                           const EditExportIntent& intent);

// Opens the snapshot's sources to establish which execution paths are actually
// available. Blocking; run off the UI thread. `input_mix_validated` is the
// caller's statement that every contributing source has a resolved starting mix.
[[nodiscard]] EditExportFacts AssessEditExportFacts(const TimelineRenderSnapshot& snapshot,
                                                    const EditExportIntent& intent, bool input_mix_validated,
                                                    const std::atomic<bool>* cancelled = nullptr);

// Writes a ready plan to a new staging path; failure and cancellation remove it.
// `config` supplies the shipping video encoder for a rendered video path.
RemuxResult ExecuteEditExport(const EditExportPlan& plan, RecorderConfig config, const std::filesystem::path& staging,
                              RemuxProgressCallback progress);

} // namespace exosnap::engine
