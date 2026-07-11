#pragma once

// Discontinuity-gap policy for WASAPI capture sources.
//
// AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY tells us frames were lost between the
// previous packet and this one, but not how many. The device position that
// IAudioCaptureClient::GetBuffer reports alongside the packet does: the
// position of the flagged packet minus the expected position (previous packet's
// position + its frame count) is exactly the number of frames the device
// dropped. These helpers turn that arithmetic into a shared, unit-tested
// policy: only forward jumps count, and a single reported gap is clamped so a
// pathological position jump can never demand minutes of synthesized silence.

#include <cstdint>

namespace recorder_core {

// Upper bound for one reported gap. Real underruns are milliseconds; anything
// beyond this is treated as a position glitch and clamped rather than honored.
inline constexpr uint64_t kMaxDiscontinuityGapSeconds = 10;

// Length (in frames) of the capture gap that precedes a packet.
//   discontinuity            — the packet carried DATA_DISCONTINUITY
//   have_expected_position   — a previous packet established the expected position
//   expected_device_position — previous packet position + previous frame count
//   device_position          — position reported with THIS packet
//   sample_rate              — used only to clamp the gap to the maximum above
// Returns 0 unless the flag is set AND the position jumped forward.
inline uint32_t ComputeDiscontinuityGapFrames(bool discontinuity, bool have_expected_position,
                                              uint64_t expected_device_position, uint64_t device_position,
                                              uint32_t sample_rate) noexcept {
    if (!discontinuity || !have_expected_position) {
        return 0;
    }
    if (device_position <= expected_device_position) {
        return 0;
    }
    const uint64_t gap = device_position - expected_device_position;
    const uint64_t max_gap = kMaxDiscontinuityGapSeconds * static_cast<uint64_t>(sample_rate);
    return static_cast<uint32_t>(gap < max_gap ? gap : max_gap);
}

// Rescale a gap length between sample rates (round to nearest). Used by
// resampling decorators so a gap measured in source frames is reported in the
// decorator's output frames.
inline uint32_t ScaleDiscontinuityGapFrames(uint32_t gap_frames, uint32_t from_rate, uint32_t to_rate) noexcept {
    if (gap_frames == 0 || from_rate == 0 || from_rate == to_rate) {
        return gap_frames;
    }
    const uint64_t scaled =
        (static_cast<uint64_t>(gap_frames) * to_rate + static_cast<uint64_t>(from_rate) / 2) / from_rate;
    return static_cast<uint32_t>(scaled);
}

} // namespace recorder_core
