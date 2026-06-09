#include "mux_thread.h"

#include "annexb_to_avcc.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

// libebml / libmatroska headers — suppress MSVC warnings from third-party code
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // size_t to smaller integer conversion
#pragma warning(disable : 4244) // possible data loss in conversion
#pragma warning(disable : 4245) // signed/unsigned mismatch in conversion
#pragma warning(disable : 4100) // unreferenced formal parameter
#endif
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTracks.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
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
    uint64_t track_num = 0; // 1=video, 2+n=audio (placeholder IDs, unchanged from old code)
    bool is_key = false;
    std::vector<uint8_t> bytes;
};

// Timecode scale: 1 000 000 ns per unit = 1 ms resolution
static constexpr uint64_t kTimecodeScaleNs = 1000000ULL;

// Cluster boundary: start a new cluster at a keyframe when at least 2000 ms have elapsed
static constexpr uint64_t kClusterBoundaryMs = 2000ULL;

// Maximum relative timecode inside a cluster (int16_t range, in ms)
static constexpr int64_t kMaxClusterRelativeMs = 32767LL;

// Space reserved for SeekHead at the start of the Segment (void placeholder).
// 3 entries (Info, Tracks, Cues) × ~30 bytes each + SeekHead header ≈ 90 bytes.
// 150 bytes provides ample margin.
static constexpr uint64_t kSeekHeadReservedBytes = 150ULL;

