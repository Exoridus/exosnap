#include "output_format_audio_src.h"

#include "discontinuity_gap.h"

// FFmpeg libswresample and libavutil headers
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cmath>
#include <cstdint>
#include <cstring>

namespace exosnap::engine {

namespace {

// Int16 interleaved -> Float32 interleaved, normalised by 1/32768. Matches the
// scale used by MixedAudioSrc and the encoder-side conversion so every audio
// path in the engine agrees on the same mapping.
void ConvertInt16ToFloat32(const std::int16_t* src, float* dst, size_t sample_count) {
    constexpr float kInt16Scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<float>(src[i]) * kInt16Scale;
    }
}

} // namespace

OutputFormatAudioSrc::OutputFormatAudioSrc(std::unique_ptr<IAudioCaptureSource> inner, uint32_t target_sample_rate,
                                           uint32_t target_channels)
    : inner_(std::move(inner)), target_sample_rate_(target_sample_rate), target_channels_(target_channels) {
}

OutputFormatAudioSrc::~OutputFormatAudioSrc() {
    Shutdown();
}

bool OutputFormatAudioSrc::Init(std::string& out_error) {
    if (!inner_) {
        out_error = "OutputFormatAudioSrc: inner source is null";
        return false;
    }
    if (!inner_->Init(out_error)) {
        return false;
    }
    return ConfigureConversion(out_error);
}

bool OutputFormatAudioSrc::Reinit(std::string& out_error) {
    out_error.clear();
    if (!inner_) {
        out_error = "OutputFormatAudioSrc::Reinit: inner source is null";
        return false;
    }
    // Reacquire the inner with its own identity rules (a PID-keyed loopback may
    // refuse; a default endpoint follows the current default). Rebuild the
    // resampler for the reacquired stream — its native format may have changed.
    if (!inner_->Reinit(out_error)) {
        return false;
    }
    if (swr_ != nullptr) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    // Clock slaving resets with the reacquired stream: the fresh device restarts
    // its position near zero and the drift estimator resets in lockstep
    // (audio_thread), so the controller must re-engage from a clean baseline
    // rather than resume against stale frame accounting.
    compensation_engaged_ = false;
    compensation_ppm_ = 0.0;
    in_total_ = 0;
    out_total_ = 0;
    return ConfigureConversion(out_error);
}

bool OutputFormatAudioSrc::ConfigureConversion(std::string& out_error) {
    inner_rate_ = inner_->SampleRate();
    inner_channels_ = inner_->Channels();
    inner_format_ = inner_->SampleFormat();

    if (inner_rate_ == 0 || inner_channels_ == 0) {
        out_error = "OutputFormatAudioSrc: inner source reported invalid rate/channels after Init";
        return false;
    }

    // Fast path: if target rate/channels == inner, skip the SwrContext entirely.
    // Even here the inner may be Int16 (e.g. process-loopback capture); the byte
    // conversion to Float32 happens in AcquireBuffer. Clock slaving can later
    // leave this passthrough lazily (SetCompensationPpm) once real drift engages.
    if (target_sample_rate_ == inner_rate_ && target_channels_ == inner_channels_) {
        passthrough_ = true;
        initialized_ = true;
        return true;
    }

    passthrough_ = false;
    if (!BuildSwrContext(out_error)) {
        return false;
    }
    initialized_ = true;
    return true;
}

