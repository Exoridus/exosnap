#include <recorder_core/recorder_session.h>

#include "audio_thread.h"
#include "mixed_audio_src.h"
#include "mux_thread.h"
#include "session_internal.h"
#include "session_stats_collector.h"
#include "video_thread.h"
#include "wasapi_capture_src.h"
#include "wasapi_loopback_src.h"
#include "wasapi_process_loopback_src.h"
#include "wgc_capture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace recorder_core {

// ---------------------------------------------------------------------------
// RecorderSession::Impl
// ---------------------------------------------------------------------------

struct RecorderSession::Impl {
    SessionState state;
    StatsCallback stats_callback;
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

    // Container: WebM and Matroska are supported
    if (config.container != Container::WebM && config.container != Container::Matroska) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only Container::WebM and Container::Matroska are supported");
    }

    // Video codec: only Av1Nvenc supported
    if (config.video_codec != VideoCodec::Av1Nvenc) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only VideoCodec::Av1Nvenc is supported");
    }

    // Audio codec: Opus is valid for WebM and Matroska; AAC is valid for Matroska only.
    if (config.audio_codec == AudioCodec::Opus) {
        // Opus: valid for both containers — no further check needed
    } else if (config.audio_codec == AudioCodec::AacMf) {
        if (config.container == Container::WebM) {
            return fail(E_INVALIDARG, ErrorPhase::Prepare,
                        "AudioCodec::AacMf is not valid for Container::WebM; use AudioCodec::Opus");
        }
    } else {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "Unsupported audio codec; supported: AudioCodec::Opus, AudioCodec::AacMf");
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
    m_impl->state.stop_requested.store(true);
    m_impl->state.premux_cv.notify_all();
    m_impl->state.mux_cv.notify_all();
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
        st.config = config;
        if (!config.record_audio) {
            st.audio_track_count = 0;
        } else {
            st.audio_track_count = config.audio_track_plan.tracks.empty()
                                       ? 1u
                                       : static_cast<uint32_t>(config.audio_track_plan.tracks.size());
        }
        st.encode_width = 0;
        st.encode_height = 0;
        st.stats_callback = m_impl->stats_callback;
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
        case AudioSourceKind::Mic:
            return std::make_unique<WasapiCaptureSrc>(config.mic_channel_mode, config.mic_device_id);
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

                if (kind == AudioSourceKind::Mic && config.mic_gain_linear != 1.0f) {
                    std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
                    std::vector<float> gains;
                    inner.push_back(std::move(single_src));
                    gains.push_back(config.mic_gain_linear);
                    track_source = std::make_unique<MixedAudioSrc>(std::move(inner), std::move(gains));
                } else {
                    track_source = std::move(single_src);
                }
            } else {
                // Multi-source track: build inner sources and wrap in MixedAudioSrc
                std::vector<std::unique_ptr<IAudioCaptureSource>> inner;
                std::vector<float> gains;
                inner.reserve(track.sources.size());
                gains.reserve(track.sources.size());
                for (const auto kind : track.sources) {
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
                    gains.push_back(kind == AudioSourceKind::Mic ? config.mic_gain_linear : 1.0f);
                }
                track_source = std::make_unique<MixedAudioSrc>(std::move(inner), std::move(gains));
            }

            audioWorkers.push_back(
                std::make_unique<AudioThread>(m_impl->state, std::move(track_source), track.track_index));
        }
    }

    m_impl->recording.store(true);

    // Start stats collector
    SessionStatsCollector statsCollector(m_impl->state);
    statsCollector.Start();

    // Start worker threads
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

    // Final stats snapshot
    {
        std::lock_guard slk(m_impl->state.stats_mutex);
        result.stats = m_impl->state.stats;
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

    return result;
}

} // namespace recorder_core
