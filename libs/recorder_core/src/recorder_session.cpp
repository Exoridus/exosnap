#include <recorder_core/recorder_session.h>

#include "audio_thread.h"
#include "brickwall_limiter.h"
#include "mic_dsp_audio_src.h"
#include "mixed_audio_src.h"
#include "mux_thread.h"
#include "session_internal.h"
#include "session_stats_collector.h"
#include "video_thread.h"
#include "wasapi_capture_src.h"
#include "wasapi_loopback_src.h"
#include "wasapi_process_loopback_src.h"
#include "wgc_capture.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace recorder_core {

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

std::filesystem::path DeriveSegmentPath(const std::filesystem::path& base, std::uint32_t index) {
    // Segment 0 keeps the base path verbatim (no rename of the first file).
    if (index == 0) {
        return base;
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

    std::filesystem::path candidate = compose(0);
    // Collision-safe: append "_N" until a free path is found. Bounded scan.
    for (unsigned d = 1; d < 10000u; ++d) {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
        candidate = compose(d);
    }
    return candidate;
}

// ---------------------------------------------------------------------------
// RecorderSession::Impl
// ---------------------------------------------------------------------------

struct RecorderSession::Impl {
    SessionState state;
    StatsCallback stats_callback;
    MeterCallback meter_callback;
    DiagnosticsCallback diagnostics_callback;
    uint64_t diagnostics_generation{0};
    std::atomic<bool> recording{false};
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
        if (config.video_codec != VideoCodec::H264Nvenc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare, "Container::Mp4 requires VideoCodec::H264Nvenc");
        }
    } else if (config.container == Container::WebM) {
        if (config.video_codec != VideoCodec::Av1Nvenc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare, "Container::WebM requires VideoCodec::Av1Nvenc");
        }
    } else {
        // Container::Matroska: AV1 and H.264 are supported
        if (config.video_codec != VideoCodec::Av1Nvenc && config.video_codec != VideoCodec::H264Nvenc) {
            return fail(E_NOTIMPL, ErrorPhase::Prepare,
                        "Container::Matroska requires VideoCodec::Av1Nvenc or VideoCodec::H264Nvenc");
        }
    }

    // Audio codec
    if (config.container == Container::Mp4) {
        if (config.audio_codec != AudioCodec::AacMf) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "Container::Mp4 requires AudioCodec::AacMf; Opus and PCM are not valid for MP4");
        }
    } else if (config.audio_codec == AudioCodec::Opus) {
        // Opus: valid for WebM and Matroska
    } else if (config.audio_codec == AudioCodec::AacMf) {
        if (config.container == Container::WebM) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::AacMf is not valid for Container::WebM; use AudioCodec::Opus");
        }
    } else if (config.audio_codec == AudioCodec::Pcm) {
        // PCM (A_PCM/INT_LIT): Matroska only.
        if (config.container != Container::Matroska) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::Pcm is only valid for Container::Matroska (MKV); "
                        "WebM and MP4 cannot carry A_PCM/INT_LIT in this build");
        }
    } else {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "Unsupported audio codec; supported: AudioCodec::Opus, AudioCodec::AacMf, AudioCodec::Pcm");
    }

    // Chroma: only Cs420 supported
    if (config.chroma != ChromaSubsampling::Cs420) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only ChromaSubsampling::Cs420 is supported in M3.1");
    }

    // Bit depth: only Bit8 supported
    if (config.bit_depth != BitDepth::Bit8) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only BitDepth::Bit8 is supported in M3.1");
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
    m_impl->state.pause_requested.store(false);
    m_impl->state.stop_requested.store(true);
    m_impl->state.premux_cv.notify_all();
    m_impl->state.mux_cv.notify_all();
}

void RecorderSession::Pause() {
    m_impl->state.pause_requested.store(true);
}

void RecorderSession::Resume() {
    m_impl->state.pause_requested.store(false);
}

