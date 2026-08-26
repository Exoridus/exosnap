#include <exosnap/engine/recorder_session.h>

#include "audio_thread.h"
#include "brickwall_limiter.h"
#include "finalize_join_policy.h"
#include "mic_dsp_audio_src.h"
#include "mixed_audio_src.h"
#include "mux_thread.h"
#include "session_internal.h"
#include "session_stats_collector.h"
#include "session_stop_reset.h"
#include "video_thread.h"
#include "wasapi_capture_src.h"
#include "wasapi_loopback_src.h"
#include "wasapi_process_loopback_src.h"
#include "wgc_capture.h"
#include "worker_join.h"

#include <exosnap/engine/logging/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace exosnap::engine {

// ---------------------------------------------------------------------------
// DeriveTransientMkvPath
// ---------------------------------------------------------------------------

std::filesystem::path DeriveTransientMkvPath(const std::filesystem::path& mp4_output_path) {
    // Replace .mp4 extension with .mkv.tmp so the transient file sits next to the
    // intended output without colliding with any real MKV the user might have.
    std::filesystem::path result = mp4_output_path;
    result.replace_extension(L".mkv.tmp");
    return result;
}

// ---------------------------------------------------------------------------
// DeriveSegmentPath
// ---------------------------------------------------------------------------

SegmentPathResult DeriveSegmentPath(const std::filesystem::path& base, std::uint32_t index,
                                    const SegmentPathExistsProbe& exists_probe) {
    // Segment 0 keeps the base path verbatim (no rename of the first file, no
    // existence probe -- this mirrors the pre-existing contract exactly).
    if (index == 0) {
        return SegmentPathResult::Ok(base);
    }

    const std::filesystem::path parent = base.parent_path();
    const std::wstring stem = base.stem().wstring();
    const std::wstring ext = base.extension().wstring(); // includes leading '.'

    // Zero-padded 3-digit, locale-independent. index is 0-based; the on-disk
    // part number is index+1 so the second segment is "_part-002".
    const unsigned part_number = index + 1u;
    wchar_t numbuf[8] = {};
    swprintf(numbuf, 8, L"%03u", part_number);

    auto compose = [&](unsigned disambiguator) -> std::filesystem::path {
        std::wstring name = stem + L"_part-" + numbuf;
        if (disambiguator > 0) {
            wchar_t dbuf[16] = {};
            swprintf(dbuf, 16, L"_%u", disambiguator);
            name += dbuf;
        }
        name += ext;
        return parent.empty() ? std::filesystem::path(name) : (parent / name);
    };

    const SegmentPathExistsProbe default_probe = [](const std::filesystem::path& p, std::error_code& ec) {
        return std::filesystem::exists(p, ec);
    };
    const SegmentPathExistsProbe& probe = exists_probe ? exists_probe : default_probe;

    // Collision-safe: probe up to kMaxDisambiguator candidates ("_part-NNN",
    // then "_part-NNN_1", "_part-NNN_2", ...). Only a candidate CONFIRMED free
    // (probe returned false with an empty error_code) is ever returned as
    // success. A real filesystem error while probing (permissions, unreadable
    // path, ...) is propagated immediately -- it must never be treated the
    // same as "path is free". Exhausting the bound without finding a free
    // candidate is a defined failure, not a silent return of the last
    // (still-colliding) candidate.
    constexpr unsigned kMaxDisambiguator = 10000u;
    for (unsigned d = 0; d < kMaxDisambiguator; ++d) {
        const std::filesystem::path candidate = compose(d);
        std::error_code ec;
        const bool candidate_exists = probe(candidate, ec);
        if (ec) {
            return SegmentPathResult::Fail(ec, "failed to probe segment path for collision: " + candidate.string() +
                                                   ": " + ec.message());
        }
        if (!candidate_exists) {
            return SegmentPathResult::Ok(candidate);
        }
    }
    return SegmentPathResult::Fail(std::error_code(static_cast<int>(ERROR_ALREADY_EXISTS), std::system_category()),
                                   "segment path collision limit (" + std::to_string(kMaxDisambiguator) +
                                       " candidates) exhausted for base: " + base.string());
}

// ---------------------------------------------------------------------------
// RecorderSession::Impl
// ---------------------------------------------------------------------------

struct RecorderSession::Impl {
    // SessionState is shared with the worker threads (each worker's thread
    // lambda holds it alive through the worker object, see audio_thread.h).
    // The pointer itself is guarded by state_mutex: Record() swaps in a fresh
    // state when a previous session abandoned a stalled worker, so a still-
    // running leaked worker keeps writing through ITS state while the new
    // session gets an untouched one.
    std::mutex state_mutex;
    std::shared_ptr<SessionState> state = std::make_shared<SessionState>();
    bool workers_leaked = false; // guarded by state_mutex
    // A Stop() that arrives outside Record()'s active-recording window (see
    // session_stop_reset.h). Deliberately NOT on SessionState: it must survive
    // the state swap above. Guarded by state_mutex.
    PendingStopTracker pending_stop;

    StatsCallback stats_callback;
    MeterCallback meter_callback;
    DiagnosticsCallback diagnostics_callback;
    PreviewSharedHandleCallback preview_shared_handle_callback;
    PreviewFramePublishedCallback preview_frame_published_callback;
    SegmentCallback segment_callback;
    uint64_t diagnostics_generation{0};
    std::atomic<bool> recording{false};

