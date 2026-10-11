#pragma once

#include "../models/CompletedRecording.h"
#include "BackgroundStorageBudget.h"
#include "RecordingArtifactRegistry.h"

#include <exosnap/engine/recording_clip.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace exosnap {

enum class RecordingClipPhase {
    Planning,
    Review,
    Queued,
    Reading,
    Remuxing,
    Finalizing,
    VerifyingMetadata,
    Publishing,
    Completed,
    Cancelling,
    Cancelled,
    Failed
};

struct RecordingClipRequest {
    engine::MediaIndexSnapshot snapshot;
    int64_t requested_begin_r_ns = 0;
    int64_t requested_end_r_ns = 0;
    std::filesystem::path destination;
    std::vector<engine::RecordingSegmentSeal> closed_segments;
    std::vector<RecordingMarker> markers;
    int64_t requested_duration_seconds = 0;
    bool cfr = true;
};

struct RecordingClipJobSnapshot {
    engine::JobId job_id;
    uint64_t generation = 0;
    engine::RecordingId recording_id;
    RecordingClipPhase phase = RecordingClipPhase::Planning;
    int64_t requested_begin_r_ns = 0;
    int64_t requested_end_r_ns = 0;
    std::optional<int64_t> actual_begin_r_ns;
    std::optional<int64_t> actual_end_r_ns;
    uint64_t source_bytes_read = 0;
    uint64_t staging_bytes_written = 0;
    uint64_t reserved_bytes = 0;
    std::vector<std::string> track_names;
    std::optional<engine::EngineError> error;
    std::optional<engine::MediaId> asset_id;
    std::optional<CompletedRecording> completed;
    std::string review_reason;
    int64_t requested_duration_seconds = 0;
};

// Owns finite clip work independently of recording stop. Shutdown cancels and
// joins outside UI and registry locks before any borrowed dependencies die.
class RecordingClipJobs {
  public:
    using ChangedCallback = std::function<void(const RecordingClipJobSnapshot&)>;
    using PausePredicate = std::function<bool()>;
    using ReserveProvider = std::function<uint64_t()>;

    RecordingClipJobs(RecordingArtifactRegistry& registry, ChangedCallback changed, PausePredicate pause_background,
                      ReserveProvider primary_reserve, std::shared_ptr<BackgroundStorageBudget> storage = {});
    ~RecordingClipJobs();
    RecordingClipJobs(const RecordingClipJobs&) = delete;
    RecordingClipJobs& operator=(const RecordingClipJobs&) = delete;

    [[nodiscard]] engine::Outcome<engine::JobId> RequestPlan(RecordingClipRequest request);
    [[nodiscard]] bool Confirm(engine::JobId job_id, uint64_t generation);
    [[nodiscard]] bool Cancel(engine::JobId job_id, uint64_t generation);
    [[nodiscard]] std::vector<RecordingClipJobSnapshot> Snapshots() const;
    void BeginShutdown();
    void Shutdown();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace exosnap
