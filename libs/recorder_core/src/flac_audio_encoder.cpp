#include "flac_audio_encoder.h"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>

namespace recorder_core {

namespace {

// C-ABI write callback matching libFLAC's FLAC__StreamEncoderWriteCallback
// typedef exactly. Routes to the owning FlacAudioEncoder via client_data.
FLAC__StreamEncoderWriteStatus FlacWriteCallback(const FLAC__StreamEncoder* /*encoder*/, const FLAC__byte buffer[],
                                                 size_t bytes, uint32_t samples, uint32_t /*current_frame*/,
                                                 void* client_data) {
    auto* self = static_cast<FlacAudioEncoder*>(client_data);
    return static_cast<FLAC__StreamEncoderWriteStatus>(self->OnWrite(buffer, bytes, samples));
}

// Cast the opaque handle held by the encoder back to the libFLAC type.
inline FLAC__StreamEncoder* AsEnc(void* p) {
    return static_cast<FLAC__StreamEncoder*>(p);
}

} // namespace

// ---------------------------------------------------------------------------
// Float32ToS16
// ---------------------------------------------------------------------------

int32_t FlacAudioEncoder::Float32ToS16(float sample) noexcept {
    // Identical mapping to the PCM path: clamp to [-1, 1], scale by 32767, round
    // to nearest. Using 32767 keeps +1.0 → +32767 and -1.0 → -32767; the
    // asymmetric -32768 is only reachable below -1.0, which is clamped away.
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const long scaled = std::lround(clamped * 32767.0f);
    const long bounded = std::clamp(scaled, -32768L, 32767L);
    return static_cast<int32_t>(bounded);
}

// ---------------------------------------------------------------------------
// OnWrite — receives bytes from libFLAC's write callback
// ---------------------------------------------------------------------------

int FlacAudioEncoder::OnWrite(const uint8_t* buffer, size_t bytes, uint32_t samples) {
    if (buffer == nullptr || bytes == 0) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    // samples == 0 → metadata/header write (the "fLaC" marker + STREAMINFO and
    // any other leading metadata blocks). Everything written before the first
    // audio frame is the native FLAC header → the A_FLAC CodecPrivate.
    if (samples == 0 && m_capturing_header) {
        m_codec_private.insert(m_codec_private.end(), buffer, buffer + bytes);
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    // samples > 0 → an encoded audio frame. The first such write closes the
    // header-capture window. FLAC frames are emitted strictly in order and are
    // contiguous, so the PTS of a frame is the input position of its first
    // sample = (emitted-samples-so-far) / sample_rate. This is independent of
    // WHEN libFLAC happens to flush the frame (it buffers a full blocksize before
    // emitting), which is why we track a dedicated emitted-sample counter rather
    // than reading the caller's accumulated-frame counter at flush time.
    m_capturing_header = false;

    EncodedAudioPacket pkt;
    pkt.pts_ns = (m_sample_rate != 0) ? (m_emitted_samples * 1000000000ULL) / m_sample_rate : 0;
    pkt.bytes.assign(buffer, buffer + bytes);
    m_pending_frames.push_back(std::move(pkt));

    m_emitted_samples += static_cast<uint64_t>(samples);

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool FlacAudioEncoder::Init(uint32_t sample_rate, uint32_t channels, std::string& out_error) {
    if (sample_rate == 0 || channels == 0) {
        out_error = "FlacAudioEncoder::Init requires non-zero sample_rate and channels";
        return false;
    }
    if (m_encoder != nullptr) {
        out_error = "FlacAudioEncoder::Init called twice";
        return false;
    }

    m_sample_rate = sample_rate;
    m_channels = channels;
    m_capturing_header = true;
    m_codec_private.clear();
    m_pending_frames.clear();
    m_emitted_samples = 0;
    m_finished = false;

    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (enc == nullptr) {
        out_error = "FLAC__stream_encoder_new returned null";
        return false;
    }
    m_encoder = enc;

    // Configure the encoder. Failures here are programmer errors (the encoder is
    // freshly created and uninitialized) but we surface them defensively.
    bool ok = true;
    ok = ok && FLAC__stream_encoder_set_channels(enc, channels);
    ok = ok && FLAC__stream_encoder_set_bits_per_sample(enc, kBitsPerSample);
    ok = ok && FLAC__stream_encoder_set_sample_rate(enc, sample_rate);
    ok = ok && FLAC__stream_encoder_set_compression_level(enc, kCompressionLevel);
    // Streamable subset keeps blocksize/parameters within the FLAC subset so the
    // stream is broadly playable (and valid embedded in Matroska).
    ok = ok && FLAC__stream_encoder_set_streamable_subset(enc, true);
    if (!ok) {
        out_error = "FLAC__stream_encoder_set_* failed before init";
        FLAC__stream_encoder_delete(enc);
        m_encoder = nullptr;
        return false;
    }

    // init_stream writes the "fLaC" marker + STREAMINFO (and any other metadata
    // blocks) through the write callback immediately — captured as CodecPrivate.
    const FLAC__StreamEncoderInitStatus init_status = FLAC__stream_encoder_init_stream(
        enc, &FlacWriteCallback, /*seek_callback=*/nullptr,
        /*tell_callback=*/nullptr, /*metadata_callback=*/nullptr, /*client_data=*/this);
    if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        out_error =
            std::string("FLAC__stream_encoder_init_stream failed: ") + FLAC__StreamEncoderInitStatusString[init_status];
        FLAC__stream_encoder_delete(enc);
        m_encoder = nullptr;
        return false;
    }

    if (m_codec_private.empty()) {
        out_error = "FLAC encoder produced no header (CodecPrivate) at init";
        FLAC__stream_encoder_delete(enc);
        m_encoder = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// FeedFloat32
// ---------------------------------------------------------------------------

void FlacAudioEncoder::FeedFloat32(const float* data, size_t total_float_samples, uint64_t pts_ns,
                                   uint64_t& accumulated_frames, uint32_t sample_rate, uint32_t channels,
                                   std::vector<EncodedAudioPacket>& out_packets) {
    (void)pts_ns;
    if (m_encoder == nullptr || data == nullptr || total_float_samples == 0 || channels == 0) {
        return;
    }
    if (sample_rate != m_sample_rate || channels != m_channels) {
        return;
    }

    const size_t frame_count = total_float_samples / channels;
    if (frame_count == 0) {
        return;
    }

    // Convert interleaved Float32 → interleaved int16 (as FLAC__int32 samples).
    m_int_scratch.resize(total_float_samples);
    for (size_t i = 0; i < total_float_samples; ++i) {
        m_int_scratch[i] = Float32ToS16(data[i]);
    }

    // process_interleaved consumes `samples` *frames* (per-channel), not the
    // total interleaved count. Emitted frames are appended via the write callback.
    if (!FLAC__stream_encoder_process_interleaved(AsEnc(m_encoder), m_int_scratch.data(),
                                                  static_cast<uint32_t>(frame_count))) {
        // Encoding error: leave whatever was already emitted; the audio thread
        // continues and the (best-effort) stream is finalized at Flush.
        return;
    }

    // Drain any frames the encoder emitted during this call.
    for (auto& pkt : m_pending_frames) {
        out_packets.push_back(std::move(pkt));
    }
    m_pending_frames.clear();

    accumulated_frames += static_cast<uint64_t>(frame_count);
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void FlacAudioEncoder::Flush(std::vector<EncodedAudioPacket>& out_packets) {
    if (m_encoder == nullptr || m_finished) {
        return;
    }
    m_finished = true;

    // finish() encodes any buffered samples, emitting final frame(s) through the
    // write callback (and may rewrite STREAMINFO, but only via seek which we do
    // not provide, so the header in CodecPrivate is the init-time one).
    FLAC__stream_encoder_finish(AsEnc(m_encoder));

    for (auto& pkt : m_pending_frames) {
        out_packets.push_back(std::move(pkt));
    }
    m_pending_frames.clear();
}

// ---------------------------------------------------------------------------
// CodecPrivateBytes / Shutdown
// ---------------------------------------------------------------------------

std::vector<uint8_t> FlacAudioEncoder::CodecPrivateBytes() const {
    return m_codec_private;
}

void FlacAudioEncoder::Shutdown() {
    if (m_encoder != nullptr) {
        FLAC__StreamEncoder* enc = AsEnc(m_encoder);
        if (!m_finished) {
            FLAC__stream_encoder_finish(enc);
        }
        FLAC__stream_encoder_delete(enc);
        m_encoder = nullptr;
    }
    m_pending_frames.clear();
    m_int_scratch.clear();
    m_sample_rate = 0;
    m_channels = 0;
    m_finished = false;
}

FlacAudioEncoder::~FlacAudioEncoder() {
    Shutdown();
}

} // namespace recorder_core