    std::shared_ptr<SessionState> State() {
        std::lock_guard lk(state_mutex);
        return state;
    }
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

RecorderSession::RecorderSession() : m_impl(new Impl{}) {
}

RecorderSession::~RecorderSession() {
    delete m_impl;
}

// ---------------------------------------------------------------------------
// EnumerateTargets
// ---------------------------------------------------------------------------

/*static*/
std::vector<CaptureTarget> RecorderSession::EnumerateTargets() {
    return EnumerateWgcTargets();
}

// ---------------------------------------------------------------------------
// SetStatsCallback
// ---------------------------------------------------------------------------

void RecorderSession::SetStatsCallback(StatsCallback cb) {
    m_impl->stats_callback = std::move(cb);
}

void RecorderSession::SetMeterCallback(MeterCallback cb) {
    m_impl->meter_callback = std::move(cb);
}

void RecorderSession::SetDiagnosticsCallback(DiagnosticsCallback cb) {
    m_impl->diagnostics_callback = std::move(cb);
}

void RecorderSession::SetPreviewSharedHandleCallback(PreviewSharedHandleCallback cb) {
    m_impl->preview_shared_handle_callback = std::move(cb);
}

void RecorderSession::SetPreviewFramePublishedCallback(PreviewFramePublishedCallback cb) {
    m_impl->preview_frame_published_callback = std::move(cb);
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

bool RecorderSession::Validate(const RecorderConfig& config, RecorderResult* out_result) {
    auto fail = [&](int32_t hr, ErrorPhase phase, const std::string& detail) -> bool {
        if (out_result) {
            out_result->succeeded = false;
            out_result->error_code = hr;
            out_result->error_phase = phase;
            out_result->error_detail = detail;
        }
        return false;
    };

    // Output path must not be empty
    if (config.output_path.empty()) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "output_path is empty");
    }

