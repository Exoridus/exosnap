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
#include <string>

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

} // namespace recorder_core
