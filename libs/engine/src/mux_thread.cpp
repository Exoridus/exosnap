#include "mux_thread.h"

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "av_epoch_align.h"
#include "exosnap/engine/audio_track_model.h"
#include "exosnap/engine/codec_types.h"
#include "exosnap/engine/error_types.h"
#include "exosnap/engine/recorder_session.h"
#include "exosnap/engine/split_trigger_source.h"
#include "matroska_stream_writer.h"
#include "mux_audio_hold.h"
#include "mux_queue.h"
#include "pipeline_diagnostics_aggregator.h"
#include "premux_state.h"
#include "session_internal.h"
#include "split_sentinel_policy.h"

#include <atomic>
#include <cstdint>
#include <exosnap/engine/logging/logging.h>
#include <exosnap/engine/packet_types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <windows.h>

namespace exosnap::engine {

// ---------------------------------------------------------------------------
// MuxThread
// ---------------------------------------------------------------------------

MuxThread::MuxThread(std::shared_ptr<SessionState> state) : m_state_ptr(std::move(state)), m_state(*m_state_ptr) {
}

MuxThread::~MuxThread() {
    // Start() gave the running thread shared ownership of this object, so a
    // joinable thread here has already returned from Run() (the final release
    // may even happen on the worker thread itself, where join() would
    // deadlock). Detaching a finished thread only releases its handle.
    if (m_thread.joinable())
        m_thread.detach();
}

void MuxThread::Start() {
    // Self-ownership handoff: the lambda keeps this worker (and through
    // m_state_ptr the SessionState) alive until Run() returns — the fix for
    // the stalled-finalize teardown, where the session abandons the worker
    // while Finalize() is still blocked inside the writer.
    m_thread = std::thread([self = shared_from_this()] { self->Run(); });
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
// Run - streaming MKV write with bounded packet holds and segment splitting.
//
// A split is a SplitSentinel in the mux queue (emitted by VideoThread right
// before the forced-keyframe that begins the new segment). The keyframe PTS
// partitions held audio before finalizing the current writer and reporting its
// metadata through the segment callback. The keyframe's session PTS becomes the new
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
    if (!m_state.premux.WaitReady(m_state.config.video_codec, m_state.audio_track_count,
                                  [&] { return m_state.stop_requested.load(); })) {
        // Only stop releases a waiter before codec readiness. The session owns
        // the outcome for a capture that produced no headers or stopped early.
        logging::log(logging::LogLevel::Info, "mux_thread",
                     "stopped before any codec private data arrived; no file written", {});
        return;
    }
    const uint32_t track_count = m_state.audio_track_count;

    if (track_count > CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Mux, "Audio track count exceeds maximum supported");
        return;
    }

