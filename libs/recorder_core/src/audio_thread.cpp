#include "audio_thread.h"

#include "codec_private.h"
#include "fdk_aac_encoder.h"
#include "flac_audio_encoder.h"
#include "opus_audio_encoder.h"
#include "output_format_audio_src.h"
#include "pcm_audio_encoder.h"
#include "recorder_core/audio_meter.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace recorder_core {

namespace {

constexpr float kRmsEmaAlpha = 0.3f;

void ConvertInt16ToFloat32(const std::int16_t* src, float* dst, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<float>(src[i]) / 32768.0f;
    }
}

// Per-codec wiring that differs between the otherwise identical encode loops:
// which encoder to build, how its init failure reads, and whether an empty
// codec-private after Init is a hard error (FLAC/AAC produce theirs during
// Init; PCM legitimately has none; Opus always builds a 19-byte OpusHead).
struct EncoderSetup {
    std::unique_ptr<IAudioEncoder> encoder;
    const char* init_error_prefix = nullptr;
    const char* empty_codec_private_error = nullptr; // nullptr == empty allowed
};

EncoderSetup MakeEncoderSetup(const RecorderConfig& config) {
    EncoderSetup setup;
    switch (config.audio_codec) {
    case AudioCodec::Opus: {
        auto enc = std::make_unique<OpusAudioEncoder>();
        enc->SetEncodingParams(config.audio_bitrate_kbps, config.opus_frame_duration, config.opus_complexity);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "Opus encoder init: ";
        break;
    }
    case AudioCodec::Pcm: {
        // PCM passthrough "encoder" (MKV-only A_PCM/INT_LIT); no CodecPrivate —
        // the track is marked ready with empty bytes so the mux thread's
        // codec-private readiness gate releases the pre-mux buffer.
        auto enc = std::make_unique<PcmAudioEncoder>();
        enc->SetBitDepth(config.audio_bit_depth); // ADR 0030: configurable depth
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "PCM encoder init: ";
        break;
    }
    case AudioCodec::Flac: {
        // FLAC lossless encoder (MKV-only A_FLAC via libFLAC). Its CodecPrivate
        // (native "fLaC" header + STREAMINFO) is produced during Init() via the
        // write callback and must be non-empty.
        auto enc = std::make_unique<FlacAudioEncoder>();
        enc->SetBitDepth(config.audio_bit_depth); // ADR 0030: configurable depth + level
        enc->SetCompressionLevel(config.flac_compression_level);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FLAC encoder init: ";
        setup.empty_codec_private_error = "FLAC codec private is empty after Init";
        break;
    }
    default: {
        auto enc = std::make_unique<FdkAacEncoder>();
        enc->SetBitrateKbps(config.audio_bitrate_kbps);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FDK-AAC encoder init: ";
        setup.empty_codec_private_error = "FDK-AAC codec private is empty after Init";
        break;
    }
    }
    return setup;
}

} // namespace

AudioThread::AudioThread(std::shared_ptr<SessionState> state, std::unique_ptr<IAudioCaptureSource> source,
                         uint32_t track_id)
    : m_state_ptr(std::move(state)), m_state(*m_state_ptr), source_(std::move(source)), track_id_(track_id) {
}

AudioThread::~AudioThread() {
    // Start() gave the running thread shared ownership of this object, so a
    // joinable thread here has already returned from Run() (the final release
    // may even happen on the worker thread itself, where join() would
    // deadlock). Detaching a finished thread only releases its handle.
    if (m_thread.joinable())
        m_thread.detach();
}

void AudioThread::Start() {
    // Self-ownership handoff: the lambda keeps this worker (and through
    // m_state_ptr the SessionState) alive until Run() returns, so dropping the
    // session's handle on a stalled worker can never dangle the state the
    // thread still writes through.
    m_thread = std::thread([self = shared_from_this()] { self->Run(); });
}

