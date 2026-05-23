#include "mux_thread.h"

#include "annexb_to_avcc.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

namespace recorder_core {

// ---------------------------------------------------------------------------
// Internal packet representation for the batch write
// ---------------------------------------------------------------------------

namespace {

struct InterleavePacket {
    uint64_t pts_ns = 0;
    uint64_t track_num = 0;
    bool is_key = false;
    std::vector<uint8_t> bytes;
};

// Write a single interleave packet to the segment.
// Returns false on error.
bool WritePacket(mkvmuxer::Segment& seg, const InterleavePacket& ip) {
    return seg.AddFrame(ip.bytes.data(), static_cast<uint64_t>(ip.bytes.size()), ip.track_num, ip.pts_ns, ip.is_key);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MuxThread
// ---------------------------------------------------------------------------

MuxThread::MuxThread(SessionState& state) : m_state(state) {
}

MuxThread::~MuxThread() {
    if (m_thread.joinable())
        m_thread.detach();
}

void MuxThread::Start() {
    m_thread = std::thread([this] { Run(); });
}

bool MuxThread::Join(unsigned timeout_ms) {
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
// Run — batch-collect, global PTS sort, final MKV write
// ---------------------------------------------------------------------------

void MuxThread::Run() {
    // --- Step 1: Wait for codec private data to be ready ---
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
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Codec private data not available at mux start");
            return;
        }

        track_count = m_state.audio_track_count;
    }

    if (track_count > CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Mux, "Audio track count exceeds maximum supported");
        return;
    }

    // Save codec private data and encode dimensions for deferred track init
    std::vector<uint8_t> video_codec_private;
    std::array<AudioCodecPrivateSlot, CodecPrivateData::kMaxAudioTracks> audioCp{};
    {
        std::lock_guard lk(m_state.premux_mutex);
        if (m_state.config.video_codec == VideoCodec::H264Nvenc) {
            if (!annexb::BuildAvccFromAnnexBSpsAndPps(m_state.codec_private.h264_sps_pps, video_codec_private)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Failed to build AVCC from H.264 SPS/PPS for Matroska");
                return;
            }
        } else {
            video_codec_private.assign(m_state.codec_private.av1_codec_private,
                                       m_state.codec_private.av1_codec_private + 4);
        }
        for (uint32_t i = 0; i < track_count; ++i) {
            audioCp[i] = m_state.codec_private.audio_codec_private[i];
        }
    }

    uint32_t encW = 0, encH = 0;
    {
        std::lock_guard lk(m_state.stats_mutex);
        encW = m_state.encode_width;
        encH = m_state.encode_height;
    }

    // --- Step 2: Collect all packets in memory ---
    std::vector<InterleavePacket> allPackets;

    // 2a. Drain premux buffers into batch collection
    //     Use placeholder track IDs: 1=video, 2+audio_track_id=audio
    //     Remapped after actual track init in Step 5.
    {
        std::lock_guard lk(m_state.premux_mutex);

        for (auto& pkt : m_state.video_premux) {
            if (pkt.bytes.empty())
                continue;
            InterleavePacket ip;
            ip.pts_ns = pkt.pts_ns;
            ip.track_num = 1;
            ip.is_key = pkt.keyframe;
            ip.bytes = std::move(pkt.bytes);
            allPackets.push_back(std::move(ip));
        }
        for (auto& pkt : m_state.audio_premux) {
            if (pkt.bytes.empty())
                continue;
            if (pkt.track_id >= track_count || pkt.track_id >= CodecPrivateData::kMaxAudioTracks) {
                std::fprintf(stderr,
                             "MuxThread: skipping premux audio packet with out-of-range track_id=%u (track_count=%u)\n",
                             pkt.track_id, track_count);
                continue;
            }
            InterleavePacket ip;
            ip.pts_ns = pkt.pts_ns;
            ip.track_num = 2 + pkt.track_id;
            ip.is_key = true;
            ip.bytes = std::move(pkt.bytes);
            allPackets.push_back(std::move(ip));
        }

        m_state.video_premux.clear();
        m_state.audio_premux.clear();
    }