    // Parent directory must exist
    auto parent = config.output_path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        return fail(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), ErrorPhase::Prepare,
                    "output directory does not exist: " + parent.string());
    }

    // Container: WebM, Matroska, and Mp4 are supported
    if (config.container != Container::WebM && config.container != Container::Matroska &&
        config.container != Container::Mp4) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "Only Container::WebM, Container::Matroska, and Container::Mp4 are supported");
    }

    // Video codec
    if (config.container == Container::Mp4) {
        // MP4 video: H.264 (Recommended) or HEVC. HEVC is recorded to the transient
        // MKV as V_MPEGH/ISO/HEVC and remuxed to MP4 with the 'hvc1' sample-entry
        // FourCC (parameter sets out-of-band in hvcC) for Apple/QuickTime/NLE
        // compatibility (0.7.0; container compat registry → Allowed, ADR 0010/0014).
        if (config.video_codec != VideoCodec::H264 && config.video_codec != VideoCodec::Hevc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare, "Container::Mp4 requires VideoCodec::H264 or VideoCodec::Hevc");
        }
    } else if (config.container == Container::WebM) {
        if (config.video_codec != VideoCodec::Av1) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare, "Container::WebM requires VideoCodec::Av1");
        }
    } else {
        // Container::Matroska: AV1, H.264, and HEVC are supported. HEVC NVENC is
        // muxed as V_MPEGH/ISO/HEVC with an hvcC codec-private blob and
        // length-prefixed samples (0.7.0; container compat registry → Allowed).
        if (config.video_codec != VideoCodec::Av1 && config.video_codec != VideoCodec::H264 &&
            config.video_codec != VideoCodec::Hevc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare,
                        "Container::Matroska requires VideoCodec::Av1, VideoCodec::H264, "
                        "or VideoCodec::Hevc");
        }
    }

    // Audio codec
    if (config.container == Container::Mp4) {
        // MP4 audio is AAC only. PCM is deferred (libavformat emits ipcm which has
        // limited player support); FLAC and Opus are also rejected for MP4 (ADR 0010,
        // ADR 0028, ADR 0030). Use MKV for PCM or FLAC.
        if (config.audio_codec != AudioCodec::Aac) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "Container::Mp4 requires AudioCodec::Aac; "
                        "PCM is deferred (ipcm sample entry, limited player support), "
                        "Opus and FLAC are not valid for MP4");
        }
    } else if (config.audio_codec == AudioCodec::Opus) {
        // Opus: valid for WebM and Matroska
    } else if (config.audio_codec == AudioCodec::Aac) {
        if (config.container == Container::WebM) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::Aac is not valid for Container::WebM; use AudioCodec::Opus");
        }
    } else if (config.audio_codec == AudioCodec::Pcm) {
        // PCM (A_PCM/INT/LIT): Matroska only.
        if (config.container != Container::Matroska) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::Pcm is only valid for Container::Matroska (MKV); "
                        "WebM and MP4 cannot carry PCM audio in this build");
        }
    } else if (config.audio_codec == AudioCodec::Flac) {
        // FLAC (A_FLAC): Matroska only.
        if (config.container != Container::Matroska) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::Flac is only valid for Container::Matroska (MKV); "
                        "WebM and MP4 cannot carry A_FLAC in this build");
        }
    } else {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "Unsupported audio codec; supported: AudioCodec::Opus, AudioCodec::Aac, "
                    "AudioCodec::Pcm, AudioCodec::Flac");
    }

    // ---------------------------------------------------------------------------
    // Audio format model validation (ADR 0030)
    // ---------------------------------------------------------------------------

    // audio_channels: only mono (1) and stereo (2) are supported.
    if (config.audio_channels != 1 && config.audio_channels != 2) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "audio_channels must be 1 (mono) or 2 (stereo)");
    }

    // audio_sample_rate: vetted set only.
    if (config.audio_sample_rate != 44100 && config.audio_sample_rate != 48000 && config.audio_sample_rate != 96000) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "audio_sample_rate must be 44100, 48000, or 96000 Hz");
    }

    // Opus requires exactly 48000 Hz.
    if (config.audio_codec == AudioCodec::Opus && config.audio_sample_rate != 48000) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "AudioCodec::Opus requires audio_sample_rate == 48000 Hz");
    }

    // audio_bit_depth: codec-gated.
    if (config.audio_codec == AudioCodec::Pcm) {
        if (config.audio_bit_depth != 16 && config.audio_bit_depth != 24 && config.audio_bit_depth != 32) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "AudioCodec::Pcm requires audio_bit_depth in {16, 24, 32}");
        }
    } else if (config.audio_codec == AudioCodec::Flac) {
        if (config.audio_bit_depth != 16 && config.audio_bit_depth != 24) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "AudioCodec::Flac requires audio_bit_depth in {16, 24}");
        }
        // flac_compression_level: [0, 8]
        if (config.flac_compression_level < 0 || config.flac_compression_level > 8) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "flac_compression_level must be in [0, 8]");
        }
    }
    // Lossy codecs (Opus, AAC): bit_depth is not applicable; no validation needed.

    // audio_pcm_float: 32-bit float PCM (A_PCM/FLOAT/IEEE) is Pcm-codec-only
    // and requires audio_bit_depth == 32 -- the only depth the mix bus can
    // supply without a lossy int conversion.
    if (config.audio_pcm_float) {
        if (config.audio_codec != AudioCodec::Pcm) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "audio_pcm_float requires AudioCodec::Pcm");
        }
        if (config.audio_bit_depth != 32) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "audio_pcm_float requires audio_bit_depth == 32");
        }
    }

    // Chroma: Cs420 is universal. Cs444 (AYUV, NVENC High 4:4:4 / HEVC FREXT) is an
    // 8-bit H.264/HEVC expert path — AV1 NVENC is 4:2:0 only, and 4:4:4 + 10-bit is
    // out of scope.
    if (config.chroma != ChromaSubsampling::Cs420 && config.chroma != ChromaSubsampling::Cs444) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Unsupported ChromaSubsampling; supported: Cs420, Cs444");
    }
    if (config.chroma == ChromaSubsampling::Cs444) {
        if (config.video_codec != VideoCodec::H264 && config.video_codec != VideoCodec::Hevc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare,
                        "ChromaSubsampling::Cs444 requires VideoCodec::H264 or VideoCodec::Hevc "
                        "(AV1 NVENC is 4:2:0 only)");
        }
        if (config.bit_depth != BitDepth::Bit8) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare, "ChromaSubsampling::Cs444 is 8-bit only");
        }
    }

    // Bit depth: Bit8 is universal. Bit10 (P010 → HEVC Main10 / AV1 10-bit, SDR BT.709,
    // ADR 0032) is valid only for Hevc and Av1 — H.264 stays 8-bit only. The
    // container constraints are identical to the 8-bit path for the same codec (already
    // enforced above): HEVC → MKV/MP4, AV1 → MKV/WebM.
    if (config.bit_depth != BitDepth::Bit8 && config.bit_depth != BitDepth::Bit10) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Unsupported BitDepth; supported: Bit8, Bit10");
    }
    if (config.bit_depth == BitDepth::Bit10 && config.video_codec != VideoCodec::Hevc &&
        config.video_codec != VideoCodec::Av1) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "BitDepth::Bit10 requires VideoCodec::Hevc or VideoCodec::Av1 "
                    "(H.264 is 8-bit only)");
    }

    // Frame rate sanity
    if (config.frame_rate_num == 0 || config.frame_rate_den == 0) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "frame_rate_num and frame_rate_den must be non-zero");
    }
    if ((config.output_width == 0) != (config.output_height == 0)) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare,
                    "output_width and output_height must both be zero or both be non-zero");
    }
    if (config.output_width != 0 || config.output_height != 0) {
        if (!IsEncoderAlignedSize({config.output_width, config.output_height})) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "output_width and output_height must be positive even encoder dimensions");
        }
    }

    if (config.record_audio) {
        if (config.audio_track_plan.tracks.size() > CodecPrivateData::kMaxAudioTracks) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "audio_track_plan: max 3 audio tracks supported");
        }

        for (const auto& track : config.audio_track_plan.tracks) {
            if (track.sources.size() < 1 || track.sources.size() > 3) {
                return fail(E_NOTIMPL, ErrorPhase::Prepare, "Audio tracks must contain between 1 and 3 sources.");
            }

            for (const auto src_kind : track.sources) {
                if ((src_kind == AudioSourceKind::App || src_kind == AudioSourceKind::Sys) &&
                    (!config.audio_target_process_id.has_value() || config.audio_target_process_id.value() == 0)) {
                    return fail(E_INVALIDARG, ErrorPhase::Prepare,
                                "audio_target_process_id must be a non-zero PID for App/Sys audio sources");
                }
            }
        }
    }

    // Capture target: must have a valid handle for the stated kind
    if (config.target.kind == CaptureTarget::Kind::Monitor && config.target.native_id == 0) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "CaptureTarget::Kind::Monitor requires a non-zero native_id");
    }
    if (config.target.kind == CaptureTarget::Kind::Window && config.target.native_id == 0) {
        return fail(E_INVALIDARG, ErrorPhase::Prepare, "CaptureTarget::Kind::Window requires a non-zero native_id");
    }

    // Crop region: requires Monitor target and valid dimensions
    if (config.crop_region.has_value()) {
        if (config.target.kind != CaptureTarget::Kind::Monitor) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare, "crop_region requires CaptureTarget::Kind::Monitor");
        }
        if (!config.crop_region->IsValid()) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "crop_region dimensions too small: " + std::to_string(config.crop_region->width) + "x" +
                            std::to_string(config.crop_region->height) + " (minimum " +
                            std::to_string(CaptureRegion::kMinDimension) + "x" +
                            std::to_string(CaptureRegion::kMinDimension) + ")");
        }
    }

    if (out_result) {
        out_result->succeeded = true;
        out_result->error_code = S_OK;
        out_result->error_phase = ErrorPhase::None;
        out_result->error_detail = {};
    }
    return true;
}

// ---------------------------------------------------------------------------
// Stop
// ---------------------------------------------------------------------------