bool AudioThread::Join(unsigned timeout_ms) {
    if (!m_thread.joinable())
        return true;
    HANDLE h = m_thread.native_handle();
    DWORD r = WaitForSingleObject(h, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
        m_thread.join();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void AudioThread::Run() {
    // COM init (apartment-threaded for Media Foundation)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_inited) {
        char buf[80];
        snprintf(buf, sizeof(buf), "AudioThread: CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
        return;
    }
    auto uninitCom = [&]() {
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
    };

    if (!source_) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio source is null");
        uninitCom();
        return;
    }

    if (track_id_ >= CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio track id is out of range");
        uninitCom();
        return;
    }

    // --- Wrap source in OutputFormatAudioSrc (ADR 0030) ---
    // Effective sample rate: Opus is locked to 48 kHz; all other codecs use
    // the configured audio_sample_rate. Channel count and bit depth are always
    // configurable. When target == 48000/stereo (the default), the decorator is
    // a byte-identical passthrough — no SwrContext is created.
    {
        const uint32_t effective_rate =
            (m_state.config.audio_codec == AudioCodec::Opus) ? 48000u : m_state.config.audio_sample_rate;
        const uint32_t effective_channels = m_state.config.audio_channels;

        // Wrap source_ so OutputFormatAudioSrc::Init calls the real source's Init
        // and configures swresample if needed. After this, source_ reports the
        // target sample_rate/channels.
        source_ = std::make_unique<OutputFormatAudioSrc>(std::move(source_), effective_rate, effective_channels);
    }

    {
        std::string err;
        if (!source_->Init(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source init failed: " + err);
            source_->Shutdown();
            uninitCom();
            return;
        }
    }

    const uint32_t kSampleRate = source_->SampleRate();
    const uint32_t kChannels = source_->Channels();
    const AudioSampleFormat sourceFormat = source_->SampleFormat();

    if (kSampleRate == 0 || kChannels == 0) {
        m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source reported invalid stream format");
        source_->Shutdown();
        uninitCom();
        return;
    }

    m_state.diagnostics.SetAudioFormat(kSampleRate, kChannels);

    // --- Encoder init (the only codec-specific part of this worker) ---
    EncoderSetup setup = MakeEncoderSetup(m_state.config);
    IAudioEncoder& encoder = *setup.encoder;
    {
        std::string err;
        if (!encoder.Init(kSampleRate, kChannels, err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, std::string(setup.init_error_prefix) + err);
            source_->Shutdown();
            uninitCom();
            return;
        }
    }

    // --- Publish codec private and mark the track ready so the mux thread's
    // codec-private readiness gate can release the pre-mux buffer. ---
    {
        auto cp = encoder.CodecPrivateBytes();
        if (setup.empty_codec_private_error != nullptr && cp.empty()) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, setup.empty_codec_private_error);
            encoder.Shutdown();
            source_->Shutdown();
            uninitCom();
            return;
        }
        std::lock_guard lk(m_state.premux_mutex);
        m_state.codec_private.audio_codec_private[track_id_].bytes = std::move(cp);
        m_state.codec_private.audio_track_ready[track_id_] = true;
        m_state.premux_cv.notify_all();
    }

    EncodeLoop(encoder, kSampleRate, kChannels, sourceFormat);

    encoder.Shutdown();
    uninitCom();
}

// ---------------------------------------------------------------------------
// EncodeLoop — capture/encode drain shared by every audio codec
// ---------------------------------------------------------------------------

