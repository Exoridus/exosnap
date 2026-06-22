#include "output_format_audio_src.h"

// FFmpeg libswresample and libavutil headers
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <cstring>

namespace recorder_core {

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

    const uint32_t inner_rate = inner_->SampleRate();
    const uint32_t inner_channels = inner_->Channels();

    if (inner_rate == 0 || inner_channels == 0) {
        out_error = "OutputFormatAudioSrc: inner source reported invalid rate/channels after Init";
        return false;
    }

    // Fast path: if target == inner, skip the SwrContext entirely.
    if (target_sample_rate_ == inner_rate && target_channels_ == inner_channels) {
        passthrough_ = true;
        initialized_ = true;
        return true;
    }

    passthrough_ = false;

    // Build channel layouts using the modern AVChannelLayout API (avutil-60).
    AVChannelLayout in_layout = {};
    AVChannelLayout out_layout = {};
    av_channel_layout_default(&in_layout, static_cast<int>(inner_channels));
    av_channel_layout_default(&out_layout, static_cast<int>(target_channels_));

    // swr_alloc_set_opts2: allocates and configures the context in one call.
    // In = inner format (Float32 interleaved = AV_SAMPLE_FMT_FLT).
    // Out = target format (Float32 interleaved = AV_SAMPLE_FMT_FLT).
    // We keep Float32 throughout; bit-depth conversion is the encoder's job.
    int ret = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(target_sample_rate_),
                                  &in_layout, AV_SAMPLE_FMT_FLT, static_cast<int>(inner_rate), 0, nullptr);

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
    if (inner_channels != target_channels_) {
        if (inner_channels == 2 && target_channels_ == 1) {
            const double matrix[2] = {0.5, 0.5}; // out0 = 0.5*in0 + 0.5*in1
            swr_set_matrix(swr_, matrix, 2);
        } else if (inner_channels == 1 && target_channels_ == 2) {
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

    initialized_ = true;
    return true;
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

    // ---- Passthrough mode ----
    if (passthrough_) {
        out_buf = src_buf;
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

    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(resample_buf_.data());

    const int produced = swr_convert(swr_, &out_ptr, max_out_frames, &in_ptr, in_frames);

    if (produced < 0) {
        // Conversion error: emit the raw source buffer in degraded mode (wrong
        // rate/channels) rather than crashing the recording. This should never
        // happen in practice given vetted inputs.
        out_buf = src_buf;
        return true;
    }

    exposed_buf_.bytes = reinterpret_cast<const uint8_t*>(resample_buf_.data());
    exposed_buf_.num_frames = static_cast<uint32_t>(produced);
    exposed_buf_.silent = (produced == 0) || src_buf.silent;
    exposed_buf_.data_discontinuity = src_buf.data_discontinuity;

    out_buf = exposed_buf_;
    return true;
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
}

} // namespace recorder_core
