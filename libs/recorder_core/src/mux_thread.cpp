#include "mux_thread.h"

#include "annexb_to_avcc.h"
#include "matroska_stream_writer.h"
#include "session_internal.h"

#include <recorder_core/logging/logging.h>
#include <recorder_core/packet_types.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <span>
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
// Run — streaming MKV write (constant-RAM via bounded reorder window) with
// in-place segment splitting (SPLIT-RECORDING-R1).
//
// A split is a SplitSentinel in the mux queue (emitted by VideoThread right
// before the forced-keyframe that begins the new segment). On the sentinel the
// current MatroskaStreamWriter is Finalize()d (drain window, Cues, Duration,
// SeekHead, Segment size, close) and the segment's metadata is reported via the
// segment callback. The next video packet's session PTS becomes the new
// segment's epoch; every packet is rebased to segment-local time (PTS - epoch)
// before Push. Because each writer owns exactly one file and frees clusters as
// it goes, a failure opening/writing segment N cannot touch segments 1..N-1.
// ---------------------------------------------------------------------------

namespace {

uint64_t QueryFileSizeBytes(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA fd{};
    const std::wstring wpath = path.wstring();
    if (GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fd)) {
        return (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    }
    return 0;
}

} // namespace

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

    // Save codec private data and encode dimensions for deferred / per-segment track init
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

    const bool is_h264 = (m_state.config.video_codec == VideoCodec::H264Nvenc);
    const std::filesystem::path base_output_path = m_state.config.output_path;

    // Build a reusable writer config; only output_path changes per segment.
    // codec-private blobs are copied (not moved) because every segment's Tracks
    // element needs its own copy.
    MatroskaStreamConfig sw_config_template;
    sw_config_template.video_codec_id =
        (m_state.config.video_codec == VideoCodec::H264Nvenc) ? "V_MPEG4/ISO/AVC" : "V_AV1";
    sw_config_template.video_codec_private = video_codec_private;
    sw_config_template.encode_width = encW;
    sw_config_template.encode_height = encH;
    sw_config_template.frame_rate_num = m_state.config.frame_rate_num;
    sw_config_template.frame_rate_den = m_state.config.frame_rate_den;
    sw_config_template.audio_is_opus = (m_state.config.audio_codec == AudioCodec::Opus);
    sw_config_template.audio_track_count = track_count;
    for (uint32_t i = 0; i < track_count; ++i) {
        sw_config_template.audio_tracks[i].codec_private = audioCp[i].bytes;
    }

    // --- Segment state ---
    struct SegmentState {
        std::unique_ptr<MatroskaStreamWriter> writer;
        std::filesystem::path path;
        uint32_t index = 0;
        // session PTS at which this segment began (epoch for segment-local rebasing)
        uint64_t epoch_session_pts_ns = 0;
        bool epoch_set = false;
        // largest segment-local PTS emitted into this segment (for duration report)
        uint64_t max_local_pts_ns = 0;
    };
    SegmentState seg;

    bool write_error = false;
    bool any_segment_opened = false;
    uint64_t frame_dur_ns = 0;
    if (m_state.config.frame_rate_num > 0 && m_state.config.frame_rate_den > 0) {
        frame_dur_ns = static_cast<uint64_t>(m_state.config.frame_rate_den) * 1000000000ULL /
                       static_cast<uint64_t>(m_state.config.frame_rate_num);
    }

    const auto open_segment = [&](uint32_t index, uint64_t epoch_session_pts_ns, bool epoch_set) -> bool {
        seg.path = DeriveSegmentPath(base_output_path, index);
        seg.index = index;
        seg.epoch_session_pts_ns = epoch_session_pts_ns;
        seg.epoch_set = epoch_set;
        seg.max_local_pts_ns = 0;
        seg.writer = std::make_unique<MatroskaStreamWriter>();

        MatroskaStreamConfig cfg = sw_config_template;
        cfg.output_path = seg.path.string();
        if (!seg.writer->Open(cfg)) {
            const std::string err = seg.writer->error();
            seg.writer.reset();
            m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                                  "Matroska stream writer open failed (segment " + std::to_string(index) + "): " + err);
            return false;
        }
        any_segment_opened = true;
        logging::LogField fields[] = {{"segment_index", std::to_string(index)}, {"path", seg.path.string()}};
        logging::log(logging::LogLevel::Info, "mux_thread", "segment started",
                     std::span<const logging::LogField>(fields, std::size(fields)));
        return true;
    };

    // Finalize the current segment, report it via the segment callback, and
    // (optionally) quarantine the file on failure.
    const auto finalize_segment = [&]() {
        if (!seg.writer)
            return;
        const bool ok = seg.writer->Finalize();
        const std::string err = seg.writer->error();
        seg.writer.reset();

        const uint64_t local_duration_ns = seg.max_local_pts_ns + (seg.max_local_pts_ns > 0 ? frame_dur_ns : 0);
        const uint64_t file_bytes = ok ? QueryFileSizeBytes(seg.path) : 0;

        CompletedSegment info;
        info.path = seg.path;
        info.index = seg.index;
        info.session_start_ms = seg.epoch_session_pts_ns / 1000000ULL;
        info.duration_ms = local_duration_ns / 1000000ULL;
        info.file_size_bytes = file_bytes;
        info.succeeded = ok;

        if (!ok) {
            write_error = true;
            // Quarantine the incomplete current segment file; prior finalized
            // segments are already closed and untouched.
            std::error_code ec;
            std::filesystem::remove(seg.path, ec);
            info.file_size_bytes = 0;
            logging::LogField fields[] = {
                {"segment_index", std::to_string(seg.index)}, {"path", seg.path.string()}, {"error", err}};
            logging::log(logging::LogLevel::Error, "mux_thread", "segment finalize failed; file quarantined",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        } else {
            logging::LogField fields[] = {{"segment_index", std::to_string(seg.index)},
                                          {"duration_ms", std::to_string(info.duration_ms)},
                                          {"bytes", std::to_string(info.file_size_bytes)}};
            logging::log(logging::LogLevel::Info, "mux_thread", "segment finalized",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        }

        if (m_state.segment_callback) {
            m_state.segment_callback(info);
        }
    };

    // --- Open the first segment (index 0 keeps the base path) ---
    if (!open_segment(0, /*epoch*/ 0, /*epoch_set*/ false)) {
        return;
    }

    // --- A/V timestamp alignment, resolved incrementally (see header note) ---
    bool epoch_resolved = false;
    uint64_t head_start_ns = 0;
    std::deque<EncodedAudioPacket> pending_audio;

    auto resolve_epoch = [&]() {
        if (epoch_resolved)
            return;
        const uint64_t video_epoch = m_state.video_epoch_qpc_100ns.load();
        if (video_epoch == 0)
            return;
        const uint64_t session_start = m_state.session_start_qpc_100ns;
        head_start_ns = (video_epoch > session_start) ? (video_epoch - session_start) * 100ULL : 0ULL;
        epoch_resolved = true;
    };

    // Rebase a session PTS to the current segment's local timeline (>= 0).
    auto to_segment_local = [&](uint64_t session_pts_ns) -> uint64_t {
        if (!seg.epoch_set)
            return session_pts_ns;
        return (session_pts_ns > seg.epoch_session_pts_ns) ? (session_pts_ns - seg.epoch_session_pts_ns) : 0ULL;
    };

    auto push_audio = [&](EncodedAudioPacket&& payload) {
        if (write_error || !seg.writer)
            return;
        if (payload.track_id >= track_count || payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
            std::fprintf(stderr, "MuxThread: skipping audio packet with out-of-range track_id=%u (track_count=%u)\n",
                         payload.track_id, track_count);
            return;
        }
        if (head_start_ns > 0) {
            if (payload.pts_ns < head_start_ns)
                return;
            payload.pts_ns -= head_start_ns;
        }
        const uint64_t local = to_segment_local(payload.pts_ns);
        MuxPacket mp;
        mp.pts_ns = local;
        mp.track_num = 2 + payload.track_id;
        mp.is_key = true;
        mp.bytes = std::move(payload.bytes);
        if (local > seg.max_local_pts_ns)
            seg.max_local_pts_ns = local;
        if (!seg.writer->Push(std::move(mp)))
            write_error = true;
    };

    auto push_video = [&](EncodedVideoPacket&& payload) {
        if (write_error || !seg.writer)
            return;
        // A new segment's epoch is the first video packet seen after a split.
        if (!seg.epoch_set) {
            seg.epoch_session_pts_ns = payload.pts_ns;
            seg.epoch_set = true;
        }
        const uint64_t local = to_segment_local(payload.pts_ns);
        MuxPacket mp;
        mp.pts_ns = local;
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
        if (local > seg.max_local_pts_ns)
            seg.max_local_pts_ns = local;
        if (!seg.writer->Push(std::move(mp)))
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

    // Begin a new segment at a SplitSentinel: finalize the current segment, then
    // open the next. The new epoch is captured from the next video packet.
    auto begin_new_segment = [&](const SplitSentinel& s) {
        if (write_error)
            return;
        // Flush any buffered pre-epoch audio into the OLD segment first.
        drain_pending_audio();
        finalize_segment();
        // finalize_segment() sets write_error on a finalize/I-O failure (failure
        // isolation: quarantine the incomplete current file, keep prior segments).
        // cppcheck cannot see the captured-by-ref mutation through the lambda call.
        // cppcheck-suppress identicalConditionAfterEarlyExit
        if (write_error)
            return;
        // Open the next segment with epoch unset; push_video sets it from the
        // first video packet (the forced keyframe that follows the sentinel).
        open_segment(s.new_segment_index, /*epoch*/ 0, /*epoch_set*/ false);
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
        } else if constexpr (std::is_same_v<T, SplitSentinel>) {
            begin_new_segment(payload);
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

    // --- Step 3: Finalize the final segment ---
    finalize_segment();

    if (write_error || !any_segment_opened) {
        if (!m_state.HasFailure()) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Matroska stream write failed");
        }
    }

    // Update output file size in stats from the (final) base segment if present.
    {
        const uint64_t sz = QueryFileSizeBytes(base_output_path);
        if (sz > 0) {
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.output_file_bytes = sz;
        }
    }
}

} // namespace recorder_core
