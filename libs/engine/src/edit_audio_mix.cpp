#include "edit_audio_mix.h"

#include <algorithm>

namespace exosnap::engine {

namespace {

// Absolute sample-frame index for a media timestamp, rounded to nearest.
// Negative timestamps are clamped away first: the playback path never produces
// one (the preroll trim cuts everything before the requested start), and the
// rounding below only holds for non-negative values.
int64_t FrameIndexForUs(int64_t us) noexcept {
    const int64_t clamped = std::max<int64_t>(us, 0);
    return (clamped * EditAudioMixer::kSampleRate + 500'000) / 1'000'000;
}

int64_t UsForFrameIndex(int64_t frame) noexcept {
    return frame * 1'000'000 / static_cast<int64_t>(EditAudioMixer::kSampleRate);
}

} // namespace

void EditAudioMixer::Reset(size_t track_count, int64_t start_us) {
    track_count_ = track_count;
    origin_frame_ = FrameIndexForUs(start_us);
    track_through_frame_.assign(track_count, origin_frame_);
    has_contributed_.assign(track_count, false);
    accum_.clear();
    late_frames_dropped_ = 0;

    BrickwallLimiter::Config cfg;
    cfg.sample_rate = kSampleRate;
    cfg.channels = kChannels;
    // Ceiling and the attack/release envelope stay at the limiter's own
    // defaults (0 dBFS, 1 ms / 80 ms) -- the same shape the record path's
    // merge already uses, so a merged track and two separate tracks played
    // back together behave the same way when they overload.
    limiter_.Configure(cfg);
    limiter_.Reset();
}

void EditAudioMixer::Submit(size_t track, int64_t pts_us, const float* interleaved_stereo, size_t frame_count) {
    if (track >= track_count_ || interleaved_stereo == nullptr || frame_count == 0)
        return;

    has_contributed_[track] = true;

    const int64_t block_start = FrameIndexForUs(pts_us);
    const int64_t block_end = block_start + static_cast<int64_t>(frame_count);
    // Monotonic: the through mark is what tells Take() which stretch is still
    // open, so a container reordering that walked it backwards would re-open a
    // stretch already handed on.
    if (block_end > track_through_frame_[track])
        track_through_frame_[track] = block_end;

    int64_t offset = block_start - origin_frame_;
    size_t skip = 0;
    if (offset < 0) {
        // Samples older than what the mix has already emitted have nowhere
        // left to go. Counted rather than silently ignored, so a lookbehind
        // that turns out too tight for some file is visible as a number
        // instead of only as something faintly missing.
        const int64_t late = -offset;
        if (late >= static_cast<int64_t>(frame_count)) {
            late_frames_dropped_ += frame_count;
            return;
        }
        skip = static_cast<size_t>(late);
        late_frames_dropped_ += skip;
        offset = 0;
    }

    const size_t contributed = frame_count - skip;
    const size_t needed = (static_cast<size_t>(offset) + contributed) * kChannels;
    // Growing to reach `offset` leaves zeroes behind it, so a stretch no track
    // delivered is handed on as silence rather than closed up. That is the
    // point: the renderer's clock counts samples, so swallowing a gap instead
    // of playing it would walk the audio clock ahead of media time and put
    // video permanently behind by the length of the hole.
    if (accum_.size() < needed)
        accum_.resize(needed, 0.0f);

    float* dst = accum_.data() + static_cast<size_t>(offset) * kChannels;
    const float* src = interleaved_stereo + skip * kChannels;
    const size_t samples = contributed * kChannels;
    for (size_t i = 0; i < samples; ++i)
        dst[i] += src[i];
}

std::optional<EditAudioMixer::MixedBlock> EditAudioMixer::Take() {
    if (track_count_ == 0 || accum_.empty())
        return std::nullopt;

    const int64_t accum_end = origin_frame_ + static_cast<int64_t>(accum_.size() / kChannels);

    // Complete through the point the TRAILING track has reached: anything past
    // it is a stretch a track could still add to.
    int64_t newest = origin_frame_;
    int64_t barrier = accum_end;
    bool all_started = true;
    for (size_t i = 0; i < track_count_; ++i) {
        newest = std::max(newest, track_through_frame_[i]);
        barrier = std::min(barrier, track_through_frame_[i]);
        if (!has_contributed_[i])
            all_started = false;
    }
    // ...except that a track this far behind is treated as having nothing more
    // to say, so one that ends early neither stops the mix nor silences the
    // others (see kMaxTrackLagFrames). A track that has not contributed even
    // once yet gets the wider kStartupGraceFrames instead: it is
    // indistinguishable from one that already ended, but deserves more benefit
    // of the doubt (see kStartupGraceFrames).
    const int64_t lag_frames = static_cast<int64_t>(all_started ? kMaxTrackLagFrames : kStartupGraceFrames);
    barrier = std::max(barrier, newest - lag_frames);
    barrier = std::min(barrier, accum_end);

    if (barrier <= origin_frame_)
        return std::nullopt;
    return TakeFrames(static_cast<size_t>(barrier - origin_frame_));
}

std::optional<EditAudioMixer::MixedBlock> EditAudioMixer::TakeRemainder() {
    if (accum_.empty())
        return std::nullopt;
    return TakeFrames(accum_.size() / kChannels);
}

std::optional<EditAudioMixer::MixedBlock> EditAudioMixer::TakeFrames(size_t frames) {
    if (frames == 0)
        return std::nullopt;
    const size_t samples = frames * kChannels;

    MixedBlock block;
    block.pts_us = UsForFrameIndex(origin_frame_);
    block.frame_count = static_cast<uint32_t>(frames);
    block.interleaved_stereo.assign(accum_.begin(), accum_.begin() + static_cast<std::ptrdiff_t>(samples));
    // Limited on the way out, once, over the summed signal -- the envelope is
    // carried across blocks (BrickwallLimiter's own contract), so re-chunking
    // the stream at barrier boundaries does not change how it sounds.
    limiter_.Process(block.interleaved_stereo.data(), block.frame_count);

    accum_.erase(accum_.begin(), accum_.begin() + static_cast<std::ptrdiff_t>(samples));
    origin_frame_ += static_cast<int64_t>(frames);
    return block;
}

} // namespace exosnap::engine
