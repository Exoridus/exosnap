#pragma once

#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/recorder_session.h>

#include <array>

namespace exosnap::engine {

struct TimelineMediaSource {
    uint64_t id = 0;
    std::filesystem::path path;
    EditMediaMetadata metadata;
};

struct TimelineRenderSnapshot {
    TimelineSnapshot timeline;
    std::vector<TimelineMediaSource> sources;
};

enum class EditExportPath { StreamCopy, Render, Unsupported };
struct EditRenderPlan {
    EditExportPath path = EditExportPath::Unsupported;
    std::string reason;
    FrameSize output;
    int fps_num = 0;
    int fps_den = 1;
};

// Metadata and stream-copy eligibility are resolved before this pure policy.
EditRenderPlan ClassifyEditExport(const TimelineRenderSnapshot& snapshot,
                                  const TimelineExportAssessment& copy_assessment, bool to_mp4);

class EditTimelineReader {
  public:
    EditTimelineReader();
    ~EditTimelineReader();
    bool Open(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan, std::string& error);
    bool VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error);
    bool AudioAt(int64_t first_sample, uint32_t count, std::vector<float>& output, std::string& error);
    void ResetAudio();
    [[nodiscard]] size_t PeakDecoders() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TimelineRenderMeasurements {
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;
    uint64_t peak_encoder_backlog = 0;
    size_t peak_decoders = 0;
    uint64_t compositor_texture_creations = 0;
    double elapsed_ms = 0;
};

RemuxResult RenderEditTimeline(const TimelineRenderSnapshot& snapshot, const EditRenderPlan& plan,
                               RecorderConfig config, const std::filesystem::path& output, bool to_mp4,
                               RemuxProgressCallback progress, TimelineRenderMeasurements* measurements = nullptr);

} // namespace exosnap::engine