    // 2b. Wait for both EOS sentinels, collecting from mux_queue
    bool videoEos = false;
    std::array<bool, CodecPrivateData::kMaxAudioTracks> audioEosReceived{};
    const auto all_audio_eos_received = [&]() {
        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audioEosReceived[i]) {
                return false;
            }
        }
        return true;
    };

    while (!(videoEos && all_audio_eos_received())) {
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
                                ip.track_num = 1;
                                ip.is_key = payload.keyframe;
                                ip.bytes = std::move(payload.bytes);
                                allPackets.push_back(std::move(ip));
                            }
                        } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                            if (!payload.bytes.empty()) {
                                if (payload.track_id >= track_count ||
                                    payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
                                    std::fprintf(
                                        stderr,
                                        "MuxThread: skipping queued audio packet with out-of-range track_id=%u "
                                        "(track_count=%u)\n",
                                        payload.track_id, track_count);
                                    return;
                                }
                                InterleavePacket ip;
                                ip.pts_ns = payload.pts_ns;
                                ip.track_num = 2 + payload.track_id;
                                ip.is_key = true;
                                ip.bytes = std::move(payload.bytes);
                                allPackets.push_back(std::move(ip));
                            }
                        } else if constexpr (std::is_same_v<T, VideoEosSentinel>) {
                            videoEos = true;
                        } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
                            if (payload.track_id >= track_count ||
                                payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
                                std::fprintf(stderr,
                                             "MuxThread: ignoring audio EOS with out-of-range track_id=%u "
                                             "(track_count=%u)\n",
                                             payload.track_id, track_count);
                            } else {
                                audioEosReceived[payload.track_id] = true;
                            }
                        }
                    },
                    item.payload);

                lk.lock();
            }
        }
    }

    // 2c. Final drain for race-window packets (arrived after both EOS sentinels)
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
                            ip.track_num = 1;
                            ip.is_key = payload.keyframe;
                            ip.bytes = std::move(payload.bytes);
                            allPackets.push_back(std::move(ip));
                        }
                    } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                        if (!payload.bytes.empty()) {
                            if (payload.track_id >= track_count ||
                                payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
                                std::fprintf(stderr,
                                             "MuxThread: skipping final-drain audio packet with out-of-range "
                                             "track_id=%u (track_count=%u)\n",
                                             payload.track_id, track_count);
                                return;
                            }
                            InterleavePacket ip;
                            ip.pts_ns = payload.pts_ns;
                            ip.track_num = 2 + payload.track_id;
                            ip.is_key = true;
                            ip.bytes = std::move(payload.bytes);
                            allPackets.push_back(std::move(ip));
                        }
                    } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
                        if (payload.track_id >= track_count || payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
                            std::fprintf(stderr,
                                         "MuxThread: ignoring final-drain audio EOS with out-of-range track_id=%u "
                                         "(track_count=%u)\n",
                                         payload.track_id, track_count);
                        } else {
                            audioEosReceived[payload.track_id] = true;
                        }
                    }
                },
                item.payload);
        }
    }

    // --- Step 3: Stable-sort by PTS ---
    std::stable_sort(allPackets.begin(), allPackets.end(),
                     [](const InterleavePacket& a, const InterleavePacket& b) { return a.pts_ns < b.pts_ns; });

    // Do not emit a container when failure was recorded and no usable payload exists.
    if (m_state.HasFailure() && allPackets.empty()) {
        return;
    }

    // --- Step 4: Open output file ---
    mkvmuxer::MkvWriter mkvWriter;
    {
        std::string outPath = m_state.config.output_path.string();
        if (!mkvWriter.Open(outPath.c_str())) {
            m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                                  "MkvWriter::Open failed for: " + outPath);
            return;
        }
    }

    mkvmuxer::Segment segment;
    if (!segment.Init(&mkvWriter)) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "mkvmuxer::Segment::Init failed");
        return;
    }
    segment.set_mode(mkvmuxer::Segment::kFile);

    // --- Step 5: Add video and audio tracks ---
    uint64_t videoTrackNum = segment.AddVideoTrack(static_cast<int32_t>(encW), static_cast<int32_t>(encH), 1);
    if (videoTrackNum == 0) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "AddVideoTrack returned 0");
        return;
    }

    auto* vt = static_cast<mkvmuxer::VideoTrack*>(segment.GetTrackByNumber(videoTrackNum));
    if (m_state.config.video_codec == VideoCodec::H264Nvenc) {
        vt->set_codec_id("V_MPEG4/ISO/AVC");
    } else {
        vt->set_codec_id("V_AV1");
    }
    vt->SetCodecPrivate(video_codec_private.data(), static_cast<uint64_t>(video_codec_private.size()));
    vt->set_frame_rate(static_cast<double>(m_state.config.frame_rate_num) /
                       static_cast<double>(m_state.config.frame_rate_den));

    segment.OutputCues(true);
    if (!segment.CuesTrack(videoTrackNum)) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "segment.CuesTrack failed for video track");
        return;
    }

    std::array<uint64_t, CodecPrivateData::kMaxAudioTracks> audioTrackNums{};
    const bool use_opus = (m_state.config.audio_codec == AudioCodec::Opus);
    for (uint32_t i = 0; i < track_count; ++i) {
        const uint64_t trackNum = segment.AddAudioTrack(48000, 2, static_cast<int32_t>(2 + i));
        if (trackNum == 0) {
            mkvWriter.Close();
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "AddAudioTrack returned 0");
            return;
        }

        auto* at = static_cast<mkvmuxer::AudioTrack*>(segment.GetTrackByNumber(trackNum));
        const auto& slot = audioCp[i];
        if (use_opus) {
            if (slot.bytes.size() < 19) {
                mkvWriter.Close();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "Opus CodecPrivate is incomplete; expected full 19-byte OpusHead");
                return;
            }

            at->set_codec_id("A_OPUS");
            at->SetCodecPrivate(slot.bytes.data(), static_cast<uint64_t>(slot.bytes.size()));

            const uint16_t pre_skip =
                static_cast<uint16_t>(slot.bytes[10]) | (static_cast<uint16_t>(slot.bytes[11]) << 8u);
            const uint64_t codec_delay_ns = static_cast<uint64_t>(pre_skip) * 1000000000ULL / 48000u;
            at->set_codec_delay(codec_delay_ns);
            at->set_seek_pre_roll(80000000ULL);
        } else {
            at->set_codec_id("A_AAC");
            at->SetCodecPrivate(slot.bytes.empty() ? nullptr : slot.bytes.data(),
                                static_cast<uint64_t>(slot.bytes.size()));
        }

        audioTrackNums[i] = trackNum;
    }

    // --- Step 6: Remap placeholder track IDs to real mkvmuxer track numbers ---
    for (auto& ip : allPackets) {
        if (ip.track_num == 1) {
            ip.track_num = videoTrackNum;
        } else {
            if (ip.track_num < 2) {
                std::fprintf(stderr, "MuxThread: skipping packet with invalid placeholder track_num=%llu\n",
                             static_cast<unsigned long long>(ip.track_num));
                ip.track_num = 0;
                continue;
            }

            const uint32_t audioId = static_cast<uint32_t>(ip.track_num - 2);
            if (audioId >= track_count || audioId >= CodecPrivateData::kMaxAudioTracks) {
                std::fprintf(stderr,
                             "MuxThread: skipping packet with out-of-range audio placeholder=%u (track_count=%u)\n",
                             audioId, track_count);
                ip.track_num = 0;
                continue;
            }

            ip.track_num = audioTrackNums[audioId];
        }
    }

    // --- Step 7: Write all packets in globally sorted PTS order ---
    // Note: video_bytes and audio_bytes are already accumulated by the encode
    // paths (video_thread / audio_thread) and must not be added here again.
    for (const auto& ip : allPackets) {
        if (ip.track_num == 0) {
            continue;
        }
        if (!WritePacket(segment, ip)) {
            mkvWriter.Close();
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "segment.AddFrame failed during final write");
            return;
        }
    }

    // --- Step 8: Finalize ---
    if (!segment.Finalize()) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Finalize, "mkvmuxer::Segment::Finalize returned false");
        return;
    }

    mkvWriter.Close();

    // Update output file size in stats
    {
        WIN32_FILE_ATTRIBUTE_DATA fd{};
        std::wstring wPath = m_state.config.output_path.wstring();
        if (GetFileAttributesExW(wPath.c_str(), GetFileExInfoStandard, &fd)) {
            uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.output_file_bytes = sz;
        }
    }
}

} // namespace recorder_core
