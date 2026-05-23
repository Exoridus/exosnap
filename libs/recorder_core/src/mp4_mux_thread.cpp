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

// Minimal IMFByteStream wrapper backed by a std::vector — not used here;
// we write directly to a URL via MFCreateSinkWriterFromURL.

// Build a HEAACWAVEINFO-style MF_MT_USER_DATA blob from the 2-byte AudioSpecificConfig.
// Format: [wPayloadType(2) | wAudioProfileLevelIndication(2) | wStructType(2) |
//          wReserved1(2) | dwReserved2(4) | AudioSpecificConfig[0..1]]
static void BuildAacUserData(const uint8_t asc[2], uint8_t out[14]) noexcept {
    std::memset(out, 0, 14);
    out[0] = 0x00; // wPayloadType = 0 (RAW, little-endian lo)
    out[1] = 0x00;
    out[2] = 0x29; // wAudioProfileLevelIndication = 0x29 (AAC-LC stereo 48 kHz, little-endian lo)
    out[3] = 0x00;
    // wStructType, wReserved1, dwReserved2 = 0 (already zeroed)
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
    pBuf->Lock(&pDst, nullptr, nullptr);
    std::memcpy(pDst, data, size);
    pBuf->Unlock();
    pBuf->SetCurrentLength(static_cast<DWORD>(size));

    IMFSample* pSample = nullptr;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) {
        pBuf->Release();
        return hr;
    }
    pSample->AddBuffer(pBuf);
    pBuf->Release();

    pSample->SetSampleTime(sampleTime100ns);
    pSample->SetSampleDuration(duration100ns);
    if (isKey)
        pSample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

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
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!comInited) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Mp4MuxThread: CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
        return;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Mp4MuxThread: MFStartup failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
        if (comInited && hr != RPC_E_CHANGED_MODE)
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
            if (comInited)
                CoUninitialize();
            return;
        }
        track_count = m_state.audio_track_count;
    }

    if (track_count > CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Mux, "Audio track count exceeds maximum");
        MFShutdown();
        if (comInited)
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
        if (comInited)
            CoUninitialize();
        return;
    }

    uint32_t encW = 0, encH = 0;
    {
        std::lock_guard lk(m_state.stats_mutex);
        encW = m_state.encode_width;
        encH = m_state.encode_height;
    }

    // --- Step 2: Collect all packets ---
    struct InterleavePacket {
        uint64_t pts_ns = 0;
        uint32_t stream_id = 0; // 0=video, 1+track_id=audio
        bool is_key = false;
        std::vector<uint8_t> bytes;
    };

    std::vector<InterleavePacket> allPackets;

    {
        std::lock_guard lk(m_state.premux_mutex);
        for (auto& pkt : m_state.video_premux) {
            if (pkt.bytes.empty())
                continue;
            InterleavePacket ip;
            ip.pts_ns = pkt.pts_ns;
            ip.stream_id = 0;
            ip.is_key = pkt.keyframe;
            ip.bytes = std::move(pkt.bytes);
            allPackets.push_back(std::move(ip));
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
            allPackets.push_back(std::move(ip));
        }
        m_state.video_premux.clear();
        m_state.audio_premux.clear();
    }

    bool videoEos = false;
    std::array<bool, CodecPrivateData::kMaxAudioTracks> audioEosReceived{};
    const auto allAudioEos = [&]() {
        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audioEosReceived[i])
                return false;
        }
        return true;
    };

    while (!(videoEos && allAudioEos())) {
        if (m_state.HasFailure())
            break;
        {
            std::unique_lock lk(m_state.mux_mutex);
            m_state.mux_cv.wait_for(lk, std::chrono::milliseconds(5),
                                    [&] { return !m_state.mux_queue.empty() || m_state.stop_requested.load(); });
            if (m_state.HasFailure())
                break;
            while (!m_state.mux_queue.empty()) {
                MuxItem item = std::move(m_state.mux_queue.front());
                m_state.mux_queue.pop_front();
                lk.unlock();
                std::visit(
                    [&](auto&& payload) {
                        using T = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                            if (!payload.bytes.empty()) {
                                InterleavePacket ip;
                                ip.pts_ns = payload.pts_ns;
                                ip.stream_id = 0;
                                ip.is_key = payload.keyframe;
                                ip.bytes = std::move(payload.bytes);
                                allPackets.push_back(std::move(ip));
                            }
                        } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                            if (!payload.bytes.empty() && payload.track_id < track_count &&
                                payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                                InterleavePacket ip;
                                ip.pts_ns = payload.pts_ns;
                                ip.stream_id = 1 + payload.track_id;
                                ip.is_key = true;
                                ip.bytes = std::move(payload.bytes);
                                allPackets.push_back(std::move(ip));
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

    // Final drain
    {
        std::lock_guard lk(m_state.mux_mutex);
        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            std::visit(
                [&](auto&& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                        if (!payload.bytes.empty()) {
                            InterleavePacket ip;
                            ip.pts_ns = payload.pts_ns;
                            ip.stream_id = 0;
                            ip.is_key = payload.keyframe;
                            ip.bytes = std::move(payload.bytes);
                            allPackets.push_back(std::move(ip));
                        }
                    } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                        if (!payload.bytes.empty() && payload.track_id < track_count &&
                            payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                            InterleavePacket ip;
                            ip.pts_ns = payload.pts_ns;
                            ip.stream_id = 1 + payload.track_id;
                            ip.is_key = true;
                            ip.bytes = std::move(payload.bytes);
                            allPackets.push_back(std::move(ip));
                        }
                    } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
                        if (payload.track_id < track_count && payload.track_id < CodecPrivateData::kMaxAudioTracks) {
                            audioEosReceived[payload.track_id] = true;
                        }
                    }
                },
                item.payload);
        }
    }

    std::stable_sort(allPackets.begin(), allPackets.end(),
                     [](const InterleavePacket& a, const InterleavePacket& b) { return a.pts_ns < b.pts_ns; });

    if (m_state.HasFailure() && allPackets.empty()) {
        MFShutdown();
        if (comInited)
            CoUninitialize();
        return;
    }

    // --- Step 3: Open IMFSinkWriter ---
    std::wstring outPath = m_state.config.output_path.wstring();

    IMFAttributes* pAttr = nullptr;
    MFCreateAttributes(&pAttr, 1);
    if (pAttr) {
        pAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
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
        if (comInited)
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
            if (comInited)
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
        // SPS+PPS as sequence header (Annex-B)
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
            if (comInited)
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
            if (comInited)
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
            if (comInited)
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
            if (comInited)
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
        if (comInited)
            CoUninitialize();
        return;
    }

    // Frame duration in 100ns units: 1/fps = frameRateDen/frameRateNum seconds
    const LONGLONG kVideoFrameDuration100ns =
        (m_state.config.frame_rate_num > 0)
            ? static_cast<LONGLONG>(10000000LL * m_state.config.frame_rate_den / m_state.config.frame_rate_num)
            : 166667LL; // ~60 fps fallback
    // AAC-LC 1024 frames at 48 kHz
    constexpr LONGLONG kAudioFrameDuration100ns = 213333LL;

    // --- Step 7: Write packets ---
    for (const auto& ip : allPackets) {
        if (ip.bytes.empty())
            continue;

        LONGLONG pts100ns = static_cast<LONGLONG>(ip.pts_ns / 100ULL);
        LONGLONG dur100ns = (ip.stream_id == 0) ? kVideoFrameDuration100ns : kAudioFrameDuration100ns;
        DWORD streamIdx = (ip.stream_id == 0) ? videoStreamIdx : audioStreamIdxs[ip.stream_id - 1];

        hr = WriteSampleBytes(pWriter, streamIdx, ip.bytes.data(), ip.bytes.size(), pts100ns, dur100ns, ip.is_key);
        if (FAILED(hr)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WriteSample(stream=%u) failed 0x%08lX", ip.stream_id,
                     static_cast<unsigned long>(hr));
            pWriter->Finalize();
            pWriter->Release();
            m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Mux, buf);
            MFShutdown();
            if (comInited)
                CoUninitialize();
            return;
        }
    }

    // --- Step 8: Finalize ---
    hr = pWriter->Finalize();
    pWriter->Release();
    pWriter = nullptr;

    if (FAILED(hr)) {
        char buf[80];
        snprintf(buf, sizeof(buf), "IMFSinkWriter::Finalize failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(static_cast<int32_t>(hr), ErrorPhase::Finalize, buf);
        MFShutdown();
        if (comInited)
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
    if (comInited)
        CoUninitialize();
}

} // namespace recorder_core