struct PendingCue {
    uint64_t timecode_ms;
};

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
    //     Placeholder track IDs: 1=video, 2+audio_track_id=audio
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

    // --- Step 2.5: A/V timestamp alignment ---
    // Audio PTS tracks wall-clock time (QPC-based). Video PTS starts at first WGC frame.
    // Shift audio PTS down by the WGC init delay so both start at 0 together.
    // Audio packets with PTS < head_start_ns (captured before video epoch) are dropped.
    {
        const uint64_t video_epoch = m_state.video_epoch_qpc_100ns.load();
        const uint64_t session_start = m_state.session_start_qpc_100ns;
        uint64_t head_start_ns = 0;
        if (video_epoch > session_start) {
            head_start_ns = (video_epoch - session_start) * 100ULL;
        }

        if (head_start_ns > 0) {
            allPackets.erase(std::remove_if(allPackets.begin(), allPackets.end(),
                                            [head_start_ns](const InterleavePacket& ip) {
                                                return ip.track_num != 1 && ip.pts_ns < head_start_ns;
                                            }),
                             allPackets.end());
            for (auto& ip : allPackets) {
                if (ip.track_num != 1) {
                    ip.pts_ns -= head_start_ns;
                }
            }
        }
    }

    // --- Step 3: Stable-sort by PTS ---
    std::stable_sort(allPackets.begin(), allPackets.end(),
                     [](const InterleavePacket& a, const InterleavePacket& b) { return a.pts_ns < b.pts_ns; });

    // Do not emit a container when failure was recorded and no usable payload exists.
    if (m_state.HasFailure() && allPackets.empty()) {
        return;
    }

    // --- Step 3.5: Convert H.264 video packets from Annex-B to AVCC ---
    // Matroska V_MPEG4/ISO/AVC requires 4-byte length-prefixed AVCC sample payloads;
    // NVENC produces Annex-B (start-code prefixed). Convert each video packet in-place.
    if (m_state.config.video_codec == VideoCodec::H264Nvenc) {
        for (auto& ip : allPackets) {
            if (ip.track_num != 1)
                continue;
            std::vector<uint8_t> avcc;
            if (annexb::ConvertAnnexBToAvcc(ip.bytes.data(), ip.bytes.size(), avcc))
                ip.bytes = std::move(avcc);
        }
    }

    // --- Step 3.6: Compute segment duration from the sorted packet timeline ---
    // All packets are collected before writing begins, so duration is known upfront.
    uint64_t max_pts_ns = 0;
    for (const auto& ip : allPackets) {
        if (ip.pts_ns > max_pts_ns)
            max_pts_ns = ip.pts_ns;
    }
    // Add one video frame's worth of duration for the last frame.
    uint64_t last_frame_ns = 0;
    if (m_state.config.frame_rate_num > 0 && m_state.config.frame_rate_den > 0) {
        last_frame_ns = static_cast<uint64_t>(m_state.config.frame_rate_den) * 1000000000ULL /
                        static_cast<uint64_t>(m_state.config.frame_rate_num);
    }
    const uint64_t segment_duration_ms = (max_pts_ns + last_frame_ns) / kTimecodeScaleNs;

    // --- Step 4: Open output file ---
    std::string outPath = m_state.config.output_path.string();
    libebml::StdIOCallback* io = nullptr;
    try {
        io = new libebml::StdIOCallback(outPath.c_str(), MODE_CREATE);
    } catch (const std::exception& ex) {
        m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                              std::string("StdIOCallback::open failed: ") + ex.what());
        return;
    }

    // --- Step 5: Write EBML header ---
    {
        libebml::EbmlHead ebml_head;
        libebml::GetChild<libebml::EDocType>(ebml_head).SetValue("matroska");
        libebml::GetChild<libebml::EDocTypeVersion>(ebml_head).SetValue(4);
        libebml::GetChild<libebml::EDocTypeReadVersion>(ebml_head).SetValue(2);
        ebml_head.Render(*io, true);
    }

    // --- Step 6: Write Segment head ---
    // Reserve an 8-byte size field (WriteHead). The true size is unknown until all
    // children are written, so it is back-patched with ForceSize + OverwriteHead at
    // finalization. segment_data_start marks the first byte after this 12-byte head.
    libmatroska::KaxSegment segment;
    segment.SetSizeInfinite(true);
    segment.WriteHead(*io, 8);
    const uint64_t segment_data_start = static_cast<uint64_t>(io->getFilePointer());

    // --- Step 6.5: Reserve space for SeekHead at the beginning of the Segment ---
    // Render a void placeholder now; after Cues is written we replace it in place
    // with the real SeekHead via EbmlVoid::ReplaceWith, which handles all element
    // size accounting and fills any remainder with a smaller void. The placeholder
    // object must stay alive until ReplaceWith so its file position is preserved.
    libebml::EbmlVoid seekhead_void;
    seekhead_void.SetSize(kSeekHeadReservedBytes);
    seekhead_void.Render(*io);

    // --- Step 7: Write Segment Info (with duration) ---
    auto& info = libebml::AddNewChild<libmatroska::KaxInfo>(segment);
    libebml::GetChild<libmatroska::KaxTimecodeScale>(info).SetValue(kTimecodeScaleNs);
    libebml::GetChild<libmatroska::KaxMuxingApp>(info).SetValueUTF8("exosnap");
    libebml::GetChild<libmatroska::KaxWritingApp>(info).SetValueUTF8("exosnap");
    libebml::GetChild<libmatroska::KaxDuration>(info).SetValue(static_cast<double>(segment_duration_ms));
    info.Render(*io);

    // --- Step 8: Build and write Tracks element ---
    // KaxTracks (tracks_elem) owns all KaxTrackEntry children via EbmlMaster.
    // track_entries holds raw pointers to those children for use during cluster writes.
    // tracks_elem must outlive the cluster write loop.
    libmatroska::KaxTracks tracks_elem;
    std::vector<libmatroska::KaxTrackEntry*> track_entries;

    // Video track (track_num = 1, track_entries[0])
    {
        auto& vid = libebml::AddNewChild<libmatroska::KaxTrackEntry>(tracks_elem);
        libebml::GetChild<libmatroska::KaxTrackNumber>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackUID>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackType>(vid).SetValue(1); // 1 = video
        libebml::GetChild<libmatroska::KaxTrackFlagEnabled>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackFlagDefault>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackFlagLacing>(vid).SetValue(0);

        if (m_state.config.video_codec == VideoCodec::H264Nvenc) {
            libebml::GetChild<libmatroska::KaxCodecID>(vid).SetValue("V_MPEG4/ISO/AVC");
        } else {
            libebml::GetChild<libmatroska::KaxCodecID>(vid).SetValue("V_AV1");
        }

        if (!video_codec_private.empty()) {
            libebml::GetChild<libmatroska::KaxCodecPrivate>(vid).CopyBuffer(
                video_codec_private.data(), static_cast<uint32_t>(video_codec_private.size()));
        }

        auto& vid_settings = libebml::GetChild<libmatroska::KaxTrackVideo>(vid);
        libebml::GetChild<libmatroska::KaxVideoPixelWidth>(vid_settings).SetValue(encW);
        libebml::GetChild<libmatroska::KaxVideoPixelHeight>(vid_settings).SetValue(encH);

        if (m_state.config.frame_rate_num > 0 && m_state.config.frame_rate_den > 0) {
            const uint64_t frame_duration_ns = static_cast<uint64_t>(m_state.config.frame_rate_den) * 1000000000ULL /
                                               static_cast<uint64_t>(m_state.config.frame_rate_num);
            libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(vid).SetValue(frame_duration_ns);
        }

        vid.SetGlobalTimecodeScale(kTimecodeScaleNs);
        track_entries.push_back(&vid);
    }

    // Audio tracks (track_num = 2+i, track_entries[1+i])
    {
        const bool use_opus = (m_state.config.audio_codec == AudioCodec::Opus);
        for (uint32_t i = 0; i < track_count; ++i) {
            const uint32_t track_number = 2 + i;
            auto& aud = libebml::AddNewChild<libmatroska::KaxTrackEntry>(tracks_elem);
            libebml::GetChild<libmatroska::KaxTrackNumber>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackUID>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackType>(aud).SetValue(2); // 2 = audio
            libebml::GetChild<libmatroska::KaxTrackFlagLacing>(aud).SetValue(0);

            const auto& slot = audioCp[i];

            if (use_opus) {
                if (slot.bytes.size() < 19) {
                    io->close();
                    delete io;
                    m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                          "Opus CodecPrivate is incomplete; expected full 19-byte OpusHead");
                    return;
                }

                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_OPUS");
                libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(
                    slot.bytes.data(), static_cast<uint32_t>(slot.bytes.size()));

                const uint16_t pre_skip =
                    static_cast<uint16_t>(slot.bytes[10]) | (static_cast<uint16_t>(slot.bytes[11]) << 8u);
                const uint64_t codec_delay_ns = static_cast<uint64_t>(pre_skip) * 1000000000ULL / 48000u;

                libebml::GetChild<libmatroska::KaxCodecDelay>(aud).SetValue(codec_delay_ns);
                libebml::GetChild<libmatroska::KaxSeekPreRoll>(aud).SetValue(80000000ULL);
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(20000000ULL);
            } else {
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_AAC");
                if (!slot.bytes.empty()) {
                    libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(
                        slot.bytes.data(), static_cast<uint32_t>(slot.bytes.size()));
                }
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(21333333ULL);
            }

            auto& aud_settings = libebml::GetChild<libmatroska::KaxTrackAudio>(aud);
            libebml::GetChild<libmatroska::KaxAudioSamplingFreq>(aud_settings).SetValue(48000.0);
            libebml::GetChild<libmatroska::KaxAudioChannels>(aud_settings).SetValue(2);

            aud.SetGlobalTimecodeScale(kTimecodeScaleNs);
            track_entries.push_back(&aud);
        }
    }

    // bSaveDefault=false (default): skips forbidden deprecated elements like
    // KaxTrackTimecodeScale that are mandatory+unique but must not be serialized.
    tracks_elem.Render(*io);

    // --- Step 9: Write clusters ---
    //
    // Block representation: KaxSimpleBlock (not KaxBlockGroup) with the correct
    // keyframe flag per packet. This avoids the Matroska ambiguity where a
    // KaxBlockGroup without KaxReferenceBlock implies the block is a random-access
    // point regardless of the actual codec frame type.
    //
    // NVENC is configured with frameIntervalP=1 (I/P only, no B-frames), so all
    // non-keyframes are P-frames referencing only the immediately preceding frame.
    // The SimpleBlock keyframe flag directly encodes this distinction without
    // requiring ReferenceBlock entries.
    //
    // Cues: generated manually for video keyframes only after each cluster render,
    // using the cluster's finalized file position.
    libmatroska::KaxCues cues;
    cues.SetGlobalTimecodeScale(kTimecodeScaleNs);

    bool first_cluster = true;
    uint64_t cluster_start_ms = 0;
    libmatroska::KaxCluster* current_cluster = nullptr;

    // Cue entries pending for the current cluster (video keyframes only).
    std::vector<PendingCue> pending_cues;

    // DataBuffer objects are heap-allocated below and owned by the cluster via
    // KaxInternalBlock::myBuffers. The cluster's destructor calls ReleaseFrames()
    // which deletes them. Do NOT keep a separate owning reference.

    // flush_cluster: render the current cluster, add cue entries for video
    // keyframes, then release resources.
    auto flush_cluster = [&]() {
        if (current_cluster == nullptr)
            return;

        current_cluster->Render(*io, cues);

        // Add one cue entry per video keyframe that started in this cluster.
        const uint64_t cluster_rel_pos = current_cluster->GetPosition();
        for (const auto& pc : pending_cues) {
            auto& point = libebml::AddNewChild<libmatroska::KaxCuePoint>(cues);
            libebml::GetChild<libmatroska::KaxCueTime>(point).SetValue(pc.timecode_ms);
            auto& track_pos = libebml::AddNewChild<libmatroska::KaxCueTrackPositions>(point);
            libebml::GetChild<libmatroska::KaxCueTrack>(track_pos).SetValue(1); // video track
            libebml::GetChild<libmatroska::KaxCueClusterPosition>(track_pos).SetValue(cluster_rel_pos);
        }
        pending_cues.clear();

        delete current_cluster;
        current_cluster = nullptr;
    };

    for (const auto& ip : allPackets) {
        if (ip.track_num == 0)
            continue;

        // Resolve KaxTrackEntry* from placeholder track_num:
        //   track_num 1        -> track_entries[0] (video)
        //   track_num 2+n      -> track_entries[1+n] (audio)
        libmatroska::KaxTrackEntry* track_entry = nullptr;
        if (ip.track_num == 1) {
            track_entry = track_entries[0];
        } else {
            const uint32_t audioIdx = static_cast<uint32_t>(ip.track_num - 2);
            const uint32_t entryIdx = audioIdx + 1;
            if (entryIdx >= static_cast<uint32_t>(track_entries.size())) {
                std::fprintf(stderr, "MuxThread: skipping packet with out-of-range track_num=%llu\n",
                             static_cast<unsigned long long>(ip.track_num));
                continue;
            }
            track_entry = track_entries[entryIdx];
        }

        const uint64_t pkt_ms = ip.pts_ns / kTimecodeScaleNs;

        // Decide whether to start a new cluster
        bool need_new_cluster = first_cluster;
        if (!first_cluster && current_cluster != nullptr) {
            const int64_t relative_ms = static_cast<int64_t>(pkt_ms) - static_cast<int64_t>(cluster_start_ms);
            if (relative_ms > kMaxClusterRelativeMs) {
                // Relative timecode would overflow int16_t: must start a new cluster
                need_new_cluster = true;
            } else if (ip.is_key && ip.track_num == 1 && (pkt_ms - cluster_start_ms) >= kClusterBoundaryMs) {
                // New video keyframe after at least 2 s in current cluster
                need_new_cluster = true;
            }
        }

        if (need_new_cluster) {
            flush_cluster();
            first_cluster = false;
            cluster_start_ms = pkt_ms;

            current_cluster = new libmatroska::KaxCluster();
            current_cluster->SetParent(segment);
            current_cluster->SetGlobalTimecodeScale(kTimecodeScaleNs);
            // InitTimecode(timecode_in_scale_units, timecode_scale_ns):
            //   cluster_start_ms is in ms == scale units for 1ms resolution.
            current_cluster->InitTimecode(cluster_start_ms, static_cast<int64_t>(kTimecodeScaleNs));
        }

        // Heap-allocate DataBuffer. The cluster owns it via myBuffers; the cluster's
        // destructor calls ReleaseFrames() which deletes it. The pointed-to bytes
        // are owned by allPackets and outlive the cluster. Do not delete data_buf here.
        auto* data_buf =
            new libmatroska::DataBuffer(const_cast<uint8_t*>(ip.bytes.data()), static_cast<uint32_t>(ip.bytes.size()));

        // Add a SimpleBlock to the cluster.
        // KaxSimpleBlock::SetKeyframe(true)  = random-access / IDR / I-frame
        // KaxSimpleBlock::SetKeyframe(false) = inter-coded frame (P-frame)
        auto& sb = libebml::AddNewChild<libmatroska::KaxSimpleBlock>(*current_cluster);
        sb.SetParent(*current_cluster);
        sb.AddFrame(*track_entry, ip.pts_ns, *data_buf, libmatroska::LACING_NONE);
        sb.SetKeyframe(ip.is_key);

        // Register video keyframes for cue emission after this cluster is rendered.
        if (ip.is_key && ip.track_num == 1) {
            pending_cues.push_back({pkt_ms});
        }
    }

    flush_cluster(); // write the last cluster

    // --- Step 10: Write Cues ---
    // Cue entries were added manually in flush_cluster for each video keyframe.
    // The KaxCluster::Render call above populates position information before
    // we call cues.PositionSet — our manual entries already have the correct
    // cluster-relative positions.
    cues.Render(*io);

    // --- Step 11: Write SeekHead into the reserved placeholder ---
    // EbmlVoid::ReplaceWith renders the SeekHead at the placeholder's recorded
    // position and fills any leftover space with a correctly-sized void element,
    // handling all EBML head/size accounting. ComeBackAfterward=true restores the
    // file pointer to the current end-of-file afterwards.
    {
        libmatroska::KaxSeekHead seekhead;
        seekhead.IndexThis(info, segment);
        seekhead.IndexThis(tracks_elem, segment);
        seekhead.IndexThis(cues, segment);

        // ReplaceWith returns INVALID_FILEPOS_T (a macro == 0) if the placeholder
        // is too small. On failure the void stays in place and the file remains
        // structurally valid; seeking simply falls back to the Cues element.
        const auto replaced = seekhead_void.ReplaceWith(seekhead, *io, /*ComeBackAfterward=*/true);
        if (replaced == INVALID_FILEPOS_T) {
            std::fprintf(stderr, "MuxThread: SeekHead did not fit in reserved space; "
                                 "wrote Cues-only seeking\n");
        }
    }

    // --- Step 12: Back-patch the Segment size and overwrite its head ---
    // ReplaceWith(ComeBackAfterward=true) left the file pointer at end-of-file, so
    // this is the true segment data size. ForceSize requires the 8-byte reservation
    // from WriteHead so the coded length matches; OverwriteHead then rewrites the
    // head in place. Without this the Segment would claim size 0 and no parser could
    // read the tracks or clusters.
    {
        const uint64_t eof_pos = static_cast<uint64_t>(io->getFilePointer());
        const uint64_t segment_size = eof_pos - segment_data_start;
        if (segment.ForceSize(segment_size)) {
            segment.OverwriteHead(*io);
        } else {
            std::fprintf(stderr,
                         "MuxThread: ForceSize failed for segment size %llu; "
                         "leaving unknown-size segment\n",
                         static_cast<unsigned long long>(segment_size));
        }
    }

    io->close();
    delete io;

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