bool OutputFormatAudioSrc::BuildSwrContext(std::string& out_error) {
    // Input sample format for the resampler follows the inner source. The
    // sources feeding this decorator deliver either Float32 (mix bus, WASAPI
    // loopback, mic DSP) or Int16 (process loopback, some mic endpoints).
    const AVSampleFormat in_sample_fmt =
        (inner_format_ == AudioSampleFormat::Int16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLT;

    // Build channel layouts using the modern AVChannelLayout API (avutil-60).
    AVChannelLayout in_layout = {};
    AVChannelLayout out_layout = {};
    av_channel_layout_default(&in_layout, static_cast<int>(inner_channels_));
    av_channel_layout_default(&out_layout, static_cast<int>(target_channels_));

    // swr_alloc_set_opts2: allocates and configures the context in one call.
    // In  = inner format (Int16 or Float32 interleaved, per in_sample_fmt).
    // Out = Float32 interleaved (AV_SAMPLE_FMT_FLT). The resampler up-converts
    // Int16 inputs to Float32; deeper bit-depth conversion is the encoder's job.
    // When inner and target rates are equal (the lazy clock-slaving engage) this
    // is an identity resampler; swr_set_compensation still nudges its rate.
    int ret = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(target_sample_rate_),
                                  &in_layout, in_sample_fmt, static_cast<int>(inner_rate_), 0, nullptr);

    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (ret < 0 || swr_ == nullptr) {
        out_error = "OutputFormatAudioSrc: swr_alloc_set_opts2 failed";
        return false;
    }

    // Channel down/up-mix uses simple averaging / duplication instead of
    // libswresample's default power-preserving (1/sqrt2) downmix matrix. The mix
    // bus that feeds us is already peak-limited, so an averaging downmix (L+R)/2
    // cannot clip on correlated full-scale content, and mono->stereo duplication
    // is unity. Must be set after configure, before swr_init.
    if (inner_channels_ != target_channels_) {
        if (inner_channels_ == 2 && target_channels_ == 1) {
            const double matrix[2] = {0.5, 0.5}; // out0 = 0.5*in0 + 0.5*in1
            swr_set_matrix(swr_, matrix, 2);
        } else if (inner_channels_ == 1 && target_channels_ == 2) {
            const double matrix[2] = {1.0, 1.0}; // out0 = in0, out1 = in0
            swr_set_matrix(swr_, matrix, 1);
        }
    }

    ret = swr_init(swr_);
    if (ret < 0) {
        swr_free(&swr_);
        out_error = "OutputFormatAudioSrc: swr_init failed";
        return false;
    }
    return true;
}

void OutputFormatAudioSrc::SetCompensationPpm(double ppm) {
    compensation_ppm_ = ppm;
    if (ppm != 0.0 && !compensation_engaged_) {
        // First non-zero command: leave the passthrough permanently. Build a
        // lazy inner->target context if we do not have one yet (the default
        // 48 k/stereo passthrough). If the build fails, stay in passthrough —
        // degrade to "no compensation" rather than break the recording.
        if (passthrough_ || swr_ == nullptr) {
            std::string err;
            if (BuildSwrContext(err)) {
                passthrough_ = false;
                in_total_ = 0;
                out_total_ = 0;
                compensation_engaged_ = true;
            } else {
                compensation_ppm_ = 0.0; // build failed: remain a no-op
            }
        } else {
            // Already on the resample path (44.1 k / 96 k / mono target).
            compensation_engaged_ = true;
        }
    }
}

double OutputFormatAudioSrc::AppliedCompensationMs() const {
    if ((in_total_ == 0 && out_total_ == 0) || inner_rate_ == 0 || target_sample_rate_ == 0) {
        return 0.0;
    }
    // A = out_seconds - in_seconds, in ms. int64 numerator (> 60 years at 48 kHz
    // without overflow), converted to a double millisecond value. Positive =
    // output timeline stretched (events later) — corrects a positive drift.
    const int64_t num =
        out_total_ * static_cast<int64_t>(inner_rate_) - in_total_ * static_cast<int64_t>(target_sample_rate_);
    return static_cast<double>(num) * 1000.0 /
           (static_cast<double>(inner_rate_) * static_cast<double>(target_sample_rate_));
}

uint32_t OutputFormatAudioSrc::PendingFrameCount() {
    return inner_->PendingFrameCount();
}

