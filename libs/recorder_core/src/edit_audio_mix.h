#pragma once

// Timestamp-aligned mix of the Edit-page player's audio tracks
// (docs/superpowers/specs/2026-08-02-timeline-thumbnails-multitrack-design.md,
// section 3).
//
// Kept here, separate from the FFmpeg-facing engine, so the alignment and the
// overload behaviour are unit-testable without a real file or a real audio
// device -- the same boundary edit_playback_pacing.h draws for the pacing
// predicates.

#include "brickwall_limiter.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recorder_core {

// Sums the decoded blocks of several audio tracks of one recording into a
// single stream in the player's fixed playback format (48 kHz stereo
// interleaved float32).
//
// Alignment by timestamp, not by arrival: a recording with system and
// microphone sound carries two independently encoded tracks whose blocks reach
// the decoder in container order and at different lengths, so adding "the
// block that just arrived" to "the block before it" would smear one track
// against the other. Every block is instead added into an accumulator at the
// sample frame its own timestamp names, and only the stretch no track can
// still write into is handed on.
//
// Overload is handled by a peak limiter rather than by dividing by the track
// count. Dividing would make a recording whose second track happens to be
// silent half as loud as the same recording with one track -- the level of
// what you actually hear would then depend on how many quiet tracks exist,
// which is not something a listener can be expected to reason about. The
// limiter is transparent while the sum stays under the ceiling (its envelope
// sits at unity and samples pass through bit-identical) and only bends the
// peaks that would otherwise clip.
//
// Not thread-safe: one instance belongs to one playback run's audio thread.
class EditAudioMixer {
  public:
    // The playback output format, matching DecodedAudioBlock's contract.
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;

    // How far behind the leading track a lagging one may fall before the mix
    // stops waiting for it. Without this floor a track that simply ENDS -- a
    // source that stopped before the video did, a track with a hole in it --
    // would hold the whole mix at its last timestamp for the rest of the clip:
    // nothing would be handed on, the renderer's ring would run dry, and the
    // audio clock would keep advancing over silence.
    //
    // The value has to exceed the container's own cross-track interleave
    // depth, or a track that is merely a little behind would lose samples
    // instead of being waited for. For the recordings this player opens that
    // depth is effectively zero -- the muxer drains its reorder window in
    // global timestamp order, so blocks of both tracks at the same instant sit
    // adjacent in the file. 250 ms is far above that and still well inside the
    // demuxer's one-second read-ahead, so holding it back cannot starve the
    // renderer.
    static constexpr uint32_t kMaxTrackLagFrames = kSampleRate / 4;

    struct MixedBlock {
        int64_t pts_us = 0;
        uint32_t frame_count = 0; // sample frames (kChannels floats each)
        std::vector<float> interleaved_stereo;
    };

    EditAudioMixer() = default;

    // Starts a fresh run over `track_count` tracks whose first output sample
    // sits at start_us. Clears every buffer and the limiter envelope.
    void Reset(size_t track_count, int64_t start_us);

    // Adds one track's decoded block. `track` indexes the tracks Reset()
    // announced; anything out of range, empty or null is ignored.
    void Submit(size_t track, int64_t pts_us, const float* interleaved_stereo, size_t frame_count);

    // The stretch the mix is complete through, or nullopt when nothing is
    // ready yet. Call repeatedly after Submit until it returns nullopt.
    [[nodiscard]] std::optional<MixedBlock> Take();

    // Everything still buffered, whether or not every track has reached it.
    // For end of stream, where nothing more can arrive by definition.
    [[nodiscard]] std::optional<MixedBlock> TakeRemainder();

    // Sample frames discarded because they arrived after the mix had already
    // moved past their timestamp. Diagnostic: a lookbehind too tight for a
    // file shows up here rather than only as something quietly missing.
    [[nodiscard]] uint64_t LateFramesDropped() const noexcept {
        return late_frames_dropped_;
    }

  private:
    // Removes the first `frames` sample frames from the accumulator, limits
    // them and returns them as a block. Advances the accumulator's origin.
    [[nodiscard]] std::optional<MixedBlock> TakeFrames(size_t frames);

    size_t track_count_ = 0;
    // Absolute sample-frame index of accum_'s first frame, and per track the
    // absolute index one past the last frame that track has delivered.
    //
    // Positions are kept in sample frames rather than microseconds because a
    // block boundary converted back and forth on every hand-off accumulates a
    // rounding error, which over a long recording drifts the tracks against
    // each other by exactly the amount this class exists to prevent.
    int64_t origin_frame_ = 0;
    std::vector<int64_t> track_through_frame_;
    std::vector<float> accum_; // interleaved, kChannels floats per frame
    BrickwallLimiter limiter_;
    uint64_t late_frames_dropped_ = 0;
};

} // namespace recorder_core
