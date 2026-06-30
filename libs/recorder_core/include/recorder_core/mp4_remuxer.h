#pragma once

// mp4_remuxer.h — stream-copy MKV → progressive MP4 with faststart
//
// Implements the first step of ExoSnap's "remux-first" architecture (ADR-0014):
// after a recording session ends, the transient MKV is remuxed to a progressive
// MP4 (moov-before-mdat) for broad compatibility and streaming support.
//
// Design constraints (CLAUDE.md):
//   - No Qt types — this header must be includable from pure recorder_core code.
//   - No re-encoding — stream copy only (no quality loss, near-zero CPU cost).
//   - No pipeline integration in this slice — the integration slice is separate.
//
// All tracks in the source file are forwarded to the output with no filtering.
// The output container is MP4 with +faststart (moov atom moved before mdat by
// libavformat's two-pass mechanism).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace recorder_core {

// Result of a remux operation.
struct RemuxResult {
    bool success = false;

    // On failure: libav error code (negative) and a human-readable message.
    // On success: both are zero/empty.
    int av_error_code = 0;
    std::string message;

    // Convenience factory methods.
    static RemuxResult Ok() {
        return {true, 0, {}};
    }
    static RemuxResult Fail(int av_err, std::string msg) {
        return {false, av_err, std::move(msg)};
    }
};

// Progress callback: called periodically during remux with a value in [0, 1].
// The fraction is estimated from the current output PTS relative to the known
// input duration. Monotonically non-decreasing. May be called from the calling
// thread (synchronous).
//
// Return false to request cooperative cancellation. The current packet will
// finish writing; cancellation is applied at the next opportunity.
using RemuxProgressCallback = std::function<bool(float progress)>;

// A null-callable sentinel: accepts all progress updates, never cancels.
inline RemuxProgressCallback RemuxNoopCallback() {
    return [](float) -> bool { return true; };
}

// ---------------------------------------------------------------------------
// TrimRange — optional start/end boundary for stream-copy remux operations.
//
// Both fields use the same unit as libavformat's AVFormatContext::duration:
// microseconds at AV_TIME_BASE (1,000,000 ticks per second).
//
// Sentinel: std::numeric_limits<int64_t>::min() (== AV_NOPTS_VALUE == INT64_MIN)
// means "no boundary" — the remux proceeds from the beginning / to the end.
//
// Trim is keyframe-accurate:
//   - start snaps backward to the nearest keyframe at or before start_us.
//   - all video packets with pts_us < end_us are copied (packets past end_us stop
//     the copy loop); other streams follow the video boundary.
// ---------------------------------------------------------------------------
struct TrimRange {
    static constexpr int64_t kNoTimestamp = std::numeric_limits<int64_t>::min();

    int64_t start_us = kNoTimestamp; ///< kNoTimestamp = from the beginning
    int64_t end_us = kNoTimestamp;   ///< kNoTimestamp = to the end

    [[nodiscard]] bool HasStart() const noexcept {
        return start_us != kNoTimestamp;
    }
    [[nodiscard]] bool HasEnd() const noexcept {
        return end_us != kNoTimestamp;
    }
};

// Trimmed overloads: re-remux only the segment [tr.start_us, tr.end_us).
// When tr.HasStart() and tr.HasEnd() are both false, behaviour is identical
// to the no-TrimRange overloads below.
RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb, TrimRange tr);

RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb, TrimRange tr);

// Scan `input_path` for all video keyframe PTS values and return them sorted
// in ascending order (microseconds at AV_TIME_BASE). The file is read without
// decoding (av_read_frame + key-frame flag only). Returns an empty vector on
// open failure or when the file has no video stream.
std::vector<int64_t> ExtractKeyframeTimestamps(const std::filesystem::path& input_path);

// Remux `input_path` (any container readable by libavformat, e.g. MKV) to
// `output_path` as a progressive MP4 with moov before mdat (+faststart).
//
// All streams (video + up to N audio tracks) are stream-copied without re-
// encoding. The codec_tag on each stream is cleared (set to 0) so libavformat
// picks the correct tag for the MP4 container.
//
// `progress_cb` is invoked with a fraction in [0, 1] after each interleaved
// write. Returning false from the callback requests cancellation. On cancel,
// the partial output file is removed and RemuxResult::success is false.
//
// If `output_path` already exists it is overwritten.
//
// Thread safety: the function itself is not thread-safe; call from one thread.
// The progress_cb is invoked synchronously on the same thread.
RemuxResult RemuxToProgressiveMp4(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                                  RemuxProgressCallback progress_cb = RemuxNoopCallback());

// Remux `input_path` to `output_path` as a Matroska (MKV) file via
// libavformat stream-copy (matroska muxer). The mkv muxer writes Cues,
// SeekHead, and Duration in the trailer, producing a seekable, well-formed
// output even when the input is a truncated or crash-interrupted file.
//
// Behaviour matches RemuxToProgressiveMp4 (cancel deletes output, progress
// callback semantics identical). Use for the "Keep as MKV" recovery path
// (ADR-0014) when the artefact is not finalized=true.
RemuxResult RemuxToMkv(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
                       RemuxProgressCallback progress_cb = RemuxNoopCallback());

} // namespace recorder_core