bool OutputFormatAudioSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    RawAudioBuffer src_buf{};
    if (!inner_->AcquireBuffer(src_buf, out_error)) {
        return false;
    }

    // ---- Passthrough mode (target rate/channels == inner) ----
    if (passthrough_) {
        // Float32 inner: hand the bytes through unchanged (zero-copy).
        // Silent/null buffers carry no samples to convert — the caller treats
        // them as zero-fill of num_frames, so forward them verbatim too.
        if (inner_format_ == AudioSampleFormat::Float32 || src_buf.silent || src_buf.bytes == nullptr) {
            out_buf = src_buf;
            return true;
        }

        // Int16 inner: convert to the Float32 this decorator advertises.
        const size_t sample_count = static_cast<size_t>(src_buf.num_frames) * target_channels_;
        resample_buf_.resize(sample_count);
        ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(src_buf.bytes), resample_buf_.data(), sample_count);

        exposed_buf_.bytes = reinterpret_cast<const uint8_t*>(resample_buf_.data());
        exposed_buf_.num_frames = src_buf.num_frames;
        exposed_buf_.silent = src_buf.silent;
        exposed_buf_.data_discontinuity = src_buf.data_discontinuity;
        exposed_buf_.gap_frames = src_buf.gap_frames;
        out_buf = exposed_buf_;
        return true;
    }

    // ---- Resampling mode ----
    // Handle silent buffers: treat as zero input of the same duration.
    // Compute how many input frames we logically have. For silent buffers
    // src_buf.bytes may be null; we still feed zeros to maintain the resampler's
    // time base.
    const uint32_t inner_channels = inner_->Channels();
    const int in_frames = static_cast<int>(src_buf.num_frames);

    // Worst-case output frame count: swr_get_out_samples accounts for internal
    // buffered samples plus the new input.
    const int max_out_frames = swr_get_out_samples(swr_, in_frames);
    if (max_out_frames < 0) {
        // Treat as produce-nothing (edge case).
        out_buf.silent = src_buf.silent;
        out_buf.data_discontinuity = src_buf.data_discontinuity;
        out_buf.gap_frames = ScaleDiscontinuityGapFrames(src_buf.gap_frames, inner_->SampleRate(), target_sample_rate_);
        out_buf.num_frames = 0;
        return true;
    }

    // Allocate output scratch.
    const size_t out_samples = static_cast<size_t>(max_out_frames) * target_channels_;
    resample_buf_.resize(out_samples);

    // Prepare source pointer (null for silent — swr_convert will generate silence).
    const uint8_t* in_ptr = nullptr;
    std::vector<float> silence_buf;
    if (!src_buf.silent && src_buf.bytes != nullptr) {
        in_ptr = src_buf.bytes;
    } else {
        // Feed zero-valued samples (silence) so swr keeps its internal clock.
        silence_buf.assign(static_cast<size_t>(in_frames) * inner_channels, 0.0f);
        in_ptr = reinterpret_cast<const uint8_t*>(silence_buf.data());
    }

    // Re-arm the clock-slaving compensation window before every convert while a
    // compensation has ever engaged. distance = 10 * target_rate gives 0.1 ppm
    // resolution; the window never expires because we re-arm each acquire. A ppm
    // of 0 arms a zero delta, cancelling cleanly while keeping the context.
    // Sign contract: p > 0 -> positive sample_delta -> more output frames per
    // input (the swr characterization test pins this; invert here if it fails).
    if (compensation_engaged_ && swr_ != nullptr) {
        const int distance = 10 * static_cast<int>(target_sample_rate_);
        const int sample_delta = static_cast<int>(std::lround(compensation_ppm_ * 1e-6 * distance));
        swr_set_compensation(swr_, sample_delta, distance);
    }

    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(resample_buf_.data());

    const int produced = swr_convert(swr_, &out_ptr, max_out_frames, &in_ptr, in_frames);

    if (produced < 0) {
        // Conversion error: emit the raw source buffer in degraded mode (wrong
        // rate/channels) rather than crashing the recording. This should never
        // happen in practice given vetted inputs.
        out_buf = src_buf;
        return true;
    }

    // Real frame accounting for the applied-compensation metric (A). Counts on
    // the resample path only; passthrough leaves both axes identical (A == 0).
    in_total_ += in_frames;
    out_total_ += produced;

    exposed_buf_.bytes = reinterpret_cast<const uint8_t*>(resample_buf_.data());
    exposed_buf_.num_frames = static_cast<uint32_t>(produced);
    exposed_buf_.silent = (produced == 0) || src_buf.silent;
    exposed_buf_.data_discontinuity = src_buf.data_discontinuity;
    // The gap was measured in the inner source's frames; report it in this
    // decorator's output frames.
    exposed_buf_.gap_frames =
        ScaleDiscontinuityGapFrames(src_buf.gap_frames, inner_->SampleRate(), target_sample_rate_);

    out_buf = exposed_buf_;
    return true;
}

