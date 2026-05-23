#include "mp4_mux_thread.h"

#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <windows.h>

#pragma comment(lib, "mfreadwrite.lib")

namespace recorder_core {

namespace {

// Build a HEAACWAVEINFO-style MF_MT_USER_DATA blob from the 2-byte AudioSpecificConfig.
static void BuildAacUserData(const uint8_t asc[2], uint8_t out[14]) noexcept {
    std::memset(out, 0, 14);
    out[0] = 0x00; // wPayloadType = 0 (RAW, little-endian lo)
    out[1] = 0x00;
    out[2] = 0x29; // wAudioProfileLevelIndication = 0x29 (AAC-LC stereo 48 kHz, little-endian lo)
    out[3] = 0x00;
    out[12] = asc[0];
    out[13] = asc[1];
}

// Write one IMFSample containing the provided bytes to the given stream index.
static HRESULT WriteSampleBytes(IMFSinkWriter* pWriter, DWORD streamIdx, const uint8_t* data, size_t size,
                                LONGLONG sampleTime100ns, LONGLONG duration100ns, bool isKey) {
    IMFMediaBuffer* pBuf = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &pBuf);
    if (FAILED(hr))
        return hr;

    BYTE* pDst = nullptr;
    hr = pBuf->Lock(&pDst, nullptr, nullptr);
    if (FAILED(hr)) {
        pBuf->Release();
        return hr;
    }
    std::memcpy(pDst, data, size);
    hr = pBuf->Unlock();
    if (FAILED(hr)) {
        pBuf->Release();
        return hr;
    }
    hr = pBuf->SetCurrentLength(static_cast<DWORD>(size));
    if (FAILED(hr)) {
        pBuf->Release();
        return hr;
    }

    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) {
        pBuf->Release();
        return hr;
    }
    hr = pSample->AddBuffer(pBuf);
    pBuf->Release();
    if (FAILED(hr)) {
        pSample->Release();
        return hr;
    }

    hr = pSample->SetSampleTime(sampleTime100ns);
    if (FAILED(hr)) {
        pSample->Release();
        return hr;
    }
    hr = pSample->SetSampleDuration(duration100ns);
    if (FAILED(hr)) {
        pSample->Release();
        return hr;
    }
    if (isKey) {
        hr = pSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (FAILED(hr)) {
            pSample->Release();
            return hr;
        }
    }

    hr = pWriter->WriteSample(streamIdx, pSample);
    pSample->Release();
    return hr;
}

} // namespace

// ---------------------------------------------------------------------------
// Mp4MuxThread
// ---------------------------------------------------------------------------

Mp4MuxThread::Mp4MuxThread(SessionState& state) : m_state(state) {
}

Mp4MuxThread::~Mp4MuxThread() {
    if (m_thread.joinable())
        m_thread.detach();
}

void Mp4MuxThread::Start() {
    m_thread = std::thread([this] { Run(); });
}