void AudioThread::EncodeLoop(IAudioEncoder& enc, uint32_t sample_rate, uint32_t channels,
                             AudioSampleFormat source_format) {
    uint64_t lastAudioPts = 0;

    // M4 Phase 4: one audio producer; Phase 5 will instantiate multiple AudioThread workers.
    auto routeAudioPackets = [&](std::vector<EncodedAudioPacket>& pkts) {
        for (auto& pkt : pkts) {
            if (pkt.bytes.empty())
                continue;

            pkt.track_id = track_id_;
            lastAudioPts = pkt.pts_ns;

            {
                std::unique_lock lk(m_state.premux_mutex);
                bool bothReady = m_state.codec_private.VideoReady(m_state.config.video_codec) &&
                                 m_state.codec_private.AudioAllReady(m_state.audio_track_count);
                if (!bothReady) {
                    if (m_state.audio_premux.size() >= SessionState::kAudioPremuxLimit) {
                        lk.unlock();
                        m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                              "Pre-mux audio buffer limit (600 packets) exceeded "
                                              "before codec private data was ready");
                        return false;
                    }
                    m_state.audio_premux.push_back(std::move(pkt));
                    m_state.diagnostics.OnAudioPremuxDepth(static_cast<uint32_t>(m_state.audio_premux.size()));
                } else {
                    lk.unlock();
                    MuxItem mi;
                    mi.payload = std::move(pkt);
                    std::unique_lock mlk(m_state.mux_mutex);
                    // Bounded steady-state queue: block briefly for room, then
                    // fail cleanly — never drop packets or grow without limit.
                    if (!m_state.WaitForMuxQueueSpace(mlk)) {
                        mlk.unlock();
                        m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                              "Mux queue limit exceeded: the output destination "
                                              "cannot keep up with the recording");
                        return false;
                    }
                    m_state.PushMuxItemLocked(std::move(mi));
                }
            }
        }
        return true;
    };

    std::vector<float> floatScratch;

    // A DATA_DISCONTINUITY means the device lost frames BEFORE the flagged
    // packet. Counting it (diagnostics) is not enough: unless the gap is
    // refilled, every packet that follows lands earlier on the sample timeline
    // and the whole remaining track plays ahead of video — a permanent A/V
    // offset that grows with each underrun. Feed the measured gap into the
    // encoder as silence so PTS (derived from the accumulated frame counter)
    // stays continuous. Chunked so a large (clamped) gap never needs a large
    // scratch buffer.
    auto feedGapSilence = [&](uint32_t gap_frames, uint64_t& accumulated_frames,
                              std::vector<EncodedAudioPacket>& out_pkts) {
        constexpr uint32_t kChunkFrames = 4800; // 100 ms at 48 kHz
        const uint32_t first_chunk = gap_frames < kChunkFrames ? gap_frames : kChunkFrames;
        std::vector<float> zeros(static_cast<size_t>(first_chunk) * channels, 0.0f);
        for (uint32_t remaining = gap_frames; remaining > 0;) {
            const uint32_t n = remaining < kChunkFrames ? remaining : kChunkFrames;
            enc.FeedFloat32(zeros.data(), static_cast<size_t>(n) * channels, 0, accumulated_frames, sample_rate,
                            channels, out_pkts);
            remaining -= n;
        }
    };

    uint64_t encoderAccumulatedFrames = 0;
    bool failed = false;

    // --- Capture / encode loop ---
    while (!m_state.stop_requested.load()) {
        if (m_state.pause_requested.load()) {
            while (source_->PendingFrameCount() > 0) {
                RawAudioBuffer raw{};
                std::string err;
                if (source_->AcquireBuffer(raw, err))
                    source_->ReleaseBuffer();
                else
                    break;
            }
            Sleep(1);
            continue;
        }

        uint32_t pendingFrames = source_->PendingFrameCount();
        m_state.diagnostics.OnAudioQueueDepth(pendingFrames);
        if (pendingFrames == 0) {
            Sleep(1);
            continue;
        }

        bool anyWork = false;
        while (source_->PendingFrameCount() > 0) {
            RawAudioBuffer raw{};
            std::string captureErr;
            if (!source_->AcquireBuffer(raw, captureErr)) {
                if (!captureErr.empty()) {
                    const int32_t captureHr = source_->LastCaptureHresult();
                    m_state.RecordFailure(captureHr != 0 ? captureHr : E_FAIL, ErrorPhase::AudioCapture,
                                          "Audio source AcquireBuffer failed: " + captureErr);
                    failed = true;
                }
                break;
            }

            if (raw.data_discontinuity) {
                m_state.diagnostics.OnAudioDiscontinuity();
            }

            std::vector<EncodedAudioPacket> pkts;
            float new_rms = 0.0f;
            if (raw.gap_frames > 0) {
                feedGapSilence(raw.gap_frames, encoderAccumulatedFrames, pkts);
            }
            const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(channels);
            if (raw.silent) {
                std::vector<float> silence(totalSamples, 0.0f);
                enc.FeedFloat32(silence.data(), silence.size(), 0, encoderAccumulatedFrames, sample_rate, channels,
                                pkts);
            } else if (raw.bytes == nullptr) {
                source_->ReleaseBuffer();
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                      "Audio source returned null bytes for non-silent packet");
                failed = true;
                break;
            } else if (source_format == AudioSampleFormat::Float32) {
                new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                enc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, 0, encoderAccumulatedFrames,
                                sample_rate, channels, pkts);
            } else {
                floatScratch.resize(totalSamples);
                ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                      totalSamples);
                new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                enc.FeedFloat32(floatScratch.data(), floatScratch.size(), 0, encoderAccumulatedFrames, sample_rate,
                                channels, pkts);
            }
            m_smoothed_rms_ = kRmsEmaAlpha * new_rms + (1.0f - kRmsEmaAlpha) * m_smoothed_rms_;

            source_->ReleaseBuffer();

            // Update stats
            {
                std::lock_guard slk(m_state.stats_mutex);
                for (const auto& p : pkts) {
                    m_state.stats.audio_packets++;
                    m_state.stats.audio_bytes += p.bytes.size();
                }
                if (track_id_ < m_state.stats.per_track_rms.size()) {
                    m_state.stats.per_track_rms[track_id_] = m_smoothed_rms_;
                }
            }

            if (!routeAudioPackets(pkts)) {
                failed = true;
                break;
            }

            anyWork = true;
        }

        if (failed)
            break;

        if (!anyWork)
            Sleep(1);
    }

    source_->Shutdown();

    // --- Drain the encoder (flush semantics are the encoder's own: Opus pads
    // the final frame, FLAC finishes the stream, AAC drains its delay line,
    // PCM holds no buffered state and emits nothing) ---
    {
        std::vector<EncodedAudioPacket> drainPkts;
        enc.Flush(drainPkts);

        {
            std::lock_guard slk(m_state.stats_mutex);
            for (const auto& p : drainPkts) {
                m_state.stats.audio_packets++;
                m_state.stats.audio_bytes += p.bytes.size();
            }
        }

        routeAudioPackets(drainPkts);
    }

    // --- Update final stats ---
    {
        std::lock_guard lk(m_state.stats_mutex);
        if (lastAudioPts > m_state.stats.audio_duration_ns) {
            m_state.stats.audio_duration_ns = lastAudioPts;
        }
    }

    // --- Push audio EOS sentinel ---
    {
        MuxItem eos;
        eos.payload = AudioEosSentinel{track_id_};
        std::lock_guard lk(m_state.mux_mutex);
        m_state.PushMuxItemLocked(std::move(eos)); // sentinel: bypasses the queue bound
    }
}

} // namespace recorder_core
