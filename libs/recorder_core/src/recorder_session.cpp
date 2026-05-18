#include <recorder_core/recorder_session.h>

#include "audio_thread.h"
#include "mux_thread.h"
#include "session_internal.h"
#include "session_stats_collector.h"
#include "video_thread.h"
#include "wgc_capture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

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

    // Container: only Matroska supported
    if (config.container != Container::Matroska) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only Container::Matroska is supported in M3.1");
    }

    // Video codec: only Av1Nvenc supported
    if (config.video_codec != VideoCodec::Av1Nvenc) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only VideoCodec::Av1Nvenc is supported in M3.1");
    }

    // Audio codec: Opus is a future placeholder only — must fail here
    if (config.audio_codec == AudioCodec::Opus) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare,
                    "AudioCodec::Opus is not implemented in M3.1; use AudioCodec::AacMf");
    }
    if (config.audio_codec != AudioCodec::AacMf) {
        return fail(E_NOTIMPL, ErrorPhase::Prepare, "Only AudioCodec::AacMf is supported in M3.1");
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
        st.encode_width = 0;
        st.encode_height = 0;
        st.stats_callback = m_impl->stats_callback;
    }

    m_impl->recording.store(true);

    // Start stats collector
    SessionStatsCollector statsCollector(m_impl->state);
    statsCollector.Start();

    // Start worker threads
    VideoThread videoThread(m_impl->state);
    AudioThread audioThread(m_impl->state);
    MuxThread muxThread(m_impl->state);

    videoThread.Start();
    audioThread.Start();
    muxThread.Start();

    // Parallel join: all three threads share a single 120-second budget.
    // VideoThread's NVENC EOS drain can take 15-40+ seconds for a 30-second
    // recording at 60fps with ~1250 GPU-buffered frames.  Sequential 10s timeouts
    // cause a cascade where MuxThread never receives VideoEosSentinel in time.
    //
    // IMPORTANT: NativeHandle() must be called BEFORE any Join() because the
    // handle becomes invalid once the std::thread is joined.
    HANDLE joinHandles[3] = {
        videoThread.NativeHandle(),
        audioThread.NativeHandle(),
        muxThread.NativeHandle(),
    };
    constexpr DWORD kParallelJoinTimeoutMs = 120000;

    DWORD joinWait = WaitForMultipleObjects(3, joinHandles, TRUE, kParallelJoinTimeoutMs);

    bool videoJoined = false;
    bool audioJoined = false;
    bool muxJoined = false;

    if (joinWait == WAIT_OBJECT_0) {
        // All three signalled within the budget — join them cleanly (non-blocking).
        videoJoined = videoThread.Join(0);
        audioJoined = audioThread.Join(0);
        muxJoined = muxThread.Join(0);
    } else {
        // At least one thread timed out; collect whichever finished in time.
        videoJoined = (WaitForSingleObject(joinHandles[0], 0) == WAIT_OBJECT_0);
        if (videoJoined)
            videoThread.Join(0);
        audioJoined = (WaitForSingleObject(joinHandles[1], 0) == WAIT_OBJECT_0);
        if (audioJoined)
            audioThread.Join(0);
        muxJoined = (WaitForSingleObject(joinHandles[2], 0) == WAIT_OBJECT_0);
        if (muxJoined)
            muxThread.Join(0);
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
        if (!videoJoined || !audioJoined || !muxJoined) {
            if (result.error_detail.empty()) {
                result.succeeded = false;
                result.error_code = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                result.error_phase = ErrorPhase::Shutdown;
            }
            result.error_detail += " [join timeout: v=" + std::string(videoJoined ? "ok" : "TIMEOUT") +
                                   " a=" + std::string(audioJoined ? "ok" : "TIMEOUT") +
                                   " m=" + std::string(muxJoined ? "ok" : "TIMEOUT") + "]";
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
