#include "audio_thread.h"

#include "codec_private.h"
#include "fdk_aac_encoder.h"
#include "mf_aac_encoder.h"
#include "opus_audio_encoder.h"
#include "pcm_audio_encoder.h"
#include "recorder_core/audio_meter.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include <cstdint>
#include <cstdio>

namespace recorder_core {

namespace {

constexpr float kRmsEmaAlpha = 0.3f;

void ConvertInt16ToFloat32(const std::int16_t* src, float* dst, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<float>(src[i]) / 32768.0f;
    }
}

} // namespace

AudioThread::AudioThread(SessionState& state, std::unique_ptr<IAudioCaptureSource> source, uint32_t track_id)
    : m_state(state), source_(std::move(source)), track_id_(track_id) {
}

AudioThread::~AudioThread() {
    if (m_thread.joinable())
        m_thread.detach();
}

void AudioThread::Start() {
    m_thread = std::thread([this] { Run(); });
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

    if (!source_) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio source is null");
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    if (track_id_ >= CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio track id is out of range");
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    {
        std::string err;
        if (!source_->Init(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source init failed: " + err);
            source_->Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    const uint32_t kSampleRate = source_->SampleRate();
    const uint32_t kChannels = source_->Channels();
    const AudioSampleFormat sourceFormat = source_->SampleFormat();

    if (kSampleRate == 0 || kChannels == 0) {
        m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source reported invalid stream format");
        source_->Shutdown();
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    uint64_t lastAudioPts = 0;

    m_state.diagnostics.SetAudioFormat(kSampleRate, kChannels);

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
                    std::lock_guard mlk(m_state.mux_mutex);
                    m_state.mux_queue.push_back(std::move(mi));
                    m_state.mux_cv.notify_one();
                }
            }
        }
        return true;
    };

    std::vector<float> floatScratch;

    if (m_state.config.audio_codec == AudioCodec::Opus) {
        // --- Opus encoder init ---
        OpusAudioEncoder opusEnc;
        opusEnc.SetEncodingParams(m_state.config.audio_bitrate_kbps, m_state.config.opus_frame_duration,
                                  m_state.config.opus_complexity);
        {
            std::string err;
            if (!opusEnc.Init(kSampleRate, kChannels, err)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "Opus encoder init: " + err);
                source_->Shutdown();
                if (com_inited && hr != RPC_E_CHANGED_MODE)
                    CoUninitialize();
                return;
            }
        }

        {
            std::lock_guard lk(m_state.premux_mutex);
            m_state.codec_private.audio_codec_private[track_id_].bytes = opusEnc.CodecPrivateBytes();
            m_state.codec_private.audio_track_ready[track_id_] = true;
            m_state.premux_cv.notify_all();
        }

        uint64_t encoderAccumulatedFrames = 0;

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
                        m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                              "Audio source AcquireBuffer failed: " + captureErr);
                        goto end_opus_loop;
                    }
                    break;
                }

                if (raw.data_discontinuity) {
                    m_state.diagnostics.OnAudioDiscontinuity();
                }

                std::vector<EncodedAudioPacket> pkts;
                float new_rms = 0.0f;
                if (raw.silent) {
                    std::vector<float> silence(static_cast<size_t>(raw.num_frames) * kChannels, 0.0f);
                    opusEnc.FeedFloat32(silence.data(), silence.size(), 0, encoderAccumulatedFrames, kSampleRate,
                                        kChannels, pkts);
                } else if (raw.bytes == nullptr) {
                    source_->ReleaseBuffer();
                    m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                          "Audio source returned null bytes for non-silent packet");
                    goto end_opus_loop;
                } else if (sourceFormat == AudioSampleFormat::Float32) {
                    const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(kChannels);
                    new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                    opusEnc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, 0,
                                        encoderAccumulatedFrames, kSampleRate, kChannels, pkts);
                } else {
                    const size_t totalSamples = static_cast<size_t>(raw.num_frames) * kChannels;
                    floatScratch.resize(totalSamples);
                    ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                          totalSamples);
                    new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                    opusEnc.FeedFloat32(floatScratch.data(), floatScratch.size(), 0, encoderAccumulatedFrames,
                                        kSampleRate, kChannels, pkts);
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

                if (!routeAudioPackets(pkts))
                    goto end_opus_loop;

                anyWork = true;
            }

            if (!anyWork)
                Sleep(1);
        }

    end_opus_loop:
        source_->Shutdown();

        // --- Drain Opus encoder ---
        {
            std::vector<EncodedAudioPacket> drainPkts;
            opusEnc.Flush(drainPkts);

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
            m_state.mux_queue.push_back(std::move(eos));
            m_state.mux_cv.notify_one();
        }

        opusEnc.Shutdown();
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    if (m_state.config.audio_codec == AudioCodec::Pcm) {
        // --- PCM passthrough "encoder" init (MKV-only A_PCM/INT_LIT) ---
        PcmAudioEncoder pcmEnc;
        {
            std::string err;
            if (!pcmEnc.Init(kSampleRate, kChannels, err)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "PCM encoder init: " + err);
                source_->Shutdown();
                if (com_inited && hr != RPC_E_CHANGED_MODE)
                    CoUninitialize();
                return;
            }
        }

        // PCM has no CodecPrivate; mark the track ready with empty bytes so the
        // mux thread's codec-private readiness gate releases the pre-mux buffer.
        {
            std::lock_guard lk(m_state.premux_mutex);
            m_state.codec_private.audio_codec_private[track_id_].bytes = pcmEnc.CodecPrivateBytes();
            m_state.codec_private.audio_track_ready[track_id_] = true;
            m_state.premux_cv.notify_all();
        }

        uint64_t encoderAccumulatedFrames = 0;

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
                        m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                              "Audio source AcquireBuffer failed: " + captureErr);
                        goto end_pcm_loop;
                    }
                    break;
                }

                if (raw.data_discontinuity) {
                    m_state.diagnostics.OnAudioDiscontinuity();
                }

                std::vector<EncodedAudioPacket> pkts;
                float new_rms = 0.0f;
                const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(kChannels);
                if (raw.silent) {
                    std::vector<float> silence(totalSamples, 0.0f);
                    pcmEnc.FeedFloat32(silence.data(), silence.size(), 0, encoderAccumulatedFrames, kSampleRate,
                                       kChannels, pkts);
                } else if (raw.bytes == nullptr) {
                    source_->ReleaseBuffer();
                    m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                          "Audio source returned null bytes for non-silent packet");
                    goto end_pcm_loop;
                } else if (sourceFormat == AudioSampleFormat::Float32) {
                    new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                    pcmEnc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, 0,
                                       encoderAccumulatedFrames, kSampleRate, kChannels, pkts);
                } else {
                    floatScratch.resize(totalSamples);
                    ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                          totalSamples);
                    new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                    pcmEnc.FeedFloat32(floatScratch.data(), floatScratch.size(), 0, encoderAccumulatedFrames,
                                       kSampleRate, kChannels, pkts);
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

                if (!routeAudioPackets(pkts))
                    goto end_pcm_loop;

                anyWork = true;
            }

            if (!anyWork)
                Sleep(1);
        }

    end_pcm_loop:
        source_->Shutdown();

        // --- Drain PCM encoder (no-op; PCM holds no buffered state) ---
        {
            std::vector<EncodedAudioPacket> drainPkts;
            pcmEnc.Flush(drainPkts);
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
            m_state.mux_queue.push_back(std::move(eos));
            m_state.mux_cv.notify_one();
        }

        pcmEnc.Shutdown();
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
        return;
    }

    // --- Init AAC encoder ---
    FdkAacEncoder aacEnc;
    aacEnc.SetBitrateKbps(m_state.config.audio_bitrate_kbps);
    {
        std::string err;
        if (!aacEnc.Init(kSampleRate, kChannels, err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "FDK-AAC encoder init: " + err);
            source_->Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // --- Read AAC codec private from FDK-AAC encoder ---
    {
        auto cp = aacEnc.CodecPrivateBytes();
        if (cp.empty()) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "FDK-AAC codec private is empty after Init");
            aacEnc.Shutdown();
            source_->Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        std::lock_guard lk(m_state.premux_mutex);
        m_state.codec_private.audio_codec_private[track_id_].bytes = std::move(cp);
        m_state.codec_private.audio_track_ready[track_id_] = true;
        m_state.premux_cv.notify_all();
    }

    uint64_t audioAccumulatedFrames = 0;

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
                    m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                          "Audio source AcquireBuffer failed: " + captureErr);
                    goto end_audio_loop;
                }
                break;
            }

            if (raw.data_discontinuity) {
                m_state.diagnostics.OnAudioDiscontinuity();
            }

            std::vector<EncodedAudioPacket> pkts;
            float new_rms = 0.0f;
            const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(kChannels);

            if (raw.silent) {
                std::vector<float> silence(totalSamples, 0.0f);
                aacEnc.FeedFloat32(silence.data(), silence.size(), 0, audioAccumulatedFrames, kSampleRate, kChannels,
                                   pkts);
            } else if (raw.bytes == nullptr) {
                source_->ReleaseBuffer();
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                      "Audio source returned null bytes for non-silent packet");
                goto end_audio_loop;
            } else if (sourceFormat == AudioSampleFormat::Float32) {
                new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                aacEnc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, 0, audioAccumulatedFrames,
                                   kSampleRate, kChannels, pkts);
            } else {
                floatScratch.resize(totalSamples);
                ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                      totalSamples);
                new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                aacEnc.FeedFloat32(floatScratch.data(), floatScratch.size(), 0, audioAccumulatedFrames, kSampleRate,
                                   kChannels, pkts);
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

            if (!routeAudioPackets(pkts))
                goto end_audio_loop;

            anyWork = true;
        }

        if (!anyWork)
            Sleep(1);
    }

end_audio_loop:
    source_->Shutdown();

    // --- Drain AAC encoder ---
    {
        std::vector<EncodedAudioPacket> drainPkts;
        aacEnc.Flush(drainPkts);

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
        m_state.mux_queue.push_back(std::move(eos));
        m_state.mux_cv.notify_one();
    }

    aacEnc.Shutdown();
    if (com_inited && hr != RPC_E_CHANGED_MODE)
        CoUninitialize();
}

} // namespace recorder_core