void RecorderSession::Stop() {
    std::shared_ptr<SessionState> st;
    {
        std::lock_guard lk(m_impl->state_mutex);
        st = m_impl->state;
        // Only a Stop() outside the active-recording window needs to survive into
        // the next Record() call — see session_stop_reset.h.
        m_impl->pending_stop.NoteStop(m_impl->recording.load());
    }
    st->pause_requested.store(false);
    st->stop_requested.store(true);
    st->SignalStopEvent();
    st->premux_cv.notify_all();
    st->mux_cv.notify_all();
    st->mux_space_cv.notify_all();
}

void RecorderSession::Pause() {
    m_impl->State()->pause_requested.store(true);
}

void RecorderSession::Resume() {
    m_impl->State()->pause_requested.store(false);
}

void RecorderSession::RequestSplit(SplitTriggerSource source) {
    if (!m_impl->recording.load())
        return;
    // Record the trigger (for logging) before bumping the sequence so the
    // observing thread sees a consistent (seq, trigger) pair.
    const auto st = m_impl->State();
    st->split_last_trigger.store(static_cast<uint32_t>(source));
    st->split_request_seq.fetch_add(1);
}

void RecorderSession::SetAudioSourceMuted(AudioSourceKind kind, bool muted) noexcept {
    if (!m_impl->recording.load()) {
        return;
    }
    const auto st = m_impl->State();
    const uint32_t bit = AudioSourceKindBit(kind);
    if (muted) {
        st->audio_mute_mask.fetch_or(bit, std::memory_order_relaxed);
    } else {
        st->audio_mute_mask.fetch_and(~bit, std::memory_order_relaxed);
    }
}

void RecorderSession::SetSegmentCallback(SegmentCallback cb) {
    // Stored on the Impl (not the SessionState) so the callback survives the
    // fresh-state swap after a leaked worker; Record() copies it in.
    m_impl->segment_callback = std::move(cb);
}

void RecorderSession::RequestFrameSnapshot(FrameSnapshotCallback callback) {
    if (!m_impl->recording.load())
        return;
    const auto st = m_impl->State();
    std::lock_guard lk(st->snapshot_callback_mutex);
    if (st->snapshot_requested.load())
        return; // already pending — ignore
    st->snapshot_callback = std::move(callback);
    st->snapshot_requested.store(true);
}

void RecorderSession::UpdateWebcamOverlay(const WebcamOverlayLive& overlay) {
    if (!m_impl->recording.load()) {
        return;
    }
    const auto st = m_impl->State();
    if (st->config.webcam.frame_provider == nullptr) {
        return;
    }
    st->UpdateWebcamOverlay(overlay);
}

// ---------------------------------------------------------------------------
// Record  (blocking)
// ---------------------------------------------------------------------------

