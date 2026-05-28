#include "audio_thread.h"

#include "codec_private.h"
#include "mf_aac_encoder.h"
#include "opus_audio_encoder.h"
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

        // QPC-based clock for tracking real time through idle gaps
        LARGE_INTEGER qpcFreq;
        QueryPerformanceFrequency(&qpcFreq);
        uint64_t lastQpc100ns = 0;
        auto Qpc100ns = [&]() -> uint64_t {
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            return static_cast<uint64_t>(qpc.QuadPart) * 10000000ULL / static_cast<uint64_t>(qpcFreq.QuadPart);
        };
        lastQpc100ns = Qpc100ns();

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
            if (pendingFrames == 0) {
                // Advance PTS by wall-clock time so idle gaps don't collapse the timeline
                uint64_t now100ns = Qpc100ns();
                if (now100ns > lastQpc100ns) {
                    uint64_t elapsed_ns = (now100ns - lastQpc100ns) * 100ULL;
                    uint64_t elapsed_frames = elapsed_ns * source_->SampleRate() / 1000000000ULL;
                    encoderAccumulatedFrames += elapsed_frames;
                }
                lastQpc100ns = now100ns;
                Sleep(1);
                continue;
            }
            lastQpc100ns = Qpc100ns();

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

    // --- Init AAC encoder ---
    MfAacEncoder aacEnc;
    {
        std::string err;
        if (!aacEnc.Init(kSampleRate, kChannels, err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "MF AAC encoder init: " + err);
            source_->Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    // --- Derive AAC codec private immediately after Init ---
    {
        char reason[256] = {};
        uint8_t cp[2] = {};
        if (!codec_private::DeriveAacCodecPrivate(aacEnc.OutputMediaType(), cp, reason, sizeof(reason))) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, std::string("AAC codec private: ") + reason);
            aacEnc.Shutdown();
            source_->Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        std::lock_guard lk(m_state.premux_mutex);
        m_state.codec_private.audio_codec_private[track_id_].bytes.assign(cp, cp + 2);
        m_state.codec_private.audio_track_ready[track_id_] = true;
        m_state.premux_cv.notify_all();
    }

    uint64_t audioAccumulatedFrames = 0;

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    uint64_t lastQpc100ns_aac = 0;
    auto Qpc100ns_aac = [&]() -> uint64_t {
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        return static_cast<uint64_t>(qpc.QuadPart) * 10000000ULL / static_cast<uint64_t>(qpcFreq.QuadPart);
    };
    lastQpc100ns_aac = Qpc100ns_aac();

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
        if (pendingFrames == 0) {
            uint64_t now100ns = Qpc100ns_aac();
            if (now100ns > lastQpc100ns_aac) {
                uint64_t elapsed_ns = (now100ns - lastQpc100ns_aac) * 100ULL;
                uint64_t elapsed_frames = elapsed_ns * kSampleRate / 1000000000ULL;
                audioAccumulatedFrames += elapsed_frames;
            }
            lastQpc100ns_aac = now100ns;
            Sleep(1);
            continue;
        }
        lastQpc100ns_aac = Qpc100ns_aac();

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

            uint64_t audio_ts_ns = audioAccumulatedFrames * 1000000000ULL / kSampleRate;
            LONGLONG sampleDuration =
                static_cast<LONGLONG>(raw.num_frames) * 10000000LL / static_cast<LONGLONG>(kSampleRate);

            std::vector<EncodedAudioPacket> pkts;
            float new_rms = 0.0f;
            const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(kChannels);

            if (raw.silent) {
                // Feed silence
                std::vector<uint8_t> silenceData(static_cast<size_t>(raw.num_frames) * aacEnc.InputBlockAlign(), 0);
                aacEnc.FeedRaw(silenceData.data(), static_cast<DWORD>(silenceData.size()), audio_ts_ns, sampleDuration,
                               pkts);
            } else if (raw.bytes == nullptr) {
                source_->ReleaseBuffer();
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                      "Audio source returned null bytes for non-silent packet");
                goto end_audio_loop;
            } else if (aacEnc.UsesPcm16()) {
                if (sourceFormat == AudioSampleFormat::Int16) {
                    new_rms = ComputeRmsFromInt16(reinterpret_cast<const std::int16_t*>(raw.bytes), totalSamples);
                    UINT32 dataBytes = raw.num_frames * aacEnc.InputBlockAlign();
                    aacEnc.FeedRaw(raw.bytes, dataBytes, audio_ts_ns, sampleDuration, pkts);
                } else {
                    new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                    aacEnc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, audio_ts_ns,
                                       audioAccumulatedFrames, kSampleRate, kChannels, pkts);
                    // FeedFloat32 already increments accumulated_frames, reset to avoid double-count
                    audioAccumulatedFrames -= raw.num_frames;
                }
            } else {
                if (sourceFormat == AudioSampleFormat::Int16) {
                    floatScratch.resize(totalSamples);
                    ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                          totalSamples);
                    new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                    aacEnc.FeedFloat32(floatScratch.data(), totalSamples, audio_ts_ns, audioAccumulatedFrames,
                                       kSampleRate, kChannels, pkts);
                    // FeedFloat32 already increments accumulated_frames, reset to avoid double-count
                    audioAccumulatedFrames -= raw.num_frames;
                } else {
                    new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                    UINT32 dataBytes = raw.num_frames * aacEnc.InputBlockAlign();
                    aacEnc.FeedRaw(raw.bytes, dataBytes, audio_ts_ns, sampleDuration, pkts);
                }
            }
            m_smoothed_rms_ = kRmsEmaAlpha * new_rms + (1.0f - kRmsEmaAlpha) * m_smoothed_rms_;

            source_->ReleaseBuffer();
            audioAccumulatedFrames += raw.num_frames;

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