uint32_t OutputFormatAudioSrc::DrainResampler(RawAudioBuffer& out_buf, int64_t* out_undrained_frames) {
    out_buf = {};
    if (out_undrained_frames != nullptr) {
        *out_undrained_frames = 0;
    }
    if (passthrough_ || swr_ == nullptr) {
        // Fast path: whole buffers are forwarded (or converted) 1:1 and no
        // filter state is retained, so end-of-stream has nothing to flush.
        return 0;
    }

    // swr_convert with a null input flushes the resampler's internal filter
    // delay. Size each chunk from swr_get_delay (queried in output samples) plus
    // a margin, so a reported delay of 0 still gets one call that can prove the
    // context empty. This works unchanged with an armed compensation: the delta
    // set by the last AcquireBuffer only shapes the rate, it does not gate the
    // flush.
    resample_buf_.clear();
    uint32_t total_frames = 0;
    constexpr int kMinChunkFrames = 64;
    // libswresample empties its delay line in one call (two with an odd
    // compensation phase); the bound keeps a pathological context from spinning
    // the stop path forever.
    bool emptied = false;
    for (int iter = 0; iter < kMaxDrainIterations; ++iter) {
        const int64_t delay = swr_get_delay(swr_, static_cast<int64_t>(target_sample_rate_));
        const int chunk = (delay > 0 ? static_cast<int>(delay) : 0) + kMinChunkFrames;
        const size_t base = resample_buf_.size();
        resample_buf_.resize(base + static_cast<size_t>(chunk) * target_channels_);
        uint8_t* out_ptr = reinterpret_cast<uint8_t*>(resample_buf_.data() + base);
        const int produced = swr_convert(swr_, &out_ptr, chunk, nullptr, 0);
        if (produced <= 0) {
            resample_buf_.resize(base);
            emptied = true;
            break;
        }
        resample_buf_.resize(base + static_cast<size_t>(produced) * target_channels_);
        total_frames += static_cast<uint32_t>(produced);
    }

    if (!emptied && out_undrained_frames != nullptr) {
        // The bound was reached while the context was still producing, so the
        // remainder is dropped. Unreachable with libswresample's actual flush
        // behaviour, but report what was left instead of losing it silently —
        // the caller has the track context to log it against.
        *out_undrained_frames = swr_get_delay(swr_, static_cast<int64_t>(target_sample_rate_));
    }

    if (total_frames == 0) {
        return 0;
    }

    // Count the flushed frames on the output axis so the applied-compensation
    // metric stays consistent with what actually reached the encoder.
    out_total_ += static_cast<int64_t>(total_frames);

    exposed_buf_ = {};
    exposed_buf_.bytes = reinterpret_cast<const uint8_t*>(resample_buf_.data());
    exposed_buf_.num_frames = total_frames;
    out_buf = exposed_buf_;
    return total_frames;
}

void OutputFormatAudioSrc::ReleaseBuffer() {
    inner_->ReleaseBuffer();
}

uint32_t OutputFormatAudioSrc::SampleRate() const {
    return target_sample_rate_;
}

uint32_t OutputFormatAudioSrc::Channels() const {
    return target_channels_;
}

AudioSampleFormat OutputFormatAudioSrc::SampleFormat() const {
    return AudioSampleFormat::Float32;
}

const std::string& OutputFormatAudioSrc::EndpointName() const {
    return inner_->EndpointName();
}

int32_t OutputFormatAudioSrc::LastCaptureHresult() const {
    return inner_ ? inner_->LastCaptureHresult() : 0;
}

bool OutputFormatAudioSrc::LastBufferDeviceTiming(AudioDeviceTiming& out_timing) const {
    // The inner device clock is what drifts; resampling shifts neither axis.
    return inner_ && inner_->LastBufferDeviceTiming(out_timing);
}

void* OutputFormatAudioSrc::BufferReadyEvent() const {
    return inner_ ? inner_->BufferReadyEvent() : nullptr;
}

uint32_t OutputFormatAudioSrc::CaptureSourceCount() const {
    return inner_ ? inner_->CaptureSourceCount() : 1;
}

uint32_t OutputFormatAudioSrc::DegradedSourceCount() const {
    return inner_ ? inner_->DegradedSourceCount() : 0;
}

uint32_t OutputFormatAudioSrc::DegradedSourceIndexMask() const {
    return inner_ ? inner_->DegradedSourceIndexMask() : 0;
}

void OutputFormatAudioSrc::Shutdown() {
    if (swr_ != nullptr) {
        swr_free(&swr_);
        swr_ = nullptr;
    }
    if (inner_) {
        inner_->Shutdown();
    }
    initialized_ = false;
    passthrough_ = false;
    compensation_engaged_ = false;
    compensation_ppm_ = 0.0;
    in_total_ = 0;
    out_total_ = 0;
}

} // namespace exosnap::engine