void RecorderSession::RequestSplit(SplitTriggerSource source) {
    if (!m_impl->recording.load())
        return;
    // Record the trigger (for logging) before bumping the sequence so the
    // observing thread sees a consistent (seq, trigger) pair.
    m_impl->state.split_last_trigger.store(static_cast<uint32_t>(source));
    m_impl->state.split_request_seq.fetch_add(1);
}

void RecorderSession::SetSegmentCallback(SegmentCallback cb) {
    m_impl->state.segment_callback = std::move(cb);
}

void RecorderSession::RequestFrameSnapshot(FrameSnapshotCallback callback) {
    if (!m_impl->recording.load())
        return;
    std::lock_guard lk(m_impl->state.snapshot_callback_mutex);
    if (m_impl->state.snapshot_requested.load())
        return; // already pending — ignore
    m_impl->state.snapshot_callback = std::move(callback);
    m_impl->state.snapshot_requested.store(true);
}

void RecorderSession::UpdateWebcamOverlay(const WebcamOverlayLive& overlay) {
    if (!m_impl->recording.load()) {
        return;
    }
    if (m_impl->state.config.webcam.frame_provider == nullptr) {
        return;
    }
    m_impl->state.UpdateWebcamOverlay(overlay);
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

    // Reset session state
    {
        auto& st = m_impl->state;
        st.stop_requested.store(false);
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
        st.split_request_seq.store(0);
        st.split_last_trigger.store(static_cast<uint32_t>(SplitTriggerSource::ManualButton));
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
            // stage, the noise gate the second, the AGC the third; the remaining
            // slice (RNNoise) extends MicDspConfig. Only wrap when a stage is
            // enabled so unaltered capture stays untouched.
            MicDspConfig dsp;
            dsp.hpf_enabled = config.mic_hpf_enabled;
            dsp.hpf_cutoff_hz = config.mic_hpf_cutoff_hz;
            dsp.gate_enabled = config.mic_gate_enabled;
            dsp.gate_threshold_db = config.mic_gate_threshold_db;
            dsp.agc_enabled = config.mic_agc_enabled;
            dsp.agc_target_db = config.mic_agc_target_db;
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

    std::vector<std::unique_ptr<AudioThread>> audioWorkers;
    if (!config.record_audio) {
        // Video-only path: no audio workers.
    } else if (config.audio_track_plan.tracks.empty()) {
        auto source = std::make_unique<WasapiLoopbackSrc>();
        audioWorkers.push_back(std::make_unique<AudioThread>(m_impl->state, std::move(source), 0));
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
                std::make_unique<AudioThread>(m_impl->state, std::move(track_source), track.track_index));
        }
    }

    m_impl->recording.store(true);

    // Start stats collector
    const auto recording_wall_start = std::chrono::steady_clock::now();
    SessionStatsCollector statsCollector(m_impl->state);
    statsCollector.Start();

    // Start worker threads — all containers (including MP4) use MuxThread/Matroska.
    // For MP4: the engine records to a transient MKV; the app layer remuxes after stop.
    VideoThread videoThread(m_impl->state);
    MuxThread muxThread(m_impl->state);

    for (auto& worker : audioWorkers) {
        worker->Start();
    }
    videoThread.Start();
    muxThread.Start();

    // Parallel join: all workers share a single 120-second budget.
    //
    // IMPORTANT: NativeHandle() must be called BEFORE any Join() because the
    // handle becomes invalid once the std::thread is joined.
    std::vector<HANDLE> allHandles;
    allHandles.reserve(audioWorkers.size() + 2);
    allHandles.push_back(videoThread.NativeHandle());
    for (auto& worker : audioWorkers) {
        allHandles.push_back(worker->NativeHandle());
    }
    allHandles.push_back(muxThread.NativeHandle());

    bool hasNullHandle = false;
    for (HANDLE h : allHandles) {
        if (h == nullptr) {
            hasNullHandle = true;
            break;
        }
    }

    constexpr DWORD kParallelJoinTimeoutMs = 120000;
    DWORD joinWait = WAIT_FAILED;
    if (!hasNullHandle) {
        joinWait = WaitForMultipleObjects(static_cast<DWORD>(allHandles.size()), allHandles.data(), TRUE,
                                          kParallelJoinTimeoutMs);
        if (joinWait == WAIT_FAILED) {
            m_impl->state.RecordFailure(HRESULT_FROM_WIN32(GetLastError()), ErrorPhase::Shutdown,
                                        "WaitForMultipleObjects failed during thread join");
        }
    } else {
        m_impl->state.RecordFailure(E_FAIL, ErrorPhase::Shutdown, "Thread native handle is null before join");
    }

    bool videoJoined = false;
    std::vector<bool> audioJoined(audioWorkers.size(), false);
    bool muxJoined = false;

    if (joinWait == WAIT_OBJECT_0) {
        videoJoined = videoThread.Join(0);
        for (size_t i = 0; i < audioWorkers.size(); ++i) {
            audioJoined[i] = audioWorkers[i]->Join(0);
        }
        muxJoined = muxThread.Join(0);
    } else {
        videoJoined = (allHandles[0] != nullptr && WaitForSingleObject(allHandles[0], 0) == WAIT_OBJECT_0);
        if (videoJoined) {
            videoThread.Join(0);
        }
        for (size_t i = 0; i < audioWorkers.size(); ++i) {
            const HANDLE h = allHandles[1 + i];
            audioJoined[i] = (h != nullptr && WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
            if (audioJoined[i]) {
                audioWorkers[i]->Join(0);
            }
        }
        const HANDLE muxHandle = allHandles[1 + audioWorkers.size()];
        muxJoined = (muxHandle != nullptr && WaitForSingleObject(muxHandle, 0) == WAIT_OBJECT_0);
        if (muxJoined) {
            muxThread.Join(0);
        }
    }

    // Stop stats collector
    statsCollector.Stop();

    m_impl->recording.store(false);

    // Build result
    RecorderResult result;
    {
        auto& st = m_impl->state;

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

        // Append join timeout info if any thread didn't join cleanly
        bool allAudioJoined = true;
        for (bool joined : audioJoined) {
            if (!joined) {
                allAudioJoined = false;
                break;
            }
        }

        if (!videoJoined || !allAudioJoined || !muxJoined) {
            if (result.error_detail.empty()) {
                result.succeeded = false;
                result.error_code = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                result.error_phase = ErrorPhase::Shutdown;
            }
            result.error_detail += " [join timeout: v=" + std::string(videoJoined ? "ok" : "TIMEOUT");
            for (size_t i = 0; i < audioJoined.size(); ++i) {
                result.error_detail += " a" + std::to_string(i) + "=" + std::string(audioJoined[i] ? "ok" : "TIMEOUT");
            }
            result.error_detail += " m=" + std::string(muxJoined ? "ok" : "TIMEOUT") + "]";
        }
    }

    // Cancel any pending frame snapshot request — VideoThread has stopped so the
    // callback will never fire from the capture loop.  Fire it with an error here.
    {
        FrameSnapshotCallback pending_cb;
        {
            std::lock_guard slk(m_impl->state.snapshot_callback_mutex);
            if (m_impl->state.snapshot_requested.load()) {
                pending_cb = std::move(m_impl->state.snapshot_callback);
                m_impl->state.snapshot_callback = nullptr;
                m_impl->state.snapshot_requested.store(false);
            }
        }
        if (pending_cb)
            pending_cb(false, 0, 0, {}, "recording session ended before snapshot completed");
    }

    // Final stats snapshot
    {
        std::lock_guard slk(m_impl->state.stats_mutex);
        result.stats = m_impl->state.stats;
    }

    // Fill in elapsed_seconds from wall-clock time (stats_callback snapshots compute this per-tick
    // but never write it back to m_state.stats, so the final stats always shows 0 without this).
    {
        const auto recording_wall_end = std::chrono::steady_clock::now();
        result.stats.elapsed_seconds = std::chrono::duration<double>(recording_wall_end - recording_wall_start).count();
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
            m_impl->state.diagnostics.BuildSnapshot(diag_now, result.stats, lifecycle, result.stats.elapsed_seconds);
        m_impl->diagnostics_callback(snapshot);
    }

    return result;
}

} // namespace recorder_core
