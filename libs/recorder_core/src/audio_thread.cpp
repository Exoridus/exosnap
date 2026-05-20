#include "audio_thread.h"

#include "codec_private.h"
#include "mf_aac_encoder.h"
#include "session_internal.h"
#include "wasapi_loopback.h"

#include <recorder_core/packet_types.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {

AudioThread::AudioThread(SessionState& state) : m_state(state) {
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

    // --- Init AAC encoder ---
    MfAacEncoder aacEnc;
    {
        std::string err;
        if (!aacEnc.Init(48000, 2, err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, "MF AAC encoder init: " + err);
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
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
        std::lock_guard lk(m_state.premux_mutex);
        std::memcpy(m_state.codec_private.aac_codec_private[0].bytes.data(), cp, 2);
        m_state.codec_private.aac_track_ready[0] = true;
        m_state.premux_cv.notify_all();
    }

    // --- Init WASAPI loopback ---
    WasapiLoopback wasapi;
    {
        std::string err;
        if (!wasapi.Init(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "WASAPI loopback init: " + err);
            aacEnc.Shutdown();
            if (com_inited && hr != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;

    uint64_t audioAccumulatedFrames = 0;
    uint64_t lastAudioPts = 0;

    // Helper: push encoded audio packets to premux or mux queue
    auto routeAudioPackets = [&](std::vector<EncodedAudioPacket>& pkts) {
        for (auto& pkt : pkts) {
            if (pkt.bytes.empty())
                continue;

            lastAudioPts = pkt.pts_ns;

            {
                std::unique_lock lk(m_state.premux_mutex);
                bool bothReady =
                    m_state.codec_private.av1_ready && m_state.codec_private.AudioAllReady(m_state.audio_track_count);
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

    // --- Capture / encode loop ---
    while (!m_state.stop_requested.load()) {
        UINT32 numFrames = wasapi.GetNextPacketSize();
        if (numFrames == 0) {
            Sleep(1);
            continue;
        }

        bool anyWork = false;
        while (wasapi.GetNextPacketSize() > 0) {
            BYTE* pData = nullptr;
            DWORD captureFlags = 0;
            bool silent = false;

            if (!wasapi.GetNextPacket(&pData, &numFrames, &captureFlags, &silent))
                break;

            uint64_t audio_ts_ns = audioAccumulatedFrames * 1000000000ULL / kSampleRate;
            LONGLONG sampleDuration =
                static_cast<LONGLONG>(numFrames) * 10000000LL / static_cast<LONGLONG>(kSampleRate);

            std::vector<EncodedAudioPacket> pkts;

            if (silent) {
                // Feed silence
                std::vector<uint8_t> silenceData(numFrames * aacEnc.InputBlockAlign(), 0);
                aacEnc.FeedRaw(silenceData.data(), static_cast<DWORD>(silenceData.size()), audio_ts_ns, sampleDuration,
                               pkts);
            } else if (aacEnc.UsesPcm16()) {
                // Convert float32 -> pcm16 then feed
                size_t totalSamples = static_cast<size_t>(numFrames) * kChannels;
                aacEnc.FeedFloat32(reinterpret_cast<const float*>(pData), totalSamples, audio_ts_ns,
                                   audioAccumulatedFrames, kSampleRate, kChannels, pkts);
                // FeedFloat32 already increments accumulated_frames, reset to avoid double-count
                audioAccumulatedFrames -= numFrames;
            } else {
                // Feed raw bytes
                UINT32 dataBytes = numFrames * aacEnc.InputBlockAlign();
                aacEnc.FeedRaw(pData, dataBytes, audio_ts_ns, sampleDuration, pkts);
            }

            wasapi.ReleasePacket(numFrames);
            audioAccumulatedFrames += numFrames;

            // Update stats
            {
                std::lock_guard slk(m_state.stats_mutex);
                for (const auto& p : pkts) {
                    m_state.stats.audio_packets++;
                    m_state.stats.audio_bytes += p.bytes.size();
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
    // --- Stop WASAPI ---
    wasapi.Shutdown();

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
        m_state.stats.audio_duration_ns = lastAudioPts;
    }

    // --- Push audio EOS sentinel ---
    {
        MuxItem eos;
        eos.payload = AudioEosSentinel{0};
        std::lock_guard lk(m_state.mux_mutex);
        m_state.mux_queue.push_back(std::move(eos));
        m_state.mux_cv.notify_one();
    }

    aacEnc.Shutdown();
    if (com_inited && hr != RPC_E_CHANGED_MODE)
        CoUninitialize();
}

} // namespace recorder_core
