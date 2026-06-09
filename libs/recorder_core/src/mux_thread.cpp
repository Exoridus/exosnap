#include "mux_thread.h"

#include "annexb_to_avcc.h"
#include "matroska_stream_writer.h"
#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <windows.h>

namespace recorder_core {

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
// Run — streaming MKV write (constant-RAM via bounded reorder window)
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

    // --- Step 2: Configure and open the streaming Matroska writer ---
    //
    // Unlike the previous batch-collect muxer, packets are now written into
    // clusters incrementally as they pass a bounded reorder window, so peak RAM
    // is O(window seconds) rather than O(entire session). See
    // matroska_stream_writer.{h,cpp}.
    MatroskaStreamConfig sw_config;
    sw_config.output_path = m_state.config.output_path.string();
    sw_config.video_codec_id = (m_state.config.video_codec == VideoCodec::H264Nvenc) ? "V_MPEG4/ISO/AVC" : "V_AV1";
    sw_config.video_codec_private = std::move(video_codec_private);
    sw_config.encode_width = encW;
    sw_config.encode_height = encH;
    sw_config.frame_rate_num = m_state.config.frame_rate_num;
    sw_config.frame_rate_den = m_state.config.frame_rate_den;
    sw_config.audio_is_opus = (m_state.config.audio_codec == AudioCodec::Opus);
    sw_config.audio_track_count = track_count;
    for (uint32_t i = 0; i < track_count; ++i) {
        sw_config.audio_tracks[i].codec_private = std::move(audioCp[i].bytes);
    }

    const bool is_h264 = (m_state.config.video_codec == VideoCodec::H264Nvenc);

