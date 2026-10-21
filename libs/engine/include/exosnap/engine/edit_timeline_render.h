#pragma once

#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/recorder_session.h>

#include <array>
#include <atomic>

namespace exosnap::engine {

struct TimelineMediaSource {
    uint64_t id = 0;
    std::filesystem::path path;
    EditMediaMetadata metadata;
    std::optional<MonitorSelectionSnapshot> monitor_selection;
    std::shared_ptr<const MediaReadCapability> access;
    std::shared_ptr<const VerifiedMediaIdentity> identity_proof;
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
    // Rendered timeline interval; a negative out renders through the timeline end.
    int64_t span_in_us = 0;
    int64_t span_out_us = -1;
    // AAC-LC instead of PCM24 for the result mix, for the MP4 delivery profile.
    bool aac_audio = false;
};

struct TimelineVideoAdmission {
    // Most enabled video clips visible at one instant of the assessed interval.
    size_t peak_sources = 0;
    // Most summed source pixels visible at one instant of the assessed interval.
    uint64_t peak_pixels = 0;
    // A source without probed geometry counts as unadmitted.
    bool admitted = true;
};

// Peak decoder and composition demand of [in_us, out_us), evaluated before any
// decoder opens. Unadmitted demand exceeds kMaxTimelineVideoSources or
// kMaxTimelineVideoSourcePixels and must be refused while planning.
[[nodiscard]] TimelineVideoAdmission AssessTimelineVideoAdmission(const TimelineRenderSnapshot& snapshot, int64_t in_us,
                                                                  int64_t out_us);

// Metadata and stream-copy eligibility are resolved before this pure policy.
EditRenderPlan ClassifyEditExport(const TimelineRenderSnapshot& snapshot,
                                  const TimelineExportAssessment& copy_assessment, bool to_mp4);

struct TimelineSourceAudio {
    uint64_t clip = 0;
    uint64_t asset = 0;
    uint64_t track = 0;
    int64_t first_timeline_sample = 0;
    std::vector<float> interleaved_stereo;
};

struct TimelineAudioReadMeasurements {
    uint64_t decoder_opens = 0;
    uint64_t cursor_reuses = 0;
    uint64_t prepared_source_ranges = 0;
    uint64_t prepared_cache_hits = 0;
    uint64_t seeks = 0;
    uint64_t decoded_frames = 0;
};
class EditTimelineReader {
  public:
    using VideoOpenAdmission = std::function<bool(const std::atomic<bool>&)>;
    EditTimelineReader();
    ~EditTimelineReader();
    bool Open(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan, std::string& error,
              const std::atomic<bool>* cancelled = nullptr);
    // Retains compatible source/stream cursors and repositions changed mappings.
    // Presentation-only revisions do not reposition decoders.
    bool UpdateSnapshot(TimelineRenderSnapshot snapshot, const EditRenderPlan& plan, std::string& error);
    // Cumulative since Open, including retired video cursors. Single-owner read.
    [[nodiscard]] VideoReadMeasurements VideoReadStats() const noexcept;
    [[nodiscard]] uint64_t VideoDecoderOpens() const noexcept;
    [[nodiscard]] std::optional<AudioMetadataReadResult> VideoAudioMetadata(uint64_t asset) const;
    // Cumulative audio I/O work since Open, including retired cursors. Single-owner read.
    [[nodiscard]] TimelineAudioReadMeasurements AudioReadStats() const noexcept;
    bool VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error);
    bool VideoAt(int64_t time_us, TimelineVideoFrame& output, std::string& error, VideoReadMode mode,
                 uint64_t generation = 0, const std::atomic<bool>* cancelled = nullptr,
                 const VideoOpenAdmission& before_open = {});
    // Validates through the same held video source decoder on this reader's owner.
    bool ConfirmMonitorSelection(uint64_t asset, int64_t time_us, const MonitorSelectionSnapshot& selection,
                                 AudioMetadataReadResult& metadata, std::string& error,
                                 const std::atomic<bool>* cancelled = nullptr,
                                 const VideoOpenAdmission& before_open = {});
    bool AudioAt(int64_t first_sample, uint32_t count, std::vector<float>& output, std::string& error);
    // Includes suppressed contributions and applies no timeline gain or limiter.
    bool SourceAudioAt(int64_t first_sample, uint32_t count, std::vector<TimelineSourceAudio>& output,
                       std::string& error);
    // Best effort within the existing bounded decoder pool. Active sources win.
    bool PrepareAudioLookahead(int64_t first_sample, int64_t through_sample, std::string& error,
                               uint64_t* deferred = nullptr);
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

// Checks every video packet without accumulating payloads. Returns false for missing
// timestamps, non-monotonic timeline DTS, unreadable sources or cancellation.
[[nodiscard]] bool HasUsableCopiedVideoTimestamps(const std::vector<TimelineExportClip>& recipe,
                                                  const std::atomic<bool>* cancelled = nullptr);

// Copies the video packets of complete, compatible sources that cover the whole
// timeline from zero and renders the result mix as 48 kHz stereo PCM beside
// them. Matroska only. Fails rather than expanding or trimming copied video.
RemuxResult ExportEditTimelineWithCopiedVideo(const TimelineRenderSnapshot& snapshot,
                                              const std::vector<TimelineExportClip>& video_recipe,
                                              const std::filesystem::path& output, bool render_audio, bool aac_audio,
                                              RemuxProgressCallback progress);

} // namespace exosnap::engine