bool Mp4MuxThread::Join(unsigned timeout_ms) {
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

void Mp4MuxThread::Run() {
    auto coinit_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comOwned = SUCCEEDED(coinit_hr);
    if (FAILED(coinit_hr) && coinit_hr != RPC_E_CHANGED_MODE) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Mp4MuxThread: CoInitializeEx failed 0x%08lX",
                 static_cast<unsigned long>(coinit_hr));
        m_state.RecordFailure(static_cast<int32_t>(coinit_hr), ErrorPhase::Mux, buf);
        return;
    }

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Mp4MuxThread: MFStartup failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
        if (comOwned)
            CoUninitialize();
        return;
    }

    // --- Step 1: Wait for H264 codec private and audio codec private ---
    uint32_t track_count = 0;
    {
        std::unique_lock lk(m_state.premux_mutex);
        m_state.premux_cv.wait(lk, [&] {
            return (m_state.codec_private.VideoReady(m_state.config.video_codec) &&
                    m_state.codec_private.AudioAllReady(m_state.audio_track_count)) ||
                   m_state.stop_requested.load();
        });

        if (!(m_state.codec_private.VideoReady(m_state.config.video_codec) &&
              m_state.codec_private.AudioAllReady(m_state.audio_track_count))) {
            lk.unlock();
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "H264 codec private not available at MP4 mux start");
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }
        track_count = m_state.audio_track_count;
    }

    if (track_count > CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Mux, "Audio track count exceeds maximum");
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // Copy codec private data
    std::vector<uint8_t> h264SpsPps;
    std::array<AudioCodecPrivateSlot, CodecPrivateData::kMaxAudioTracks> audioCp{};
    {
        std::lock_guard lk(m_state.premux_mutex);
        h264SpsPps = m_state.codec_private.h264_sps_pps;
        for (uint32_t i = 0; i < track_count; ++i) {
            audioCp[i] = m_state.codec_private.audio_codec_private[i];
        }
    }

    if (h264SpsPps.empty()) {
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "H264 SPS/PPS is empty");
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    uint32_t encW = 0, encH = 0;
    {
        std::lock_guard lk(m_state.stats_mutex);
        encW = m_state.encode_width;
        encH = m_state.encode_height;
    }

    // --- Step 2: Collect premux packets ---
    struct InterleavePacket {
        uint64_t pts_ns = 0;
        uint32_t stream_id = 0; // 0=video, 1+track_id=audio
        bool is_key = false;
        std::vector<uint8_t> bytes;
    };

    std::vector<InterleavePacket> premuxPackets;
    {
        std::lock_guard lk(m_state.premux_mutex);
        premuxPackets.reserve(m_state.video_premux.size() + m_state.audio_premux.size());
        for (auto& pkt : m_state.video_premux) {
            if (pkt.bytes.empty())
                continue;
            InterleavePacket ip;
            ip.pts_ns = pkt.pts_ns;
            ip.stream_id = 0;
            ip.is_key = pkt.keyframe;
            ip.bytes = std::move(pkt.bytes);
            premuxPackets.push_back(std::move(ip));
        }
        for (auto& pkt : m_state.audio_premux) {
            if (pkt.bytes.empty())
                continue;
            if (pkt.track_id >= track_count || pkt.track_id >= CodecPrivateData::kMaxAudioTracks)
                continue;
            InterleavePacket ip;
            ip.pts_ns = pkt.pts_ns;
            ip.stream_id = 1 + pkt.track_id;
            ip.is_key = true;
            ip.bytes = std::move(pkt.bytes);
            premuxPackets.push_back(std::move(ip));
        }
        m_state.video_premux.clear();
        m_state.audio_premux.clear();
    }

    // --- Compute A/V timestamp alignment offset ---
    // Audio PTS originates from AudioThread init (~session start).
    // Video PTS originates from the first WGC frame (after WGC init delay).
    // head_start_ns = WGC init delay in nanoseconds.
    // Audio packets before head_start_ns predate the first video frame and are dropped.
    uint64_t head_start_ns = 0;
    {
        const uint64_t video_epoch = m_state.video_epoch_qpc_100ns.load();
        const uint64_t session_start = m_state.session_start_qpc_100ns;
        if (video_epoch > session_start) {
            head_start_ns = (video_epoch - session_start) * 100ULL;
        }
    }
    std::printf("[MP4] A/V alignment: head_start_ns=%llu (%.1f ms)\n", static_cast<unsigned long long>(head_start_ns),
                static_cast<double>(head_start_ns) / 1e6);

    // Normalize premux audio: drop packets before video epoch, shift remaining
    if (head_start_ns > 0) {
        uint32_t trimmed = 0;
        std::vector<InterleavePacket> kept;
        kept.reserve(premuxPackets.size());
        for (auto& ip : premuxPackets) {
            if (ip.stream_id != 0) {
                if (ip.pts_ns < head_start_ns) {
                    ++trimmed;
                    continue;
                }
                ip.pts_ns -= head_start_ns;
            }
            kept.push_back(std::move(ip));
        }
        premuxPackets = std::move(kept);
        if (trimmed > 0) {
            std::printf("[MP4] Trimmed %u premux audio packets before video epoch\n", trimmed);
        }
    }

    std::stable_sort(premuxPackets.begin(), premuxPackets.end(),
                     [](const InterleavePacket& a, const InterleavePacket& b) { return a.pts_ns < b.pts_ns; });

    if (m_state.HasFailure() && premuxPackets.empty()) {
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // --- Step 3: Open IMFSinkWriter ---
    std::wstring outPath = m_state.config.output_path.wstring();

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 2);
    if (pAttr) {
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        // Disable real-time throttling — we write file data, not a live stream.
        // Without this, WriteSample blocks until each sample's PTS has elapsed,
        // causing the finalize to take as long as the recording duration.
        pAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }

    IMFSinkWriter* pWriter = nullptr;
    hr = MFCreateSinkWriterFromURL(outPath.c_str(), nullptr, pAttr, &pWriter);
    if (pAttr) {
        pAttr->Release();
        pAttr = nullptr;
    }
    if (FAILED(hr) || !pWriter) {
        char buf[128];
        snprintf(buf, sizeof(buf), "MFCreateSinkWriterFromURL failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // --- Step 4: Configure video stream ---
    DWORD videoStreamIdx = 0;
    {
        IMFMediaType* pVideoType = nullptr;
        hr = MFCreateMediaType(&pVideoType);
        if (FAILED(hr)) {
            pWriter->Release();
            m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, "MFCreateMediaType(video) failed");
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }

        bool ok = true;
        ok = ok && SUCCEEDED(pVideoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        ok = ok && SUCCEEDED(pVideoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
        ok = ok && SUCCEEDED(pVideoType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        ok = ok && SUCCEEDED(MFSetAttributeSize(pVideoType, MF_MT_FRAME_SIZE, encW, encH));
        ok = ok && SUCCEEDED(MFSetAttributeRatio(pVideoType, MF_MT_FRAME_RATE, m_state.config.frame_rate_num,
                                                 m_state.config.frame_rate_den));
        ok = ok && SUCCEEDED(MFSetAttributeRatio(pVideoType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        ok = ok && SUCCEEDED(pVideoType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, h264SpsPps.data(),
                                                 static_cast<UINT32>(h264SpsPps.size())));

        if (ok) {
            hr = pWriter->AddStream(pVideoType, &videoStreamIdx);
            ok = SUCCEEDED(hr);
        }
        if (ok) {
            hr = pWriter->SetInputMediaType(videoStreamIdx, pVideoType, nullptr);
            ok = SUCCEEDED(hr);
        }
        pVideoType->Release();

        if (!ok) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Video stream config failed 0x%08lX", static_cast<unsigned long>(hr));
            pWriter->Release();
            m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }
    }

    // --- Step 5: Configure audio streams ---
    std::array<DWORD, CodecPrivateData::kMaxAudioTracks> audioStreamIdxs{};
    for (uint32_t i = 0; i < track_count; ++i) {
        const auto& slot = audioCp[i];
        if (slot.bytes.size() < 2) {
            pWriter->Release();
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                  "AAC codec private too small for MP4 track " + std::to_string(i));
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }

        uint8_t userData[14] = {};
        BuildAacUserData(slot.bytes.data(), userData);

        IMFMediaType* pAudioType = nullptr;
        hr = MFCreateMediaType(&pAudioType);
        if (FAILED(hr)) {
            pWriter->Release();
            m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, "MFCreateMediaType(audio) failed");
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }

        bool ok = true;
        ok = ok && SUCCEEDED(pAudioType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
        ok = ok && SUCCEEDED(pAudioType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000));
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0)); // RAW
        ok = ok && SUCCEEDED(pAudioType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29));
        ok = ok && SUCCEEDED(pAudioType->SetBlob(MF_MT_USER_DATA, userData, sizeof(userData)));

        DWORD audioIdx = 0;
        if (ok) {
            hr = pWriter->AddStream(pAudioType, &audioIdx);
            ok = SUCCEEDED(hr);
        }
        if (ok) {
            hr = pWriter->SetInputMediaType(audioIdx, pAudioType, nullptr);
            ok = SUCCEEDED(hr);
        }
        pAudioType->Release();

        if (!ok) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Audio stream %u config failed 0x%08lX", i, static_cast<unsigned long>(hr));
            pWriter->Release();
            m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
            MFShutdown();
            if (comOwned)
                CoUninitialize();
            return;
        }

        audioStreamIdxs[i] = audioIdx;
    }

    // --- Step 6: Begin writing ---
    hr = pWriter->BeginWriting();
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IMFSinkWriter::BeginWriting failed 0x%08lX", static_cast<unsigned long>(hr));
        pWriter->Release();
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    const LONGLONG kVideoFrameDuration100ns =
        (m_state.config.frame_rate_num > 0)
            ? static_cast<LONGLONG>(10000000LL * m_state.config.frame_rate_den / m_state.config.frame_rate_num)
            : 166667LL; // ~60 fps fallback
    // AAC-LC 1024 frames at 48 kHz
    constexpr LONGLONG kAudioFrameDuration100ns = 213333LL;

    bool writeError = false;

    // Helper: write one normalized sample to IMFSinkWriter.
    // apply_norm=true: subtract head_start_ns from audio PTS; drop audio before video epoch.
    // apply_norm=false: premux packets already have normalized PTS, write directly.
    // Returns false if WriteSample failed (failure already recorded; pWriter already released).
    auto writeSample = [&](uint64_t pts_ns, uint32_t stream_id, bool is_key, const uint8_t* data, size_t size,
                           bool apply_norm) -> bool {
        if (apply_norm && stream_id != 0) {
            if (pts_ns < head_start_ns)
                return true; // drop pre-video audio — not an error
            pts_ns -= head_start_ns;
        }
        const LONGLONG pts100ns = static_cast<LONGLONG>(pts_ns / 100ULL);
        const LONGLONG dur100ns = (stream_id == 0) ? kVideoFrameDuration100ns : kAudioFrameDuration100ns;
        const DWORD stIdx = (stream_id == 0) ? videoStreamIdx : audioStreamIdxs[stream_id - 1];
        const HRESULT whr = WriteSampleBytes(pWriter, stIdx, data, size, pts100ns, dur100ns, is_key);
        if (FAILED(whr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WriteSample(stream=%u) failed 0x%08lX", stream_id,
                     static_cast<unsigned long>(whr));
            pWriter->Finalize();
            pWriter->Release();
            pWriter = nullptr;
            m_state.RecordFailure(static_cast<int32_t>(whr), ErrorPhase::Mux, buf);
            writeError = true;
            return false;
        }
        return true;
    };

    // --- Step 7: Write premux packets immediately ---
    const auto tPremuxStart = std::chrono::steady_clock::now();
    for (const auto& ip : premuxPackets) {
        if (ip.bytes.empty() || writeError)
            break;
        if (!writeSample(ip.pts_ns, ip.stream_id, ip.is_key, ip.bytes.data(), ip.bytes.size(), false))
            break;
    }
    premuxPackets.clear();

    if (writeError) {
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // --- Step 8: Incremental drain loop ---
    // Packets are written to IMFSinkWriter as they arrive rather than batched post-EOS.
    // stopDeadline is 5 s after Stop() — tight enough to avoid long hangs while generous
    // enough for encoder drain (NVENC flush + MF AAC drain).
    bool videoEos = false;
    std::array<bool, CodecPrivateData::kMaxAudioTracks> audioEosReceived{};
    const auto allAudioEos = [&]() {
        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audioEosReceived[i])
                return false;
        }
        return true;
    };

    auto stopDeadline = (std::chrono::steady_clock::time_point::max)();
    const auto tDrainStart = std::chrono::steady_clock::now();

    while (!(videoEos && allAudioEos())) {
        if (writeError || m_state.HasFailure())
            break;
        if (m_state.stop_requested.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (stopDeadline == (std::chrono::steady_clock::time_point::max)())
                stopDeadline = now + std::chrono::seconds(5);
            else if (now >= stopDeadline)
                break;
        }
        {
            std::unique_lock lk(m_state.mux_mutex);
            m_state.mux_cv.wait_for(lk, std::chrono::milliseconds(5),
                                    [&] { return !m_state.mux_queue.empty() || m_state.stop_requested.load(); });
            if (writeError || m_state.HasFailure())
                break;
            while (!m_state.mux_queue.empty()) {
                MuxItem item = std::move(m_state.mux_queue.front());
                m_state.mux_queue.pop_front();
                lk.unlock();
                std::visit(
                    [&](auto&& payload) {
                        using T = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                            if (!payload.bytes.empty() && !writeError) {
                                writeSample(payload.pts_ns, 0, payload.keyframe, payload.bytes.data(),
                                            payload.bytes.size(), false);
                            }
                        } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                            if (!payload.bytes.empty() && !writeError && payload.track_id < track_count &&
                                payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                                writeSample(payload.pts_ns, 1 + payload.track_id, true, payload.bytes.data(),
                                            payload.bytes.size(), true);
                            }
                        } else if constexpr (std::is_same_v<T, VideoEosSentinel>) {
                            videoEos = true;
                        } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
                            if (payload.track_id < track_count &&
                                payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                                audioEosReceived[payload.track_id] = true;
                            }
                        }
                    },
                    item.payload);
                lk.lock();
            }
        }
    }

    const auto tDrainEnd = std::chrono::steady_clock::now();
    std::printf("[MP4] Drain loop: %.3f s (premux_write=%.3f s, drain=%.3f s)%s\n",
                std::chrono::duration<double>(tDrainEnd - tPremuxStart).count(),
                std::chrono::duration<double>(tDrainStart - tPremuxStart).count(),
                std::chrono::duration<double>(tDrainEnd - tDrainStart).count(),
                (videoEos && allAudioEos()) ? "" : " [EOS timeout]");

    // Final drain: flush remaining data packets after loop exits (timeout/failure path)
    if (!writeError) {
        std::lock_guard lk(m_state.mux_mutex);
        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            std::visit(
                [&](auto&& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                        if (!payload.bytes.empty() && !writeError) {
                            writeSample(payload.pts_ns, 0, payload.keyframe, payload.bytes.data(), payload.bytes.size(),
                                        false);
                        }
                    } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                        if (!payload.bytes.empty() && !writeError && payload.track_id < track_count &&
                            payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                            writeSample(payload.pts_ns, 1 + payload.track_id, true, payload.bytes.data(),
                                        payload.bytes.size(), true);
                        }
                    }
                    // EOS sentinels: ignored in final drain
                },
                item.payload);
        }
    }

    if (writeError) {
        // pWriter already released and failure recorded inside writeSample
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // --- Step 9: Finalize ---
    const auto tFinalizeStart = std::chrono::steady_clock::now();
    hr = pWriter->Finalize();
    pWriter->Release();
    pWriter = nullptr;
    const auto tFinalizeEnd = std::chrono::steady_clock::now();
    std::printf("[MP4] Finalize: %.3f s\n", std::chrono::duration<double>(tFinalizeEnd - tFinalizeStart).count());

    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IMFSinkWriter::Finalize failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Finalize, buf);
        MFShutdown();
        if (comOwned)
            CoUninitialize();
        return;
    }

    // Update output file size
    {
        WIN32_FILE_ATTRIBUTE_DATA fd{};
        if (GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &fd)) {
            uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.output_file_bytes = sz;
        }
    }

    MFShutdown();
    if (comOwned)
        CoUninitialize();
}

} // namespace recorder_core