    MatroskaStreamWriter writer;
    if (!writer.Open(sw_config)) {
        m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                              std::string("Matroska stream writer open failed: ") + writer.error());
        return;
    }

    // --- A/V timestamp alignment, resolved incrementally ---
    // Audio PTS tracks wall-clock (QPC) time; video PTS starts at the first WGC
    // frame. We must shift audio down by the WGC init delay (head_start_ns) so
    // both start near 0, and drop audio captured before the video epoch.
    //
    // In the streaming path head_start_ns becomes known as soon as the video
    // epoch is published (which happens on the first encoded video frame, early
    // in the session). Audio that arrives before the epoch is resolved is held
    // in a small pending buffer and flushed once the offset is known. Video
    // packets need no rebasing and are pushed immediately.
    bool epoch_resolved = false;
    uint64_t head_start_ns = 0;
    std::deque<EncodedAudioPacket> pending_audio; // bounded: drained on epoch resolve

    auto resolve_epoch = [&]() {
        if (epoch_resolved)
            return;
        const uint64_t video_epoch = m_state.video_epoch_qpc_100ns.load();
        if (video_epoch == 0)
            return; // not yet known
        const uint64_t session_start = m_state.session_start_qpc_100ns;
        head_start_ns = (video_epoch > session_start) ? (video_epoch - session_start) * 100ULL : 0ULL;
        epoch_resolved = true;
    };

    bool write_error = false;

    auto push_audio = [&](EncodedAudioPacket&& payload) {
        if (write_error)
            return;
        if (payload.track_id >= track_count || payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
            std::fprintf(stderr, "MuxThread: skipping audio packet with out-of-range track_id=%u (track_count=%u)\n",
                         payload.track_id, track_count);
            return;
        }
        // Drop audio captured before the video epoch; rebase the rest.
        if (head_start_ns > 0) {
            if (payload.pts_ns < head_start_ns)
                return;
            payload.pts_ns -= head_start_ns;
        }
        MuxPacket mp;
        mp.pts_ns = payload.pts_ns;
        mp.track_num = 2 + payload.track_id;
        mp.is_key = true;
        mp.bytes = std::move(payload.bytes);
        if (!writer.Push(std::move(mp)))
            write_error = true;
    };

    auto push_video = [&](EncodedVideoPacket&& payload) {
        if (write_error)
            return;
        MuxPacket mp;
        mp.pts_ns = payload.pts_ns;
        mp.track_num = 1;
        mp.is_key = payload.keyframe;
        if (is_h264) {
            std::vector<uint8_t> avcc;
            if (annexb::ConvertAnnexBToAvcc(payload.bytes.data(), payload.bytes.size(), avcc))
                mp.bytes = std::move(avcc);
            else
                mp.bytes = std::move(payload.bytes);
        } else {
            mp.bytes = std::move(payload.bytes);
        }
        if (!writer.Push(std::move(mp)))
            write_error = true;
    };

    auto drain_pending_audio = [&]() {
        if (!epoch_resolved)
            return;
        while (!pending_audio.empty() && !write_error) {
            push_audio(std::move(pending_audio.front()));
            pending_audio.pop_front();
        }
    };

    // A single dispatch for a queued MuxItem payload.
    auto handle_payload = [&](auto&& payload, bool& video_eos,
                              std::array<bool, CodecPrivateData::kMaxAudioTracks>& audio_eos) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
            if (!payload.bytes.empty()) {
                resolve_epoch();
                push_video(std::move(payload));
                drain_pending_audio();
            }
        } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
            if (!payload.bytes.empty()) {
                resolve_epoch();
                if (epoch_resolved) {
                    drain_pending_audio();
                    push_audio(std::move(payload));
                } else {
                    pending_audio.push_back(std::move(payload));
                }
            }
        } else if constexpr (std::is_same_v<T, VideoEosSentinel>) {
            video_eos = true;
        } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
            if (payload.track_id < track_count && payload.track_id < CodecPrivateData::kMaxAudioTracks)
                audio_eos[payload.track_id] = true;
        }
    };

    // 2a. Drain premux buffers (packets captured before tracks were initialized).
    {
        std::lock_guard lk(m_state.premux_mutex);
        for (auto& pkt : m_state.video_premux) {
            if (pkt.bytes.empty())
                continue;
            resolve_epoch();
            push_video(std::move(pkt));
            drain_pending_audio();
        }
        for (auto& pkt : m_state.audio_premux) {
            if (pkt.bytes.empty())
                continue;
            resolve_epoch();
            if (epoch_resolved) {
                drain_pending_audio();
                push_audio(std::move(pkt));
            } else {
                pending_audio.push_back(std::move(pkt));
            }
        }
        m_state.video_premux.clear();
        m_state.audio_premux.clear();
    }

    // 2b. Stream packets from the mux queue until both EOS sentinels arrive.
    bool videoEos = false;
    std::array<bool, CodecPrivateData::kMaxAudioTracks> audioEosReceived{};
    const auto all_audio_eos_received = [&]() {
        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audioEosReceived[i])
                return false;
        }
        return true;
    };

    while (!(videoEos && all_audio_eos_received())) {
        if (m_state.HasFailure() || write_error)
            break;

        std::unique_lock lk(m_state.mux_mutex);
        m_state.mux_cv.wait_for(lk, std::chrono::milliseconds(5),
                                [&] { return !m_state.mux_queue.empty() || m_state.stop_requested.load(); });
        if (m_state.HasFailure())
            break;

        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            lk.unlock();
            std::visit([&](auto&& payload) { handle_payload(std::move(payload), videoEos, audioEosReceived); },
                       item.payload);
            lk.lock();
        }
    }

    // 2c. Final drain for race-window packets (arrived after both EOS sentinels).
    {
        std::lock_guard lk(m_state.mux_mutex);
        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            std::visit([&](auto&& payload) { handle_payload(std::move(payload), videoEos, audioEosReceived); },
                       item.payload);
        }
    }

    // If the epoch never resolved (no video frame at all), flush whatever audio
    // was buffered un-rebased so it is not silently lost.
    if (!epoch_resolved) {
        while (!pending_audio.empty() && !write_error) {
            push_audio(std::move(pending_audio.front()));
            pending_audio.pop_front();
        }
    }

    // --- Step 3: Finalize (drain window, Cues, Duration, SeekHead, Segment size) ---
    const bool finalize_ok = writer.Finalize();

    if (write_error || !finalize_ok) {
        // Preserve first-error semantics: only record if not already failed.
        if (!m_state.HasFailure()) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                  std::string("Matroska stream write failed: ") + writer.error());
        }
        // The file is closed by Finalize(); leave it for the owner to quarantine.
    }

    // Report bounded buffering for diagnostics.
    std::fprintf(stderr, "MuxThread: streaming mux peak window = %zu packets, %llu bytes\n",
                 writer.peak_window_packets(), static_cast<unsigned long long>(writer.peak_window_bytes()));

    // Update output file size in stats.
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