    // Save codec private data and encode dimensions for deferred / per-segment track init
    std::vector<uint8_t> video_codec_private;
    std::array<AudioCodecPrivateSlot, CodecPrivateData::kMaxAudioTracks> audioCp{};
    {
        const auto codec_private = m_state.premux.CodecSnapshot();
        if (m_state.config.video_codec == VideoCodec::H264) {
            if (!annexb::BuildAvccFromAnnexBSpsAndPps(codec_private.h264_sps_pps, video_codec_private)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Failed to build AVCC from H.264 SPS/PPS for Matroska");
                return;
            }
        } else if (m_state.config.video_codec == VideoCodec::Hevc) {
            if (!annexb::BuildHvccFromAnnexBVpsSpsPps(codec_private.hevc_vps_sps_pps, video_codec_private)) {
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "Failed to build hvcC from HEVC VPS/SPS/PPS for Matroska");
                return;
            }
        } else {
            video_codec_private = codec_private.av1_codec_private;
        }
        for (uint32_t i = 0; i < track_count; ++i) {
            audioCp[i] = codec_private.audio_codec_private[i];
        }
    }

    uint32_t encW = 0, encH = 0;
    {
        std::lock_guard lk(m_state.stats_mutex);
        encW = m_state.encode_width;
        encH = m_state.encode_height;
    }

    const bool is_h264 = (m_state.config.video_codec == VideoCodec::H264);
    const bool is_hevc = (m_state.config.video_codec == VideoCodec::Hevc);
    const std::filesystem::path base_output_path = m_state.config.output_path;

    // Build a reusable writer config; only output_path changes per segment.
    // codec-private blobs are copied (not moved) because every segment's Tracks
    // element needs its own copy.
    MatroskaStreamConfig sw_config_template;
    sw_config_template.video_codec_id = is_h264 ? "V_MPEG4/ISO/AVC" : (is_hevc ? "V_MPEGH/ISO/HEVC" : "V_AV1");
    sw_config_template.video_codec_private = video_codec_private;
    sw_config_template.encode_width = encW;
    sw_config_template.encode_height = encH;
    sw_config_template.frame_rate_num = m_state.config.frame_rate_num;
    sw_config_template.frame_rate_den = m_state.config.frame_rate_den;
    // Color description — carried from RecorderConfig into the track's
    // Colour element. Default SDR BT.709 limited-range.
    sw_config_template.color = m_state.config.color;
    // An HDR10 session measures its content light levels while it records, so the
    // track header reserves MaxCLL and MaxFALL and finalize_segment patches in
    // what the video thread has accumulated by then. SDR and tone-mapped sessions
    // write an SDR file and have no HDR10 metadata to carry.
    sw_config_template.reserve_content_light_level = sw_config_template.color.hdr;
    switch (m_state.config.audio_codec) {
    case AudioCodec::Opus:
        sw_config_template.audio_codec = StreamAudioCodec::Opus;
        break;
    case AudioCodec::Pcm:
        sw_config_template.audio_codec = StreamAudioCodec::Pcm;
        break;
    case AudioCodec::Flac:
        sw_config_template.audio_codec = StreamAudioCodec::Flac;
        break;
    case AudioCodec::Aac:
    default:
        sw_config_template.audio_codec = StreamAudioCodec::Aac;
        break;
    }
    sw_config_template.webm = (m_state.config.container == Container::WebM);
    sw_config_template.chroma_420 = (m_state.config.chroma == ChromaSubsampling::Cs420);
    sw_config_template.opus_frame_samples =
        static_cast<uint32_t>(OpusFrameSizeSamples(m_state.config.opus_frame_duration));
    sw_config_template.audio_track_count = track_count;
    for (uint32_t i = 0; i < track_count; ++i) {
        sw_config_template.audio_tracks[i].codec_private = audioCp[i].bytes;
        sw_config_template.audio_tracks[i].codec_delay_samples = audioCp[i].codec_delay_samples;
        // Track name, muxed into the container as KaxTrackName. Only the
        // resolved-plan path carries source semantics -- the legacy empty-plan
        // single-loopback-track path (see recorder_session.cpp) has no
        // ResolvedAudioTrack to name, so that track is left unnamed rather than
        // guessed at.
        if (i < m_state.config.audio_track_plan.tracks.size()) {
            sw_config_template.audio_tracks[i].name = DeriveAudioTrackName(m_state.config.audio_track_plan.tracks[i]);
        }
    }
    // Thread audio format from RecorderConfig into the stream writer config.
    // Opus is locked to 48 kHz by Validate(); all other codecs use the configured rate.
    sw_config_template.audio_sample_rate = m_state.config.audio_sample_rate;
    sw_config_template.audio_channels = m_state.config.audio_channels;
    // bit_depth is written for PCM and FLAC only; for lossy codecs the field is
    // ignored by MatroskaStreamWriter::Open() so any value is safe.
    sw_config_template.audio_bit_depth = m_state.config.audio_bit_depth;
    // Float-PCM: selects A_PCM/FLOAT/IEEE over A_PCM/INT/LIT; ignored unless
    // audio_codec == Pcm (see MatroskaStreamConfig::audio_float).
    sw_config_template.audio_float = m_state.config.audio_pcm_float;

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
    // Live-diagnostics baselines for per-segment write deltas (reset on each open).
    uint64_t diag_prev_bytes = 0;
    uint64_t diag_prev_flush = 0;
    uint64_t frame_dur_ns = 0;
    if (m_state.config.frame_rate_num > 0 && m_state.config.frame_rate_den > 0) {
        frame_dur_ns = static_cast<uint64_t>(m_state.config.frame_rate_den) * 1000000000ULL /
                       static_cast<uint64_t>(m_state.config.frame_rate_num);
    }

    const auto open_segment = [&](uint32_t index, uint64_t epoch_session_pts_ns, bool epoch_set) -> bool {
        const SegmentPathResult path_result = DeriveSegmentPath(base_output_path, index);
        if (!path_result.success) {
            // A real filesystem error while probing for a free segment path, or the
            // bounded collision scan was exhausted -- either way this must abort the
            // segment (never silently reuse the last colliding candidate).
            const int32_t hr =
                path_result.error ? HRESULT_FROM_WIN32(static_cast<DWORD>(path_result.error.value())) : E_FAIL;
            m_state.RecordFailure(hr, ErrorPhase::Mux,
                                  "failed to derive segment path (segment " + std::to_string(index) +
                                      "): " + path_result.message);
            return false;
        }
        seg.path = path_result.path;
        seg.index = index;
        seg.epoch_session_pts_ns = epoch_session_pts_ns;
        seg.epoch_set = epoch_set;
        seg.max_local_pts_ns = 0;
        seg.writer = std::make_unique<MatroskaStreamWriter>();
        // Publish cluster-flush progress into SessionState so the shutdown sequence
        // can tell a slow-but-progressing finalize from a stalled one. A new writer
        // starts at 0; the progress-based wait tolerates that reset.
        seg.writer->SetProgressSink(&m_state.mux_bytes_written);

        MatroskaStreamConfig cfg = sw_config_template;
        cfg.output_path = seg.path.string();
        cfg.fail_io = m_state.output_io_failure;
        // Only segment 0 keeps the base path the caller reserved; DeriveSegmentPath
        // mints every later one, so those are the writer's to create exclusively.
        cfg.path_pre_reserved = index == 0 && m_state.config.output_path_pre_reserved;
        if (!seg.writer->Open(cfg)) {
            const std::string err = seg.writer->error();
            seg.writer.reset();
            m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                                  "Matroska stream writer open failed (segment " + std::to_string(index) + "): " + err);
            return false;
        }
        any_segment_opened = true;
        m_state.diagnostics.OnSegmentOpened(index);
        diag_prev_bytes = 0;
        diag_prev_flush = 0;
        logging::LogField fields[] = {{"segment_index", std::to_string(index)}, {"path", seg.path.string()}};
        logging::log(logging::LogLevel::Info, "mux_thread", "segment started",
                     std::span<const logging::LogField>(fields, std::size(fields)));
        return true;
    };

    // Finalize the current segment and report it via the segment callback.
    const auto finalize_segment = [&](bool session_end) {
        if (!seg.writer)
            return;
        // Read as late as possible, because both only rise: the last observation
        // before the header is patched is the one that covers the most frames.
        // A split session finalises its earlier segments mid-recording, so those
        // carry the levels measured up to their own boundary.
        seg.writer->SetContentLightLevels(m_state.measured_max_cll_nits.load(std::memory_order_relaxed),
                                          m_state.measured_max_fall_nits.load(std::memory_order_relaxed));
        const auto finalize_t0 = std::chrono::steady_clock::now();
        const bool ok = seg.writer->Finalize();
        const double finalize_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - finalize_t0).count();
        const std::string err = seg.writer->error();
        const auto& io = seg.writer->io_timings();
        m_state.diagnostics.OnOutputIoFinalized(io.write_ms, io.crt_flush_ms, io.durability_flush_ms,
                                                io.durability_failures);
        const logging::LogField io_fields[] = {{"segment_index", std::to_string(seg.index)},
                                               {"write_ms", std::to_string(io.write_ms)},
                                               {"crt_flush_ms", std::to_string(io.crt_flush_ms)},
                                               {"durability_flush_ms", std::to_string(io.durability_flush_ms)},
                                               {"durability_failures", std::to_string(io.durability_failures)},
                                               {"finalize_ms", std::to_string(finalize_ms)}};
        logging::log(logging::LogLevel::Info, "mux_thread", "output I/O timing",
                     std::span<const logging::LogField>(io_fields, std::size(io_fields)));
        seg.writer.reset();
        m_state.diagnostics.OnSegmentFinalized(finalize_ms, ok);

        // The trailing frame duration represents the last *video* frame's on-screen
        // interval. A segment that never received a video frame (epoch never set — an
        // audio-only flush) has no such frame, so adding a video frame's worth would
        // overstate its duration; use the audio PTS span as-is.
        const uint64_t tail_ns = (seg.max_local_pts_ns > 0 && seg.epoch_set) ? frame_dur_ns : 0;
        const uint64_t local_duration_ns = seg.max_local_pts_ns + tail_ns;
        // Query the real on-disk size regardless of `ok`: a failed Finalize()
        // still leaves the file on disk (see below), and any clusters flushed
        // before the failure are genuine bytes, not zero.
        const uint64_t file_bytes = QueryFileSizeBytes(seg.path);

        CompletedSegment info;
        info.path = seg.path;
        info.index = seg.index;
        info.session_start_ms = seg.epoch_session_pts_ns / 1000000ULL;
        info.duration_ms = local_duration_ns / 1000000ULL;
        info.file_size_bytes = file_bytes;
        info.succeeded = ok;
        info.session_end = session_end;

        if (!ok) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            // Deliberately NOT deleting seg.path here. RecordingCoordinator
            // keeps this segment's recovery-manifest entry precisely because
            // the engine failed (see RecordingCoordinator::FinishRecording,
            // "Engine failed → leave the entry so recovery UI can offer
            // repair" / the RemuxToMkv salvage path) — deleting the
            // file here would silently destroy the footage that flow exists
            // to save. Every cluster flushed before the failure (typically
            // all of them; Finalize() only fails writing the trailer) is
            // still valid Matroska data on disk.
            logging::LogField fields[] = {
                {"segment_index", std::to_string(seg.index)}, {"path", seg.path.string()}, {"error", err}};
            logging::log(logging::LogLevel::Error, "mux_thread", "segment finalize failed; left on disk for recovery",
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
    uint64_t video_epoch_100ns = 0;
    MuxAudioHold pending_audio;

    auto resolve_epoch = [&]() {
        if (epoch_resolved)
            return;
        const uint64_t video_epoch = m_state.video_epoch_qpc_100ns.load();
        if (video_epoch == 0)
            return;
        video_epoch_100ns = video_epoch;
        epoch_resolved = true;
    };

    // Per-track shift that puts an audio track's session PTS on the video
    // timeline (av_epoch_align.h). Resolved lazily on that track's first packet,
    // because AudioThread publishes its measured zero point from the first
    // capture packet that carries device timing — which is always before that
    // packet's audio reaches the muxer. A track whose source attributes no
    // device clock (a multi-source merged track) reports no epoch and falls back
    // to the session baseline: exactly the alignment used before this existed.
    std::array<int64_t, CodecPrivateData::kMaxAudioTracks> audio_shift_ns{};
    std::array<bool, CodecPrivateData::kMaxAudioTracks> audio_shift_resolved{};
    auto audio_shift_for = [&](uint32_t track) -> int64_t {
        if (!epoch_resolved)
            return 0; // no video at all: audio is written un-rebased (see the tail flush)
        if (track >= CodecPrivateData::kMaxAudioTracks)
            return 0;
        if (!audio_shift_resolved[track]) {
            const uint64_t measured = m_state.audio_epoch_qpc_100ns[track].load();
            // A reading that would place the track implausibly far from the
            // picture is a bad clock, not a real offset: fall back to the
            // session baseline (the alignment used before any of this existed)
            // rather than apply a clamped but still seconds-wrong shift.
            const bool usable = IsPlausibleAudioEpoch(measured, video_epoch_100ns);
            if (measured != 0 && !usable) {
                logging::LogField warn[] = {{"track", std::to_string(track)},
                                            {"audio_epoch_100ns", std::to_string(measured)},
                                            {"video_epoch_100ns", std::to_string(video_epoch_100ns)}};
                logging::log(logging::LogLevel::Warn, "mux_thread",
                             "measured audio epoch is implausibly far from the video epoch; "
                             "aligning this track from the session start instead",
                             std::span<const logging::LogField>(warn, std::size(warn)));
            }
            const uint64_t audio_epoch = usable ? measured : m_state.session_start_qpc_100ns;
            audio_shift_ns[track] = AudioTimelineShiftNs(audio_epoch, video_epoch_100ns);
            audio_shift_resolved[track] = true;
            logging::LogField fields[] = {{"track", std::to_string(track)},
                                          {"measured", usable ? "1" : "0"},
                                          {"shift_ms", std::to_string(audio_shift_ns[track] / 1000000LL)}};
            logging::log(logging::LogLevel::Debug, "mux_thread", "audio track aligned to the video epoch",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        }
        return audio_shift_ns[track];
    };

    // Audio held because the current segment's epoch is not known yet. After a
    // split the next segment's epoch is the first video packet written into it,
    // and the audio queue is independent -- so audio can arrive first, and
    // writing it before the epoch exists is what put ten-minute timestamps in a
    // freshly-opened file.
    MuxAudioHold segment_pending_audio;
    // Audio that belonged to a segment already closed. Counted, not just dropped.
    uint64_t trimmed_audio_packets = 0;
    // Highest aligned session PTS actually WRITTEN per audio track, so the
    // duration reported for audio describes the file rather than the encoder.
    std::array<uint64_t, CodecPrivateData::kMaxAudioTracks> aligned_audio_end_ns{};
    MuxAudioHold audio_hold;
    std::optional<SplitSentinel> pending_split;

    std::array<uint64_t, CodecPrivateData::kMaxAudioTracks> audio_codec_delay_ns{};
    for (uint32_t i = 0; i < track_count; ++i) {
        audio_codec_delay_ns[i] = static_cast<uint64_t>(audioCp[i].codec_delay_samples) * 1000000000ULL /
                                  std::max<uint32_t>(1u, m_state.config.audio_sample_rate);
    }
    auto align_audio = [&](EncodedAudioPacket& payload) -> bool {
        if (write_error || !seg.writer)
            return false;
        if (payload.track_id >= track_count || payload.track_id >= CodecPrivateData::kMaxAudioTracks) {
            std::fprintf(stderr, "MuxThread: skipping audio packet with out-of-range track_id=%u (track_count=%u)\n",
                         payload.track_id, track_count);
            return false;
        }
        uint64_t shifted_pts_ns = payload.pts_ns;
        if (!ShiftAudioPts(payload.pts_ns, audio_shift_for(payload.track_id), shifted_pts_ns)) {
            return false; // audio predates the first video frame
        }
        payload.pts_ns = shifted_pts_ns;
        return true;
    };
    auto push_audio = [&](EncodedAudioPacket&& payload) {
        if (write_error || !seg.writer)
            return;
        const SegmentLocalPts placed = PlaceOnSegmentTimeline(payload.pts_ns, seg.epoch_set, seg.epoch_session_pts_ns);
        if (placed.placement == SegmentPlacement::Defer) {
            // No epoch yet: hold it rather than write a session PTS into a
            // segment-local timeline. Drained by drain_segment_pending_audio as
            // soon as the first video packet of this segment sets the epoch.
            if (!segment_pending_audio.Push(std::move(payload))) {
                write_error = true;
                m_state.diagnostics.OnMuxFailure();
                m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                      "Audio waiting for the segment epoch exceeded the mux byte or media-time limit");
            }
            return;
        }
        if (placed.placement == SegmentPlacement::Trim) {
            // Belongs before this segment began, and the previous one is already
            // finalized, so there is nowhere to write it. Counted so a report can
            // say how much audio the split cost instead of it vanishing.
            ++trimmed_audio_packets;
            return;
        }
        // Matroska stores audio block timestamps offset by the track's CodecDelay
        // (the reader subtracts it), so the first audible sample -- not the
        // encoder's priming -- lands where the timeline says it does.
        const uint64_t local = placed.local_pts_ns + audio_codec_delay_ns[payload.track_id];
        MuxPacket mp;
        mp.pts_ns = local;
        mp.track_num = 2 + payload.track_id;
        mp.is_key = true;
        mp.bytes = std::move(payload.bytes);
        if (local > seg.max_local_pts_ns)
            seg.max_local_pts_ns = local;
        if (!seg.writer->Push(std::move(mp))) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            return;
        }
        // The end of the audio this file actually contains, on the same aligned
        // timeline the video duration is on. The audio thread's own last PTS is
        // on the capture timeline and differs from this by the track's shift, so
        // comparing that against the video duration reported the epoch offset as
        // drift.
        const uint64_t aligned_end = payload.pts_ns + audio_codec_delay_ns[payload.track_id];
        if (aligned_end > aligned_audio_end_ns[payload.track_id])
            aligned_audio_end_ns[payload.track_id] = aligned_end;
    };

    auto push_video = [&](EncodedVideoPacket&& payload) {
        if (write_error || !seg.writer)
            return;
        // A new segment's epoch is the first video packet seen after a split. The
        // audio held for this segment is placed right after this call returns
        // (drain_segment_pending_audio at every push_video call site) -- not from
        // in here, because that drain goes back through push_audio.
        if (!seg.epoch_set) {
            seg.epoch_session_pts_ns = payload.pts_ns;
            seg.epoch_set = true;
        }
        // The epoch was just set from this packet when it is the segment's first,
        // so Write is the only outcome here; Defer cannot happen and a video
        // packet before its own segment's IDR epoch violates a closed GOP.
        const SegmentLocalPts placed = PlaceOnSegmentTimeline(payload.pts_ns, seg.epoch_set, seg.epoch_session_pts_ns);
        if (placed.placement != SegmentPlacement::Write) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                  "video packet could not be placed on its own segment's timeline");
            return;
        }
        const uint64_t local = placed.local_pts_ns;
        MuxPacket mp;
        mp.pts_ns = local;
        mp.track_num = 1;
        if (payload.dts_ns)
            mp.dts_ns = *payload.dts_ns - static_cast<int64_t>(seg.epoch_session_pts_ns);
        mp.is_key = payload.keyframe;
        // H.264/HEVC samples must be length-prefixed to match the avcC/hvcC
        // CodecPrivate written into the Tracks element. A failed conversion used
        // to fall back to the raw Annex-B bytes, which produces a file whose
        // samples do not match its own track header — a structurally invalid
        // track that players reject, reported to the user as a success. Treat it
        // as a mux failure instead, so the recording fails loudly and the partial
        // file goes down the recovery path like any other mux error.
        if (is_h264) {
            std::vector<uint8_t> avcc;
            if (!annexb::ConvertAnnexBToAvcc(payload.bytes.data(), payload.bytes.size(), avcc)) {
                write_error = true;
                m_state.diagnostics.OnMuxFailure();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "Failed to convert H.264 Annex-B packet to AVCC for Matroska");
                return;
            }
            mp.bytes = std::move(avcc);
        } else if (is_hevc) {
            std::vector<uint8_t> hvcc_sample;
            if (!annexb::ConvertAnnexBToHevcSample(payload.bytes.data(), payload.bytes.size(), hvcc_sample)) {
                write_error = true;
                m_state.diagnostics.OnMuxFailure();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "Failed to convert HEVC Annex-B packet to hvcC sample for Matroska");
                return;
            }
            mp.bytes = std::move(hvcc_sample);
        } else {
            mp.bytes = std::move(payload.bytes);
        }
        if (local > seg.max_local_pts_ns)
            seg.max_local_pts_ns = local;
        if (!seg.writer->Push(std::move(mp))) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            return;
        }
    };

    auto drain_audio_hold = [&](bool all) {
        while (!write_error) {
            auto packet = all ? audio_hold.Pop() : audio_hold.PopEligible();
            if (!packet)
                break;
            push_audio(std::move(*packet));
        }
    };
    auto hold_audio = [&](EncodedAudioPacket&& payload) {
        if (!align_audio(payload))
            return;
        if (!audio_hold.Push(std::move(payload))) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                  "Audio waiting for video exceeded the mux byte or media-time limit");
        }
    };
    auto buffer_pending_audio = [&](EncodedAudioPacket&& payload) {
        if (!pending_audio.Push(std::move(payload))) {
            write_error = true;
            m_state.diagnostics.OnMuxFailure();
            m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                  "Audio waiting for the video epoch exceeded the mux byte or media-time limit");
        }
    };
    auto drain_pending_audio = [&]() {
        if (!epoch_resolved)
            return;
        while (!write_error) {
            auto packet = pending_audio.Pop();
            if (!packet)
                break;
            hold_audio(std::move(*packet));
        }
    };

    // Called once the current segment's epoch is known. Each held packet goes
    // back through push_audio on its already aligned timeline.
    auto drain_segment_pending_audio = [&]() {
        if (!seg.epoch_set)
            return;
        while (!write_error) {
            auto packet = segment_pending_audio.Pop();
            if (!packet)
                break;
            push_audio(std::move(*packet));
        }
    };

    // The adjacent keyframe provides the actual split boundary. Audio may have
    // arrived ahead of this delayed video output and still belongs to either file.
    auto begin_new_segment = [&](const SplitSentinel& s, uint64_t boundary_ns) {
        if (write_error)
            return;
        // Flush any buffered pre-epoch audio into the OLD segment first -- both
        // the session-level hold (audio that predates the first video frame of
        // the recording) and anything held for this segment, which still has its
        // own epoch and can therefore still place it.
        drain_pending_audio();
        while (!write_error) {
            auto packet = audio_hold.PopBefore(boundary_ns);
            if (!packet)
                break;
            push_audio(std::move(*packet));
        }
        drain_segment_pending_audio();
        finalize_segment(/*session_end=*/false);
        // finalize_segment() sets write_error on a finalize/I-O failure (failure
        // isolation: quarantine the incomplete current file, keep prior segments).
        // cppcheck cannot see the captured-by-ref mutation through the lambda call.
        // cppcheck-suppress identicalConditionAfterEarlyExit
        if (write_error)
            return;
        // Open the next segment with epoch unset; push_video sets it from the
        // first video packet (the forced keyframe that follows the sentinel).
        m_state.diagnostics.OnSplitTransition(ToDiagnosticsSplitTrigger(s.trigger));
        // Reset size-split guard so the new segment can trigger if it also exceeds
        // the threshold.
        m_state.size_split_armed.store(false);
        open_segment(s.new_segment_index, /*epoch*/ 0, /*epoch_set*/ false);
    };

    // A single dispatch for a queued MuxItem payload.
    auto handle_payload = [&](auto&& payload, bool& video_eos,
                              std::array<bool, CodecPrivateData::kMaxAudioTracks>& audio_eos) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
            if (!payload.bytes.empty()) {
                m_state.diagnostics.OnMuxPacket(payload.bytes.size());
                resolve_epoch();
                if (pending_split) {
                    if (!payload.keyframe) {
                        write_error = true;
                        m_state.diagnostics.OnMuxFailure();
                        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Split boundary video is not a keyframe");
                        return;
                    }
                    begin_new_segment(*pending_split, payload.pts_ns);
                    pending_split.reset();
                }
                const uint64_t video_pts_ns = payload.pts_ns;
                push_video(std::move(payload));
                audio_hold.AdvanceVideo(video_pts_ns);
                drain_pending_audio();
                drain_audio_hold(false);
                drain_segment_pending_audio();
            }
        } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
            if (!payload.bytes.empty()) {
                m_state.diagnostics.OnMuxPacket(payload.bytes.size());
                resolve_epoch();
                if (epoch_resolved) {
                    drain_pending_audio();
                    hold_audio(std::move(payload));
                    drain_audio_hold(video_eos && !pending_split);
                } else {
                    buffer_pending_audio(std::move(payload));
                }
            }
        } else if constexpr (std::is_same_v<T, VideoProgressSentinel>) {
            if (pending_split) {
                write_error = true;
                m_state.diagnostics.OnMuxFailure();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Video progress interrupted a split boundary");
                return;
            }
            audio_hold.AdvanceVideo(payload.safe_before_pts_ns);
            drain_pending_audio();
            drain_audio_hold(false);
        } else if constexpr (std::is_same_v<T, SplitSentinel>) {
            if (pending_split) {
                write_error = true;
                m_state.diagnostics.OnMuxFailure();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Split boundary has no adjacent keyframe");
                return;
            }
            pending_split = payload;
        } else if constexpr (std::is_same_v<T, VideoEosSentinel>) {
            video_eos = true;
            drain_pending_audio();
            if (!pending_split)
                drain_audio_hold(true);
        } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
            if (payload.track_id < track_count && payload.track_id < CodecPrivateData::kMaxAudioTracks)
                audio_eos[payload.track_id] = true;
        }
    };

    // 2a. Drain premux buffers (packets captured before tracks were initialized).
    {
        auto pending = m_state.premux.TakePending();
        for (auto& pkt : pending.video) {
            if (pkt.bytes.empty())
                continue;
            resolve_epoch();
            const uint64_t video_pts_ns = pkt.pts_ns;
            push_video(std::move(pkt));
            audio_hold.AdvanceVideo(video_pts_ns);
            drain_pending_audio();
            drain_audio_hold(false);
            drain_segment_pending_audio();
        }
        for (auto& pkt : pending.audio) {
            if (pkt.bytes.empty())
                continue;
            resolve_epoch();
            if (epoch_resolved) {
                drain_pending_audio();
                hold_audio(std::move(pkt));
                drain_audio_hold(false);
            } else {
                buffer_pending_audio(std::move(pkt));
            }
        }
    }

    // Size-split threshold (SPLIT-BY-SIZE-R1): 0 means disabled.
    const uint64_t size_split_threshold = m_state.config.split.size_bytes;

    // Surface live mux/disk/reorder-window metrics from the active writer (filesystem
    // write boundary). Called once per mux-loop iteration outside the queue lock.
    // Also triggers a size-based split when the segment byte count crosses the threshold.
    const auto sample_mux_diagnostics = [&]() {
        if (!seg.writer)
            return;
        m_state.diagnostics.OnReorderWindow(static_cast<uint32_t>(seg.writer->current_window_packets()),
                                            static_cast<uint32_t>(seg.writer->peak_window_packets()),
                                            seg.writer->current_window_bytes(), seg.writer->peak_window_bytes());
        const uint64_t bytes = seg.writer->bytes_written();
        const uint64_t flushes = seg.writer->flush_count();
        if (flushes != diag_prev_flush) {
            const uint64_t byte_delta = (bytes >= diag_prev_bytes) ? (bytes - diag_prev_bytes) : 0;
            m_state.diagnostics.OnDiskWrite(std::chrono::steady_clock::now(), seg.writer->last_flush_ms(), byte_delta);
            diag_prev_flush = flushes;
            diag_prev_bytes = bytes;
        }
        // Size-based split check (SPLIT-BY-SIZE-R1): if the committed byte count for
        // the current segment crosses the threshold and we haven't yet armed a size
        // split for this segment, request one via the same seq path as manual splits.
        // VideoThread sees the seq bump, arms a forced keyframe, and enqueues a
        // SplitSentinel; begin_new_segment resets size_split_armed for the next segment.
        if (size_split_threshold > 0 && bytes >= size_split_threshold && !m_state.size_split_armed.load()) {
            bool expected = false;
            if (m_state.size_split_armed.compare_exchange_strong(expected, true)) {
                // Same compare-exchange as the manual path: sequence and trigger
                // become visible together.
                uint64_t observed_request = m_state.split_request.load(std::memory_order_relaxed);
                while (!m_state.split_request.compare_exchange_weak(
                    observed_request,
                    SplitRequestWith(observed_request, static_cast<uint32_t>(SplitTriggerSource::AutomaticSize)),
                    std::memory_order_release, std::memory_order_relaxed)) {
                }
                logging::LogField fields[] = {{"segment_index", std::to_string(seg.index)},
                                              {"bytes", std::to_string(bytes)},
                                              {"threshold", std::to_string(size_split_threshold)}};
                logging::log(logging::LogLevel::Info, "mux_thread", "size threshold reached; size split requested",
                             std::span<const logging::LogField>(fields, std::size(fields)));
            }
        }
    };

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

        const auto mux_queue_depth =
            static_cast<uint32_t>(m_state.mux_queue.WaitForItems([&] { return m_state.stop_requested.load(); }));
        if (m_state.HasFailure())
            break;

        const auto mux_t0 = std::chrono::steady_clock::now();
        bool processed_any = false;
        while (auto item = m_state.mux_queue.Pop()) {
            std::visit([&](auto&& payload) { handle_payload(std::move(payload), videoEos, audioEosReceived); },
                       item->payload);
            processed_any = true;
        }
        if (processed_any) {
            const auto mux_t1 = std::chrono::steady_clock::now();
            m_state.diagnostics.OnMuxLatency(mux_t1,
                                             std::chrono::duration<double, std::milli>(mux_t1 - mux_t0).count());
        }
        m_state.diagnostics.OnVideoQueueDepth(mux_queue_depth);
        sample_mux_diagnostics();
    }

    // 2c. Final drain for race-window packets (arrived after both EOS sentinels).
    {
        // Same shape as the main loop: a split sentinel here finalizes a file
        // and runs the segment callback, and a producer that is still pushing
        // its own EOS must not wait on the queue lock for that.
        while (auto item = m_state.mux_queue.Pop()) {
            std::visit([&](auto&& payload) { handle_payload(std::move(payload), videoEos, audioEosReceived); },
                       item->payload);
        }
    }

    // If the epoch never resolved (no video frame at all), flush whatever audio
    // was buffered un-rebased so it is not silently lost.
    if (!epoch_resolved) {
        while (!write_error) {
            auto packet = pending_audio.Pop();
            if (!packet)
                break;
            if (align_audio(*packet))
                push_audio(std::move(*packet));
        }
    }
    if (pending_split && !write_error) {
        write_error = true;
        m_state.diagnostics.OnMuxFailure();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "Split boundary ended before its keyframe arrived");
    }
    drain_audio_hold(true);
    // Audio still held for the last segment. With an epoch it is placed as
    // usual; without one this segment never got a video packet, so there is no
    // timeline to place it on and it is counted as trimmed rather than written
    // at a timestamp that means nothing.
    drain_segment_pending_audio();
    while (segment_pending_audio.Pop())
        ++trimmed_audio_packets;

    // The aligned audio ends, published where the collector can compare them
    // against the video duration on the same timeline.
    {
        std::lock_guard lk(m_state.stats_mutex);
        for (uint32_t i = 0; i < track_count && i < m_state.stats.aligned_audio_duration_ns.size(); ++i)
            m_state.stats.aligned_audio_duration_ns[i] = aligned_audio_end_ns[i];
    }

    // What the splits cost, stated once. Audio the muxer could not place is
    // audio the file does not contain; a report that said nothing about it would
    // describe a recording that is complete.
    if (trimmed_audio_packets > 0) {
        {
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.audio_packets_trimmed_at_split = trimmed_audio_packets;
        }
        const logging::LogField fields[] = {{"packets", std::to_string(trimmed_audio_packets)}};
        logging::log(logging::LogLevel::Warn, "mux_thread",
                     "audio packets arrived for a segment that was already closed and were trimmed",
                     std::span<const logging::LogField>(fields, std::size(fields)));
    }

    // --- Step 3: Finalize the final segment ---
    finalize_segment(/*session_end=*/true);

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

} // namespace exosnap::engine