RecorderResult RecorderSession::Record(const RecorderConfig& config) {
    // Validate first
    RecorderResult validationResult;
    if (!Validate(config, &validationResult)) {
        return validationResult;
    }

    // ADR-0014: MP4 remux-on-stop. The engine always records to MKV (Matroska).
    // When the user selected MP4, redirect the engine to a transient MKV path;
    // the app layer handles the remux and deletion after this call returns.
    RecorderConfig engine_config = config;
    if (config.container == Container::Mp4) {
        engine_config.container = Container::Matroska;
        engine_config.output_path = DeriveTransientMkvPath(config.output_path);
    }

    // A previous session that abandoned a stalled/hung worker leaked that
    // worker with shared ownership of ITS SessionState. Never reuse that state:
    // swap in a fresh one so the leaked worker drains against the old state
    // (kept alive by its own shared_ptr) while this session starts clean.
    std::shared_ptr<SessionState> state_ptr;
    bool pre_stop = false;
    {
        std::lock_guard lk(m_impl->state_mutex);
        if (m_impl->workers_leaked) {
            m_impl->state = std::make_shared<SessionState>();
            m_impl->workers_leaked = false;
        }
        state_ptr = m_impl->state;
        // Consumed here (under the same lock as the swap above) so a Stop() that
        // raced ahead of this Record() call is honored regardless of whether the
        // swap just happened — see session_stop_reset.h.
        pre_stop = m_impl->pending_stop.Consume();
    }

    // Reset session state
    {
        auto& st = *state_ptr;
        // Preserves (instead of discarding) a Stop() that raced ahead of this
        // Record() call — see session_stop_reset.h.
        ResetStopRequestedForNewSession(st, pre_stop);
        // A Pause() left set by a session that ended without going through Stop()
        // (e.g. RecordFailure while paused) must not silently carry into the next
        // recording.
        st.pause_requested.store(false);
        {
            std::lock_guard lk(st.failure_mutex);
            st.failure_recorded = false;
            st.failure = {};
        }
        {
            std::lock_guard lk(st.premux_mutex);
            st.video_premux.clear();
            st.audio_premux.clear();
            st.codec_private = {};
        }
        {
            std::lock_guard lk(st.mux_mutex);
            st.mux_queue.clear();
            st.mux_queue_bytes = 0;
        }
        {
            std::lock_guard lk(st.stats_mutex);
            st.stats = {};
        }
        st.config = engine_config;
        st.SeedWebcamOverlayFromConfig();
        if (!config.record_audio) {
            st.audio_track_count = 0;
        } else {
            st.audio_track_count = config.audio_track_plan.tracks.empty()
                                       ? 1u
                                       : static_cast<uint32_t>(config.audio_track_plan.tracks.size());
        }
        st.encode_width = 0;
        st.encode_height = 0;
        st.video_epoch_qpc_100ns.store(0);
        for (auto& epoch : st.audio_epoch_qpc_100ns) {
            epoch.store(0); // a previous session's measured audio zero point must not leak in
        }
        st.split_request_seq.store(0);
        st.split_last_trigger.store(static_cast<uint32_t>(SplitTriggerSource::ManualButton));
        // An "armed but unconsumed" size-split from a session that ended before
        // mux_thread's begin_new_segment reset it must not suppress the first
        // size-based split of the next recording.
        st.size_split_armed.store(false);
        {
            std::lock_guard slk(st.snapshot_callback_mutex);
            st.snapshot_requested.store(false);
            st.snapshot_callback = nullptr;
        }
        {
            LARGE_INTEGER qpc, freq;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&qpc);
            // Overflow-safe conversion: divide-first to avoid qpc*10e6 wrapping uint64_t
            // (wraps at ~51 h on a 10 MHz QPC counter)
            const auto q = static_cast<uint64_t>(qpc.QuadPart);
            const auto f = static_cast<uint64_t>(freq.QuadPart);
            st.session_start_qpc_100ns = (q / f) * 10000000ULL + (q % f) * 10000000ULL / f;
        }
        st.stats_callback = m_impl->stats_callback;
        st.meter_callback = m_impl->meter_callback;
        st.diagnostics_callback = m_impl->diagnostics_callback;
        st.segment_callback = m_impl->segment_callback;
        // Bridge the public uintptr_t handle callback to the internal HANDLE-typed one.
        if (m_impl->preview_shared_handle_callback) {
            auto pub_cb = m_impl->preview_shared_handle_callback;
            st.preview_shared_handle_cb = [pub_cb](HANDLE h, uint32_t w, uint32_t ht, PreviewTapDesc tap) {
                pub_cb(reinterpret_cast<uintptr_t>(h), w, ht, tap);
            };
        } else {
            st.preview_shared_handle_cb = nullptr;
        }
        st.preview_frame_published_cb = m_impl->preview_frame_published_callback;
        st.diagnostics.Reset(++m_impl->diagnostics_generation, MakeDiagnosticsStaticConfig(engine_config));
    }

    auto failPrepare = [&](int32_t hr, const std::string& detail) -> RecorderResult {
        RecorderResult r;
        r.succeeded = false;
        r.error_code = hr;
        r.error_phase = ErrorPhase::Prepare;
        r.error_detail = detail;
        m_impl->recording.store(false);
        return r;
    };

    auto createAudioSource = [&](AudioSourceKind kind) -> std::unique_ptr<IAudioCaptureSource> {
        switch (kind) {
        case AudioSourceKind::Mic: {
            std::unique_ptr<IAudioCaptureSource> mic =
                std::make_unique<WasapiCaptureSrc>(config.mic_channel_mode, config.mic_device_id);
            // Mic-DSP chain (Audio v2 — 0.6.0): the high-pass filter is the first
            // stage, the noise gate the second, the AGC the third, and RNNoise
            // neural noise suppression the fourth. Only wrap when a stage is
            // enabled so unaltered capture stays untouched.
            MicDspConfig dsp;
            dsp.hpf_enabled = config.mic_hpf_enabled;
            dsp.hpf_cutoff_hz = config.mic_hpf_cutoff_hz;
            dsp.gate_enabled = config.mic_gate_enabled;
            dsp.gate_threshold_db = config.mic_gate_threshold_db;
            dsp.agc_enabled = config.mic_agc_enabled;
            dsp.agc_target_db = config.mic_agc_target_db;
            dsp.rnnoise_enabled = config.mic_rnnoise_enabled;
            if (dsp.AnyEnabled()) {
                mic = std::make_unique<MicDspAudioSrc>(std::move(mic), dsp);
            }
            return mic;
        }
        case AudioSourceKind::App:
            return std::make_unique<WasapiProcessLoopbackSrc>(
                static_cast<DWORD>(config.audio_target_process_id.value_or(0u)),
                ProcessLoopbackMode::IncludeProcessTree);
        case AudioSourceKind::Sys:
            return std::make_unique<WasapiProcessLoopbackSrc>(
                static_cast<DWORD>(config.audio_target_process_id.value_or(0u)),
                ProcessLoopbackMode::ExcludeProcessTree);
        case AudioSourceKind::SystemOutput:
            return std::make_unique<WasapiLoopbackSrc>();
        default:
            return nullptr;
        }
    };

    std::vector<std::shared_ptr<AudioThread>> audioWorkers;
    if (!config.record_audio) {
        // Video-only path: no audio workers.
    } else if (config.audio_track_plan.tracks.empty()) {
        auto source = std::make_unique<WasapiLoopbackSrc>();
        audioWorkers.push_back(std::make_shared<AudioThread>(
            state_ptr, std::move(source), 0, std::vector<AudioSourceKind>{AudioSourceKind::SystemOutput}));
    } else {
        audioWorkers.reserve(config.audio_track_plan.tracks.size());
        for (const auto& track : config.audio_track_plan.tracks) {
            std::unique_ptr<IAudioCaptureSource> track_source;

            if (track.sources.size() == 1) {
                const AudioSourceKind kind = track.sources[0];
                if ((kind == AudioSourceKind::App || kind == AudioSourceKind::Sys) &&
                    (!config.audio_target_process_id.has_value() || config.audio_target_process_id.value() == 0)) {
                    return failPrepare(E_INVALIDARG,
                                       "audio_target_process_id must be a non-zero PID for App/Sys audio sources");
                }
                auto single_src = createAudioSource(kind);
                if (!single_src) {
                    return failPrepare(E_NOTIMPL, "Unsupported audio source kind for Phase 5A");
                }

                // Compose per-row gain with mic_gain_linear (for Mic sources).
                // source_gain_linear[0] carries GainDbToLinear(row.gain_db, row.muted).
                const float row_gain = track.source_gain_linear.empty() ? 1.0f : track.source_gain_linear[0];
                const float effective_gain =
                    (kind == AudioSourceKind::Mic) ? (config.mic_gain_linear * row_gain) : row_gain;

                if (effective_gain != 1.0f) {
                    std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
                    std::vector<float> gains;
                    inner.push_back(std::move(single_src));
                    gains.push_back(effective_gain);
                    track_source = std::make_unique<MixedAudioSrc>(
                        std::move(inner), std::move(gains), config.audio_limiter_enabled,
                        LimiterCeilingDbToLinear(config.audio_limiter_ceiling_db));
                } else {
                    track_source = std::move(single_src);
                }
            } else {
                // Multi-source track: build inner sources and wrap in MixedAudioSrc.
                // Per-source gain is the product of the row's gain_db/muted value
                // and mic_gain_linear (for Mic sources).
                std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
                std::vector<float> gains;
                inner.reserve(track.sources.size());
                gains.reserve(track.sources.size());
                for (std::size_t si = 0; si < track.sources.size(); ++si) {
                    const AudioSourceKind kind = track.sources[si];
                    if ((kind == AudioSourceKind::App || kind == AudioSourceKind::Sys) &&
                        (!config.audio_target_process_id.has_value() || config.audio_target_process_id.value() == 0)) {
                        return failPrepare(E_INVALIDARG,
                                           "audio_target_process_id must be a non-zero PID for App/Sys audio sources");
                    }
                    auto inner_src = createAudioSource(kind);
                    if (!inner_src) {
                        return failPrepare(E_NOTIMPL, "Unsupported audio source kind in merged track");
                    }
                    inner.push_back(std::move(inner_src));
                    const float row_gain = si < track.source_gain_linear.size() ? track.source_gain_linear[si] : 1.0f;
                    const float effective_gain =
                        (kind == AudioSourceKind::Mic) ? (config.mic_gain_linear * row_gain) : row_gain;
                    gains.push_back(effective_gain);
                }
                track_source =
                    std::make_unique<MixedAudioSrc>(std::move(inner), std::move(gains), config.audio_limiter_enabled,
                                                    LimiterCeilingDbToLinear(config.audio_limiter_ceiling_db));
            }

            audioWorkers.push_back(
                std::make_shared<AudioThread>(state_ptr, std::move(track_source), track.track_index, track.sources));
        }
    }

    m_impl->recording.store(true);

    // Start stats collector (Stop() joins its thread before Record returns, so
    // a plain reference into the shared state is safe here).
    const auto recording_wall_start = std::chrono::steady_clock::now();
    SessionStatsCollector statsCollector(*state_ptr);
    statsCollector.Start();

    // Start worker threads — all containers (including MP4) use MuxThread/Matroska.
    // For MP4: the engine records to a transient MKV; the app layer remuxes after stop.
    // Workers are shared_ptr-owned: each Start() hands the running thread shared
    // ownership of its worker (and through it the SessionState), so abandoning a
    // stalled worker below leaks the thread but never dangles what it touches.
    auto videoThread = std::make_shared<VideoThread>(state_ptr);
    auto muxThread = std::make_shared<MuxThread>(state_ptr);

    for (auto& worker : audioWorkers) {
        worker->Start();
    }
    videoThread->Start();
    muxThread->Start();

    // Two-phase cooperative shutdown (see worker_join.h):
    //   Phase 1 — wait (unbounded) until Stop() or a fatal worker failure sets
    //             stop_requested. Workers keep running until then, so this must
    //             NOT consume the join budget — otherwise recordings longer than
    //             the budget would fail with ERROR_TIMEOUT and an unfinalized file.
    //   Phase 2 — once stopping, all workers share a single 120 s join budget to
    //             drain their queues and finalize the container.
    //
    // IMPORTANT: NativeHandle() must be called BEFORE any Join() because the
    // handle becomes invalid once the std::thread is joined.
    std::vector<HANDLE> allHandles;
    allHandles.reserve(audioWorkers.size() + 2);
    allHandles.push_back(videoThread->NativeHandle());
    for (auto& worker : audioWorkers) {
        allHandles.push_back(worker->NativeHandle());
    }
    allHandles.push_back(muxThread->NativeHandle());

    bool hasNullHandle = false;
    for (HANDLE h : allHandles) {
        if (h == nullptr) {
            hasNullHandle = true;
            break;
        }
    }

    // Two-phase shutdown (see finalize_join_policy.h). The single fixed 120 s join
    // budget conflated two very different waits: the producer workers (video/audio)
    // only have to flush their encoders and exit, while the mux thread's finalize
    // (drain window -> clusters, Cues -> Render, back-patch Duration/SeekHead/
    // Segment size, close) is O(duration) and disk-bound. On a long recording
    // finalising to a NAS, finalize can legitimately run past 120 s while writing
    // bytes the whole time, yet was reported as a hung worker (m=TIMEOUT).
    //   Phase 1 — producers under a SHORT budget: a producer that will not flush
    //             and exit in this window is the real "worker hangs" fault.
    //   Phase 2 — the mux/finalize handle under a PROGRESS-based wait: keep waiting
    //             as long as bytes are still being committed, abort only on a real
    //             stall (no byte progress for a whole stall window) or a hard cap.
    constexpr DWORD kProducerJoinBudgetMs = 10000; // Phase 1: flush + exit
    constexpr DWORD kStopPollIntervalMs = 100;
    constexpr DWORD kFinalizePollIntervalMs = 250;     // Phase 2: sampling cadence
    constexpr uint64_t kFinalizeStallWindowMs = 30000; // abort after 30 s of zero disk progress
    constexpr uint64_t kFinalizeHardCapMs = 0;         // 0 = purely progress-based (no fixed cap)

    // allHandles is [video, audio..., mux]; split the mux (finalize) handle off.
    const size_t muxIndex = allHandles.size() - 1;
    std::vector<HANDLE> producerHandles(allHandles.begin(), allHandles.begin() + muxIndex);
    const HANDLE muxHandle = allHandles[muxIndex];

    bool videoJoined = false;
    std::vector<bool> audioJoined(audioWorkers.size(), false);
    bool muxJoined = false;
    bool finalizeStalled = false;

    if (!hasNullHandle) {
        // Shutdown-timing observability (added during the 2026-07-15 "fast
        // start/stop leaves the FinalizingOverlay spinning" investigation; kept
        // permanently — low-volume, shutdown-only, useful for any future
        // finalize-timing report). ---
        const auto shutdown_wait_start = std::chrono::steady_clock::now();
        auto elapsedMsSince = [](std::chrono::steady_clock::time_point start) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                    .count());
        };

        // --- Phase 1: producer workers under the short budget. Phase 1 of the wait
        // is still unbounded until stop_requested, so recording duration is never
        // capped; only the post-stop drain is bounded (kProducerJoinBudgetMs). ---
        const WorkerJoinResult jr = WaitForWorkersThenJoin(producerHandles, state_ptr->stop_requested,
                                                           kProducerJoinBudgetMs, kStopPollIntervalMs);
        if (jr.wait_failed) {
            state_ptr->RecordFailure(HRESULT_FROM_WIN32(jr.last_error), ErrorPhase::Shutdown,
                                     "WaitForMultipleObjects failed during producer join");
        }
        // Join the std::thread wrappers for producers that actually exited so their
        // destructors don't detach a still-running thread (order matches producerHandles:
        // video at 0, audio at 1..n). Timed-out workers stay unsignaled and are detached.
        if (!jr.signaled.empty() && jr.signaled[0]) {
            videoJoined = videoThread->Join(0);
        }
        for (size_t i = 0; i < audioWorkers.size(); ++i) {
            if (jr.signaled[1 + i]) {
                audioJoined[i] = audioWorkers[i]->Join(0);
            }
        }

        {
            const uint64_t phase1_ms = elapsedMsSince(shutdown_wait_start);
            const logging::LogField fields[] = {
                {"phase1_elapsed_ms", std::to_string(phase1_ms)},
                {"video_joined", videoJoined ? "true" : "false"},
                {"audio_all_joined",
                 std::all_of(audioJoined.begin(), audioJoined.end(), [](bool b) { return b; }) ? "true" : "false"},
                {"wait_failed", jr.wait_failed ? "true" : "false"},
            };
            logging::log(logging::LogLevel::Info, "recorder_session", "shutdown: phase1 (producer join) complete",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        }

        // --- Phase 2: mux/finalize under a progress-based wait. Sample the bytes
        // the Matroska writer has committed (published from inside Finalize()) and
        // keep waiting while they grow; abort only on a genuine stall. ---
        FinalizeProgressTracker finalizeTracker(kFinalizeStallWindowMs, kFinalizeHardCapMs);
        const auto phase2_start = std::chrono::steady_clock::now();
        uint64_t lastLoggedElapsedMs = 0;
        {
            const uint64_t bytes0 = state_ptr->mux_bytes_written.load(std::memory_order_relaxed);
            const logging::LogField fields[] = {{"baseline_bytes", std::to_string(bytes0)}};
            logging::log(logging::LogLevel::Info, "recorder_session", "shutdown: phase2 (finalize) begin",
                         std::span<const logging::LogField>(fields, std::size(fields)));
        }
        for (;;) {
            const DWORD w = WaitForSingleObject(muxHandle, kFinalizePollIntervalMs);
            if (w == WAIT_OBJECT_0) {
                muxJoined = muxThread->Join(0);
                const uint64_t elapsed_ms = elapsedMsSince(phase2_start);
                const logging::LogField fields[] = {
                    {"phase2_elapsed_ms", std::to_string(elapsed_ms)},
                    {"final_bytes", std::to_string(finalizeTracker.last_bytes())},
                };
                logging::log(logging::LogLevel::Info, "recorder_session", "shutdown: phase2 (finalize) joined",
                             std::span<const logging::LogField>(fields, std::size(fields)));
                break;
            }
            if (w == WAIT_FAILED) {
                state_ptr->RecordFailure(HRESULT_FROM_WIN32(GetLastError()), ErrorPhase::Shutdown,
                                         "WaitForSingleObject failed during finalize wait");
                break;
            }
            // WAIT_TIMEOUT: sample finalize byte progress and decide whether to keep waiting.
            const uint64_t bytes = state_ptr->mux_bytes_written.load(std::memory_order_relaxed);
            const uint64_t elapsed_ms = elapsedMsSince(phase2_start);
            if (elapsed_ms - lastLoggedElapsedMs >= 1000) {
                lastLoggedElapsedMs = elapsed_ms;
                const logging::LogField fields[] = {
                    {"phase2_elapsed_ms", std::to_string(elapsed_ms)},
                    {"bytes", std::to_string(bytes)},
                    {"last_progress_ms", std::to_string(finalizeTracker.last_progress_ms())},
                };
                logging::log(logging::LogLevel::Info, "recorder_session", "shutdown: phase2 (finalize) waiting",
                             std::span<const logging::LogField>(fields, std::size(fields)));
            }
            if (finalizeTracker.Observe(bytes, elapsed_ms) == FinalizeWaitDecision::StalledAbort) {
                finalizeStalled = true;
                const logging::LogField fields[] = {
                    {"phase2_elapsed_ms", std::to_string(elapsed_ms)},
                    {"bytes", std::to_string(bytes)},
                };
                logging::log(logging::LogLevel::Warn, "recorder_session", "shutdown: phase2 (finalize) stalled-abort",
                             std::span<const logging::LogField>(fields, std::size(fields)));
                break;
            }
        }
    } else {
        state_ptr->RecordFailure(E_FAIL, ErrorPhase::Shutdown, "Thread native handle is null before join");
    }

    // Stop stats collector
    statsCollector.Stop();

    m_impl->recording.store(false);

    // Any worker that missed its join is now abandoned — but it stays alive
    // through its own shared ownership (worker thread -> worker -> state).
    // Mark the leak so the next Record() starts on a fresh SessionState
    // instead of racing the leaked worker on this one.
    {
        bool anyLeaked = !videoJoined || !muxJoined;
        for (const bool joined : audioJoined) {
            if (!joined) {
                anyLeaked = true;
            }
        }
        if (anyLeaked) {
            std::lock_guard lk(m_impl->state_mutex);
            m_impl->workers_leaked = true;
        }
    }

    // Build result
    RecorderResult result;
    {
        auto& st = *state_ptr;

        std::lock_guard flk(st.failure_mutex);
        if (st.failure_recorded) {
            result.succeeded = false;
            result.error_code = st.failure.error_code;
            result.error_phase = st.failure.error_phase;
            result.error_detail = st.failure.error_detail;
        } else {
            result.succeeded = true;
            result.error_code = S_OK;
            result.error_phase = ErrorPhase::None;
        }

        // Report the two shutdown faults distinctly (finalize_join_policy.h): a
        // producer that failed to flush and join in Phase 1 (WorkerHang) is a very
        // different problem from a finalize that stalled in Phase 2 (FinalizeStalled),
        // and they must not be conflated under one "join timeout" message.
        bool allAudioJoined = true;
        for (bool joined : audioJoined) {
            if (!joined) {
                allAudioJoined = false;
                break;
            }
        }
        const bool producersJoined = videoJoined && allAudioJoined;
        const ShutdownFault fault = ClassifyShutdownFault(producersJoined, muxJoined);

        if (fault != ShutdownFault::None) {
            if (result.error_detail.empty()) {
                result.succeeded = false;
                result.error_code = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                result.error_phase = ErrorPhase::Shutdown;
            }
            if (fault == ShutdownFault::WorkerHang) {
                result.error_detail += " [worker hang: v=" + std::string(videoJoined ? "ok" : "TIMEOUT");
                for (size_t i = 0; i < audioJoined.size(); ++i) {
                    result.error_detail +=
                        " a" + std::to_string(i) + "=" + std::string(audioJoined[i] ? "ok" : "TIMEOUT");
                }
                result.error_detail +=
                    " m=" + std::string(muxJoined ? "ok" : (finalizeStalled ? "STALLED" : "TIMEOUT")) + "]";
            } else { // FinalizeStalled: producers were clean, only finalize failed to complete.
                result.error_detail += " [finalize " + std::string(finalizeStalled ? "stalled" : "incomplete") +
                                       ": no byte progress for " + std::to_string(kFinalizeStallWindowMs) + " ms]";
            }
        }
    }

    // Cancel any pending frame snapshot request — VideoThread has stopped so the
    // callback will never fire from the capture loop.  Fire it with an error here.
    {
        FrameSnapshotCallback pending_cb;
        {
            std::lock_guard slk(state_ptr->snapshot_callback_mutex);
            if (state_ptr->snapshot_requested.load()) {
                pending_cb = std::move(state_ptr->snapshot_callback);
                state_ptr->snapshot_callback = nullptr;
                state_ptr->snapshot_requested.store(false);
            }
        }
        if (pending_cb)
            pending_cb(false, 0, 0, {}, "recording session ended before snapshot completed");
    }

    // Final stats snapshot
    {
        std::lock_guard slk(state_ptr->stats_mutex);
        result.stats = state_ptr->stats;
    }

    // Fill in elapsed_seconds from wall-clock time (stats_callback snapshots compute this per-tick
    // but never write it back to m_state.stats, so the final stats always shows 0 without this).
    //
    // Minus everything the session spent paused, which the stats collector
    // accumulated as it watched `pause_requested`. A paused capture writes no
    // frames, so counting that time here would report a duration the file does
    // not have -- and would disagree with the clock the user was watching.
    {
        const auto recording_wall_end = std::chrono::steady_clock::now();
        const auto captured =
            recording_wall_end - recording_wall_start - std::chrono::nanoseconds(state_ptr->paused_ns.load());
        const double seconds = std::chrono::duration<double>(captured).count();
        result.stats.elapsed_seconds = seconds > 0.0 ? seconds : 0.0;
    }

    // Defensive: if nominally succeeded but no output file was produced
    if (result.succeeded && result.stats.output_file_bytes == 0) {
        result.succeeded = false;
        result.error_code = E_FAIL;
        result.error_phase = ErrorPhase::Mux;
        if (result.error_detail.empty()) {
            result.error_detail = "Mux completed without writing output file";
        }
    }

    // Compute skew in result stats
    if (result.stats.video_duration_ns > 0 && result.stats.audio_duration_ns > 0) {
        double vd = static_cast<double>(result.stats.video_duration_ns) / 1e6;
        double ad = static_cast<double>(result.stats.audio_duration_ns) / 1e6;
        result.stats.duration_skew_ms = (vd > ad) ? (vd - ad) : (ad - vd);
    }

    // Final frozen diagnostics snapshot (Completed/Failed). The stats collector is
    // already stopped, so this is the single source of the terminal snapshot and no
    // further periodic updates follow. Diagnostics failure must never fail recording.
    if (m_impl->diagnostics_callback) {
        const auto diag_now = std::chrono::steady_clock::now();
        const DiagnosticsLifecycle lifecycle =
            result.succeeded ? DiagnosticsLifecycle::Completed : DiagnosticsLifecycle::Failed;
        const RecordingDiagnosticsSnapshot snapshot =
            state_ptr->diagnostics.BuildSnapshot(diag_now, result.stats, lifecycle, result.stats.elapsed_seconds);
        m_impl->diagnostics_callback(snapshot);
    }

    // Discard any Stop() that arrived after m_impl->recording dropped above (e.g.
    // an extra click during finalize) — it targets this already-finishing session,
    // not the next Record() call.
    {
        std::lock_guard lk(m_impl->state_mutex);
        (void)m_impl->pending_stop.Consume();
    }

    return result;
}

} // namespace exosnap::engine
