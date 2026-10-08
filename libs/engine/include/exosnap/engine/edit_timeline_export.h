#pragma once

#include "exosnap/engine/mp4_remuxer.h"

namespace exosnap::engine {

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
    std::string error;
};

EditMediaMetadata ProbeEditMedia(const std::filesystem::path& source);

struct TimelineExportClip {
    std::filesystem::path source;
    int64_t source_in_us = 0;
    int64_t source_out_us = 0;
    int64_t timeline_start_us = 0;
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
TimelineExportAssessment AssessTimelineExport(const std::vector<TimelineExportClip>& clips, bool to_mp4);

// Synchronous and independent of mutable workspace state. Output must be a new
// staging path, never a source. Failed/cancelled exports remove that staging file.
RemuxResult ExportTimelineStreamCopy(const std::vector<TimelineExportClip>& clips, const std::filesystem::path& output,
                                     bool to_mp4, RemuxProgressCallback progress_cb = RemuxNoopCallback());

} // namespace exosnap::engine
