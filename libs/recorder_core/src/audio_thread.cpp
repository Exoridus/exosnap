#include "audio_thread.h"

#include "audio_clock_drift.h"
#include "audio_device_loss_policy.h"
#include "clock_slaving.h"
#include "codec_private.h"
#include "ffmpeg_aac_encoder.h"
#include "flac_audio_encoder.h"
#include "opus_audio_encoder.h"
#include "output_format_audio_src.h"
#include "pcm_audio_encoder.h"
#include "recorder_core/audio_meter.h"
#include "session_internal.h"

#include <recorder_core/logging/logging.h>
#include <recorder_core/packet_types.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>

namespace recorder_core {

namespace {

constexpr float kRmsEmaAlpha = 0.3f;

// Wall-clock now in nanoseconds on the QPC timeline. Used to size the silence
// that fills an audio source's device-loss outage (ADR 0046): the degraded
// source delivers no packets and no device positions, so the gap is measured
// against the wall clock and fed to the encoder as whole silence frames, keeping
// PTS (derived from the accumulated frame counter) continuous across the outage.
uint64_t QpcNowNs() noexcept {
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    if (freq.QuadPart <= 0) {
        return 0;
    }
    const uint64_t c = static_cast<uint64_t>(counter.QuadPart);
    const uint64_t f = static_cast<uint64_t>(freq.QuadPart);
    return (c / f) * 1000000000ULL + ((c % f) * 1000000000ULL) / f;
}

void ConvertInt16ToFloat32(const std::int16_t* src, float* dst, size_t sample_count) {
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<float>(src[i]) / 32768.0f;
    }
}

// Per-codec wiring that differs between the otherwise identical encode loops:
// which encoder to build, how its init failure reads, and whether an empty
// codec-private after Init is a hard error (FLAC/AAC produce theirs during
// Init; PCM legitimately has none; Opus always builds a 19-byte OpusHead).
struct EncoderSetup {
    std::unique_ptr<IAudioEncoder> encoder;
    const char* init_error_prefix = nullptr;
    const char* empty_codec_private_error = nullptr; // nullptr == empty allowed
};

EncoderSetup MakeEncoderSetup(const RecorderConfig& config) {
    EncoderSetup setup;
    switch (config.audio_codec) {
    case AudioCodec::Opus: {
        auto enc = std::make_unique<OpusAudioEncoder>();
        enc->SetEncodingParams(config.audio_bitrate_kbps, config.opus_frame_duration, config.opus_complexity);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "Opus encoder init: ";
        break;
    }
    case AudioCodec::Pcm: {
        // PCM passthrough "encoder" (MKV-only A_PCM/INT/LIT); no CodecPrivate —
        // the track is marked ready with empty bytes so the mux thread's
        // codec-private readiness gate releases the pre-mux buffer.
        auto enc = std::make_unique<PcmAudioEncoder>();
        enc->SetBitDepth(config.audio_bit_depth);    // ADR 0030: configurable depth
        enc->SetFloatFormat(config.audio_pcm_float); // Float-PCM: A_PCM/FLOAT/IEEE
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "PCM encoder init: ";
        break;
    }
    case AudioCodec::Flac: {
        // FLAC lossless encoder (MKV-only A_FLAC via libFLAC). Its CodecPrivate
        // (native "fLaC" header + STREAMINFO) is produced during Init() via the
        // write callback and must be non-empty.
        auto enc = std::make_unique<FlacAudioEncoder>();
        enc->SetBitDepth(config.audio_bit_depth); // ADR 0030: configurable depth + level
        enc->SetCompressionLevel(config.flac_compression_level);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FLAC encoder init: ";
        setup.empty_codec_private_error = "FLAC codec private is empty after Init";
        break;
    }
    case AudioCodec::Aac: {
        // FFmpeg's native AAC-LC encoder (ADR 0052, cut over once
        // exosnap-ffmpeg-build r5 shipped an encoder-enabled avcodec DLL).
        auto enc = std::make_unique<FfmpegAacEncoder>();
        enc->SetBitrateKbps(config.audio_bitrate_kbps);
        setup.encoder = std::move(enc);
        setup.init_error_prefix = "FFmpeg AAC encoder init: ";
        setup.empty_codec_private_error = "FFmpeg AAC codec private is empty after Init";
        break;
    }
        // Deliberately no default: label (CV-BUG-004). This switch must stay
        // exhaustive over every named AudioCodec enumerator so /W4 (and
        // -Wswitch on Clang/GCC) flags a future enumerator added here without a
        // matching case, instead of silently routing it into whichever branch
        // happened to be last. A value outside the known enumerators (e.g. an
        // out-of-range int cast from a corrupted config) simply falls through
        // with setup.encoder left null; AudioThread::Run() below turns a null
        // encoder into a visible RecordFailure instead of building an AAC
        // encoder for an unrecognized codec.
    }
    return setup;
}

} // namespace

AudioThread::AudioThread(std::shared_ptr<SessionState> state, std::unique_ptr<IAudioCaptureSource> source,
                         uint32_t track_id)
    : m_state_ptr(std::move(state)), m_state(*m_state_ptr), source_(std::move(source)), track_id_(track_id) {
}

AudioThread::~AudioThread() {
    // Start() gave the running thread shared ownership of this object, so a
    // joinable thread here has already returned from Run() (the final release
    // may even happen on the worker thread itself, where join() would
    // deadlock). Detaching a finished thread only releases its handle.
    if (m_thread.joinable())
        m_thread.detach();
}

void AudioThread::Start() {
    // Self-ownership handoff: the lambda keeps this worker (and through
    // m_state_ptr the SessionState) alive until Run() returns, so dropping the
    // session's handle on a stalled worker can never dangle the state the
    // thread still writes through.
    m_thread = std::thread([self = shared_from_this()] { self->Run(); });
}

bool AudioThread::Join(unsigned timeout_ms) {
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

void AudioThread::Run() {
    // COM init (apartment-threaded for Media Foundation)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool com_inited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!com_inited) {
        char buf[80];
        snprintf(buf, sizeof(buf), "AudioThread: CoInitializeEx failed 0x%08lX", static_cast<unsigned long>(hr));
        m_state.RecordFailure(hr, ErrorPhase::Prepare, buf);
        return;
    }
    auto uninitCom = [&]() {
        if (com_inited && hr != RPC_E_CHANGED_MODE)
            CoUninitialize();
    };

    if (!source_) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio source is null");
        uninitCom();
        return;
    }

    if (track_id_ >= CodecPrivateData::kMaxAudioTracks) {
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::Prepare, "Audio track id is out of range");
        uninitCom();
        return;
    }

    // --- Wrap source in OutputFormatAudioSrc (ADR 0030) ---
    // Effective sample rate: Opus is locked to 48 kHz; all other codecs use
    // the configured audio_sample_rate. Channel count and bit depth are always
    // configurable. When target == 48000/stereo (the default), the decorator is
    // a byte-identical passthrough — no SwrContext is created.
    {
        const uint32_t effective_rate =
            (m_state.config.audio_codec == AudioCodec::Opus) ? 48000u : m_state.config.audio_sample_rate;
        const uint32_t effective_channels = m_state.config.audio_channels;

        // Wrap source_ so OutputFormatAudioSrc::Init calls the real source's Init
        // and configures swresample if needed. After this, source_ reports the
        // target sample_rate/channels. Keep a typed view so the clock-slaving
        // controller can drive compensation on it.
        auto wrapper = std::make_unique<OutputFormatAudioSrc>(std::move(source_), effective_rate, effective_channels);
        output_format_src_ = wrapper.get();
        source_ = std::move(wrapper);
    }

    {
        std::string err;
        if (!source_->Init(err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source init failed: " + err);
            source_->Shutdown();
            uninitCom();
            return;
        }
    }

    const uint32_t kSampleRate = source_->SampleRate();
    const uint32_t kChannels = source_->Channels();
    const AudioSampleFormat sourceFormat = source_->SampleFormat();

    if (kSampleRate == 0 || kChannels == 0) {
        m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture, "Audio source reported invalid stream format");
        source_->Shutdown();
        uninitCom();
        return;
    }

    m_state.diagnostics.SetAudioFormat(kSampleRate, kChannels);

    // --- Encoder init (the only codec-specific part of this worker) ---
    EncoderSetup setup = MakeEncoderSetup(m_state.config);
    if (!setup.encoder) {
        // config.audio_codec held a value outside the known AudioCodec
        // enumerators (CV-BUG-004) — surface it the same way every other
        // pre-encode failure in this function is surfaced, rather than
        // dereferencing a null encoder or silently falling back to AAC.
        m_state.RecordFailure(E_INVALIDARG, ErrorPhase::AudioEncode, "Unrecognized audio codec");
        source_->Shutdown();
        uninitCom();
        return;
    }
    IAudioEncoder& encoder = *setup.encoder;
    {
        std::string err;
        if (!encoder.Init(kSampleRate, kChannels, err)) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, std::string(setup.init_error_prefix) + err);
            source_->Shutdown();
            uninitCom();
            return;
        }
    }

    // --- Publish codec private and mark the track ready so the mux thread's
    // codec-private readiness gate can release the pre-mux buffer. ---
    {
        auto cp = encoder.CodecPrivateBytes();
        if (setup.empty_codec_private_error != nullptr && cp.empty()) {
            m_state.RecordFailure(E_FAIL, ErrorPhase::AudioEncode, setup.empty_codec_private_error);
            encoder.Shutdown();
            source_->Shutdown();
            uninitCom();
            return;
        }
        std::lock_guard lk(m_state.premux_mutex);
        m_state.codec_private.audio_codec_private[track_id_].bytes = std::move(cp);
        m_state.codec_private.audio_track_ready[track_id_] = true;
        m_state.premux_cv.notify_all();
    }

    EncodeLoop(encoder, kSampleRate, kChannels, sourceFormat);

    encoder.Shutdown();
    uninitCom();
}

// ---------------------------------------------------------------------------
// EncodeLoop — capture/encode drain shared by every audio codec
// ---------------------------------------------------------------------------

void AudioThread::EncodeLoop(IAudioEncoder& enc, uint32_t sample_rate, uint32_t channels,
                             AudioSampleFormat source_format) {
    uint64_t lastAudioPts = 0;

    // M4 Phase 4: one audio producer; Phase 5 will instantiate multiple AudioThread workers.
    auto routeAudioPackets = [&](std::vector<EncodedAudioPacket>& pkts) {
        for (auto& pkt : pkts) {
            if (pkt.bytes.empty())
                continue;

            pkt.track_id = track_id_;
            lastAudioPts = pkt.pts_ns;

            {
                std::unique_lock lk(m_state.premux_mutex);
                bool bothReady = m_state.codec_private.VideoReady(m_state.config.video_codec) &&
                                 m_state.codec_private.AudioAllReady(m_state.audio_track_count);
                if (!bothReady) {
                    if (m_state.audio_premux.size() >= SessionState::kAudioPremuxLimit) {
                        lk.unlock();
                        m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                              "Pre-mux audio buffer limit (600 packets) exceeded "
                                              "before codec private data was ready");
                        return false;
                    }
                    m_state.audio_premux.push_back(std::move(pkt));
                    m_state.diagnostics.OnAudioPremuxDepth(static_cast<uint32_t>(m_state.audio_premux.size()));
                } else {
                    lk.unlock();
                    MuxItem mi;
                    mi.payload = std::move(pkt);
                    std::unique_lock mlk(m_state.mux_mutex);
                    // Bounded steady-state queue: block briefly for room, then
                    // fail cleanly — never drop packets or grow without limit.
                    if (!m_state.WaitForMuxQueueSpace(mlk)) {
                        mlk.unlock();
                        m_state.RecordFailure(E_OUTOFMEMORY, ErrorPhase::Mux,
                                              "Mux queue limit exceeded: the output destination "
                                              "cannot keep up with the recording");
                        return false;
                    }
                    m_state.PushMuxItemLocked(std::move(mi));
                }
            }
        }
        return true;
    };

    std::vector<float> floatScratch;

    // A DATA_DISCONTINUITY means the device lost frames BEFORE the flagged
    // packet. Counting it (diagnostics) is not enough: unless the gap is
    // refilled, every packet that follows lands earlier on the sample timeline
    // and the whole remaining track plays ahead of video — a permanent A/V
    // offset that grows with each underrun. Feed the measured gap into the
    // encoder as silence so PTS (derived from the accumulated frame counter)
    // stays continuous. Chunked so a large (clamped) gap never needs a large
    // scratch buffer.
    auto feedGapSilence = [&](uint32_t gap_frames, uint64_t& accumulated_frames,
                              std::vector<EncodedAudioPacket>& out_pkts) {
        constexpr uint32_t kChunkFrames = 4800; // 100 ms at 48 kHz
        const uint32_t first_chunk = gap_frames < kChunkFrames ? gap_frames : kChunkFrames;
        std::vector<float> zeros(static_cast<size_t>(first_chunk) * channels, 0.0f);
        for (uint32_t remaining = gap_frames; remaining > 0;) {
            const uint32_t n = remaining < kChunkFrames ? remaining : kChunkFrames;
            enc.FeedFloat32(zeros.data(), static_cast<size_t>(n) * channels, 0, accumulated_frames, sample_rate,
                            channels, out_pkts);
            remaining -= n;
        }
    };

    uint64_t encoderAccumulatedFrames = 0;
    bool failed = false;

    // A/V clock-drift estimation: every capture packet carries the device's
    // sample position plus the QPC time it was recorded at; the estimator turns
    // those into a smoothed audio-clock-vs-QPC drift (audio_clock_drift.h).
    AudioClockDriftEstimator drift_estimator;

    // A/V clock slaving (H-3): a P-controller that, once drift crosses the engage
    // threshold, drives the decorator's swr rate compensation to pull the audio
    // output timeline back onto the QPC axis. Gated by config; when off the
    // controller is never fed, so the default 48 kHz/stereo path stays a
    // byte-identical passthrough.
    ClockSlavingController clock_controller;
    const bool clock_slaving_enabled = m_state.config.audio_clock_slaving_enabled;
    bool clock_slaving_logged_engage = false;
    bool clock_slaving_logged_saturation = false;

    // --- Device hot-swap / source-degradation state (ADR 0046) ---
    // bare_degraded: the sole (non-merged) source lost its endpoint; the thread
    // owns its silence + reactivation. Merged tracks self-report per-inner health
    // (DegradedSourceCount) and reactivate through source_->Reinit() instead.
    // lastAccountedQpcNs: wall-clock time up to which the encoder timeline has
    // been accounted. Reset to "now" after every packet that delivered frames, so
    // a subsequent outage's silence fills exactly the gap since the last real
    // audio. lastReinitAttempt throttles reactivation to the poll cadence.
    bool bare_degraded = false;
    uint64_t lastAccountedQpcNs = QpcNowNs();
    auto lastReinitAttempt = std::chrono::steady_clock::now();

    // Feed encoder-cadence silence covering the wall-clock elapsed since the last
    // accounted audio, keeping PTS continuous across a source outage. Returns
    // false only if routing the produced packets failed (a genuine mux failure).
    auto emitSilenceForElapsed = [&]() -> bool {
        const uint64_t now = QpcNowNs();
        if (now <= lastAccountedQpcNs) {
            return true;
        }
        const uint64_t elapsedNs = now - lastAccountedQpcNs;
        uint64_t framesToFill = (elapsedNs * sample_rate) / 1000000000ULL;
        if (framesToFill == 0) {
            return true;
        }
        // Bound a single fill so a long stall never allocates/encodes a huge
        // block at once; the remainder is filled on the next iterations.
        constexpr uint64_t kMaxFillPerIter = 48000ULL * 5; // 5 s of frames
        if (framesToFill > kMaxFillPerIter) {
            framesToFill = kMaxFillPerIter;
        }
        std::vector<EncodedAudioPacket> pkts;
        feedGapSilence(static_cast<uint32_t>(framesToFill), encoderAccumulatedFrames, pkts);
        lastAccountedQpcNs += (framesToFill * 1000000000ULL) / sample_rate;
        {
            std::lock_guard slk(m_state.stats_mutex);
            for (const auto& p : pkts) {
                m_state.stats.audio_packets++;
                m_state.stats.audio_bytes += p.bytes.size();
            }
        }
        return routeAudioPackets(pkts);
    };

    auto markDegradedOccurred = [&]() {
        std::lock_guard slk(m_state.stats_mutex);
        m_state.stats.audio_degraded_occurred = true;
    };

    // Idle wait between drains. Sources initialized with
    // AUDCLNT_STREAMFLAGS_EVENTCALLBACK expose a buffer-ready event (mic
    // capture, process loopback); waiting on it plus the session stop event
    // replaces the 1 ms poll — the stop event preserves today's shutdown
    // behavior of a producer that wakes the moment stop/failure is raised. The
    // bounded timeout is the safety net for the wake paths that have no event:
    // pause_requested toggling, a stop flag set by a worker that bypasses the
    // session helpers, or an engine that stops signaling. Sources without an
    // event keep the previous 1 ms poll: the system-loopback stream never
    // signals capture events (see wasapi_loopback.cpp), and a merged track
    // drains several sources with no single event to wait on.
    HANDLE wait_handles[2] = {m_state.stop_event, static_cast<HANDLE>(source_->BufferReadyEvent())};
    const bool event_driven = (wait_handles[0] != nullptr && wait_handles[1] != nullptr);
    constexpr DWORD kEventWaitTimeoutMs = 10;
    auto waitForCaptureWork = [&]() {
        if (event_driven) {
            WaitForMultipleObjects(2, wait_handles, FALSE, kEventWaitTimeoutMs);
        } else {
            Sleep(1);
        }
    };

    // --- Capture / encode loop ---
    while (!m_state.stop_requested.load()) {
        if (m_state.pause_requested.load()) {
            while (source_->PendingFrameCount() > 0) {
                RawAudioBuffer raw{};
                std::string err;
                if (source_->AcquireBuffer(raw, err)) {
                    source_->ReleaseBuffer();
                } else {
                    // A device loss while paused degrades the source just the same
                    // (ADR 0046): mark it so the resume path reactivates it rather
                    // than silently dropping the endpoint.
                    if (!err.empty() &&
                        ClassifyAudioSourceLoss(source_->LastCaptureHresult()) == AudioLossReaction::DegradeSource &&
                        source_->CaptureSourceCount() <= 1) {
                        bare_degraded = true;
                        markDegradedOccurred();
                    }
                    break;
                }
            }
            Sleep(1);
            continue;
        }

        // Degraded bare source (ADR 0046): keep the encoder timeline honest with
        // wall-clock silence and throttled-reactivate. The dead source is not
        // polled at all until it comes back — polling it would only re-fail.
        if (bare_degraded) {
            m_state.diagnostics.OnAudioSourceHealth(track_id_, 1, 1);
            if (!emitSilenceForElapsed()) {
                failed = true;
                break;
            }
            const auto nowtp = std::chrono::steady_clock::now();
            if (nowtp - lastReinitAttempt >= kAudioReactivatePollDelay) {
                std::string rerr;
                const bool ok = source_->Reinit(rerr);
                const AudioReactivateDecision decision =
                    DecideAudioDeviceLoss(ok, kAudioReactivatePollDelay, kAudioReactivatePollDelay);
                lastReinitAttempt = nowtp;
                if (decision.action == AudioReactivateAction::Reactivated) {
                    bare_degraded = false;
                    // The reacquired stream restarts its device position near
                    // zero; drop the stale drift baseline so the reacquired
                    // timeline is not read as a huge drift.
                    drift_estimator.Reset();
                    // Clock slaving restarts with the reacquired stream (the
                    // decorator reset its compensation in Reinit); re-engage from a
                    // clean baseline rather than resume against stale accounting.
                    clock_controller = ClockSlavingController{};
                    clock_slaving_logged_engage = false;
                    clock_slaving_logged_saturation = false;
                    lastAccountedQpcNs = QpcNowNs();
                    m_state.diagnostics.OnAudioSourceHealth(track_id_, 0, 1);
                }
            }
            Sleep(2);
            continue;
        }

        // Merged-track device-loss health + reactivation (ADR 0046). A merged
        // source (MixedAudioSrc) never fails its acquire — it degrades individual
        // inners and keeps mixing the survivors. This runs every iteration (even
        // at 0 pending) so a fully-degraded merged track still reactivates and
        // keeps its timeline continuous with silence.
        {
            const uint32_t total_sources = source_->CaptureSourceCount();
            const uint32_t degraded_sources = source_->DegradedSourceCount();
            m_state.diagnostics.OnAudioSourceHealth(track_id_, degraded_sources, total_sources);
            if (degraded_sources > 0) {
                markDegradedOccurred();
                const auto nowtp = std::chrono::steady_clock::now();
                if (nowtp - lastReinitAttempt >= kAudioReactivatePollDelay) {
                    std::string rerr;
                    source_->Reinit(rerr); // reacquires degraded inners; survivors untouched
                    lastReinitAttempt = nowtp;
                }
                if (total_sources > 0 && degraded_sources == total_sources) {
                    // Every inner is down: the mixer emits nothing, so hold the
                    // track's timeline with silence like a bare degraded source.
                    if (!emitSilenceForElapsed()) {
                        failed = true;
                        break;
                    }
                }
            }
        }

        uint32_t pendingFrames = source_->PendingFrameCount();
        m_state.diagnostics.OnAudioQueueDepth(pendingFrames);
        if (pendingFrames == 0) {
            waitForCaptureWork();
            continue;
        }

        bool anyWork = false;
        while (source_->PendingFrameCount() > 0) {
            RawAudioBuffer raw{};
            std::string captureErr;
            if (!source_->AcquireBuffer(raw, captureErr)) {
                if (!captureErr.empty()) {
                    const int32_t captureHr = source_->LastCaptureHresult();
                    // ADR 0046: an audio endpoint lost mid-recording no longer
                    // ends the session. Degrade this source to honest silence and
                    // reactivate it (handled by the bare_degraded branch above);
                    // video and every other track keep running. A benign no-data
                    // classification simply stops draining this tick.
                    if (ClassifyAudioSourceLoss(captureHr) == AudioLossReaction::DegradeSource) {
                        if (!bare_degraded) {
                            bare_degraded = true;
                            lastReinitAttempt = std::chrono::steady_clock::now();
                            markDegradedOccurred();
                        }
                    }
                }
                break;
            }

            if (raw.data_discontinuity) {
                m_state.diagnostics.OnAudioDiscontinuity();
            }

            {
                AudioDeviceTiming timing{};
                if (source_->LastBufferDeviceTiming(timing)) {
                    drift_estimator.AddObservation(timing.device_position_ns, timing.qpc_position_ns);
                    const double raw_drift = drift_estimator.DriftMs();
                    double residual = raw_drift; // no slaving => residual is the raw drift
                    double applied_ppm = 0.0;
                    if (clock_slaving_enabled && output_format_src_ != nullptr) {
                        const double applied = output_format_src_->AppliedCompensationMs();
                        if (clock_controller.Update(raw_drift, applied, timing.qpc_position_ns)) {
                            output_format_src_->SetCompensationPpm(clock_controller.Ppm());
                            if (!clock_slaving_logged_engage) {
                                clock_slaving_logged_engage = true;
                                logging::LogField f[] = {{"track", std::to_string(track_id_)},
                                                         {"drift_ms", std::to_string(raw_drift)}};
                                logging::log(logging::LogLevel::Info, "audio.clock_slaving", "clock slaving engaged",
                                             std::span<const logging::LogField>(f, std::size(f)));
                            } else {
                                logging::LogField f[] = {{"track", std::to_string(track_id_)},
                                                         {"ppm", std::to_string(clock_controller.Ppm())}};
                                logging::log(logging::LogLevel::Debug, "audio.clock_slaving", "compensation updated",
                                             std::span<const logging::LogField>(f, std::size(f)));
                            }
                        }
                        residual = clock_controller.ResidualMs();
                        applied_ppm = clock_controller.Ppm();
                        // Saturation warn (once): the rate is pinned at the cap and
                        // the residual still exceeds the cap-case bound — drift beyond
                        // the correction envelope (defective hardware/driver). Calm,
                        // not alarmist: a log line, no toast, no blocker.
                        if (!clock_slaving_logged_saturation &&
                            std::abs(applied_ppm) >= ClockSlavingController::kMaxPpm &&
                            std::abs(residual) >
                                ClockSlavingController::kMaxPpm * ClockSlavingController::kControlHorizonS / 1000.0) {
                            clock_slaving_logged_saturation = true;
                            logging::LogField f[] = {{"track", std::to_string(track_id_)},
                                                     {"residual_ms", std::to_string(residual)},
                                                     {"ppm", std::to_string(applied_ppm)}};
                            logging::log(logging::LogLevel::Warn, "audio.clock_slaving",
                                         "clock slaving saturated; drift exceeds correction envelope",
                                         std::span<const logging::LogField>(f, std::size(f)));
                        }
                    }
                    m_state.diagnostics.OnAudioClockSlaving(track_id_, raw_drift, residual, applied_ppm);
                }
            }

            std::vector<EncodedAudioPacket> pkts;
            float new_rms = 0.0f;
            if (raw.gap_frames > 0) {
                feedGapSilence(raw.gap_frames, encoderAccumulatedFrames, pkts);
            }
            const size_t totalSamples = static_cast<size_t>(raw.num_frames) * static_cast<size_t>(channels);
            if (raw.silent) {
                std::vector<float> silence(totalSamples, 0.0f);
                enc.FeedFloat32(silence.data(), silence.size(), 0, encoderAccumulatedFrames, sample_rate, channels,
                                pkts);
            } else if (raw.bytes == nullptr) {
                source_->ReleaseBuffer();
                m_state.RecordFailure(E_FAIL, ErrorPhase::AudioCapture,
                                      "Audio source returned null bytes for non-silent packet");
                failed = true;
                break;
            } else if (source_format == AudioSampleFormat::Float32) {
                new_rms = ComputeRmsLinear(reinterpret_cast<const float*>(raw.bytes), totalSamples);
                enc.FeedFloat32(reinterpret_cast<const float*>(raw.bytes), totalSamples, 0, encoderAccumulatedFrames,
                                sample_rate, channels, pkts);
            } else {
                floatScratch.resize(totalSamples);
                ConvertInt16ToFloat32(reinterpret_cast<const std::int16_t*>(raw.bytes), floatScratch.data(),
                                      totalSamples);
                new_rms = ComputeRmsLinear(floatScratch.data(), floatScratch.size());
                enc.FeedFloat32(floatScratch.data(), floatScratch.size(), 0, encoderAccumulatedFrames, sample_rate,
                                channels, pkts);
            }
            m_smoothed_rms_ = kRmsEmaAlpha * new_rms + (1.0f - kRmsEmaAlpha) * m_smoothed_rms_;

            source_->ReleaseBuffer();

            // A packet that carried real frames advances the timeline normally;
            // rebase the silence clock to now so a subsequent outage's silence
            // fills exactly the gap after this last real audio (ADR 0046).
            if (raw.num_frames > 0) {
                lastAccountedQpcNs = QpcNowNs();
            }

            // Update stats
            {
                std::lock_guard slk(m_state.stats_mutex);
                for (const auto& p : pkts) {
                    m_state.stats.audio_packets++;
                    m_state.stats.audio_bytes += p.bytes.size();
                }
                if (track_id_ < m_state.stats.per_track_rms.size()) {
                    m_state.stats.per_track_rms[track_id_] = m_smoothed_rms_;
                }
            }

            if (!routeAudioPackets(pkts)) {
                failed = true;
                break;
            }

            anyWork = true;
        }

        // A merged inner can go degraded during the drain above (its acquire
        // fails inside MixedAudioSrc). Record it here too, so the post-flight
        // fact and live diagnostics are accurate even when the whole track
        // drains in a single outer iteration and the pre-drain block does not
        // run again before the session ends (ADR 0046).
        {
            const uint32_t degraded_after = source_->DegradedSourceCount();
            if (degraded_after > 0) {
                markDegradedOccurred();
                m_state.diagnostics.OnAudioSourceHealth(track_id_, degraded_after, source_->CaptureSourceCount());
            }
        }

        if (failed)
            break;

        if (!anyWork)
            waitForCaptureWork();
    }

    // --- Drain the resampler before the source (and its SwrContext) goes away ---
    // Whenever a resample context is active — a non-default output rate/channel
    // count, or engaged clock slaving on the default 48 kHz path — libswresample
    // holds already-captured audio in its filter delay (~10 ms on a rate
    // conversion, sub-ms to a few ms otherwise). Push it through the encoder as a
    // normal buffer first: the encoder derives PTS from the accumulated frame
    // counter, so the tail lands directly after the last real packet. Shutdown()
    // frees the context, so this must run before it; the encoder's own EOS drain
    // must run after, or the tail would sit behind an already-flushed encoder.
    if (output_format_src_ != nullptr && !failed) {
        RawAudioBuffer tail{};
        int64_t undrained_frames = 0;
        const uint32_t tail_frames = output_format_src_->DrainResampler(tail, &undrained_frames);
        // Post-flight fact for the session report: how much tail the drain
        // recovered, and what (if anything) it had to leave behind. The recorded
        // bit is what distinguishes "the drain ran and found nothing" from "the
        // drain never ran" — a session that failed or timed out before this point
        // leaves the counters untouched, and 0 there is not a measurement.
        {
            std::lock_guard slk(m_state.stats_mutex);
            if (track_id_ < m_state.stats.per_track_resampler_drained_frames.size()) {
                m_state.stats.per_track_resampler_drain_recorded[track_id_] = true;
                m_state.stats.per_track_resampler_drained_frames[track_id_] = tail_frames;
                m_state.stats.per_track_resampler_undrained_frames[track_id_] =
                    undrained_frames > 0 ? static_cast<uint64_t>(undrained_frames) : 0;
            }
        }
        if (undrained_frames > 0) {
            // The flush loop gave up at its iteration bound with the context
            // still holding audio. Not reachable with libswresample's real flush
            // behaviour; log it rather than lose the remainder silently.
            logging::LogField f[] = {{"track", std::to_string(track_id_)},
                                     {"iteration_bound", std::to_string(OutputFormatAudioSrc::kMaxDrainIterations)},
                                     {"drained_frames", std::to_string(tail_frames)},
                                     {"undrained_frames", std::to_string(undrained_frames)}};
            logging::log(logging::LogLevel::Warn, "audio.resampler_drain",
                         "resampler drain hit its iteration bound; the remaining tail was dropped",
                         std::span<const logging::LogField>(f, std::size(f)));
        }
        if (tail_frames > 0 && tail.bytes != nullptr) {
            std::vector<EncodedAudioPacket> tailPkts;
            enc.FeedFloat32(reinterpret_cast<const float*>(tail.bytes),
                            static_cast<size_t>(tail_frames) * static_cast<size_t>(channels), 0,
                            encoderAccumulatedFrames, sample_rate, channels, tailPkts);
            {
                std::lock_guard slk(m_state.stats_mutex);
                for (const auto& p : tailPkts) {
                    m_state.stats.audio_packets++;
                    m_state.stats.audio_bytes += p.bytes.size();
                }
            }
            routeAudioPackets(tailPkts);
        }
    }

    source_->Shutdown();

    // --- Drain the encoder (flush semantics are the encoder's own: Opus pads
    // the final frame, FLAC finishes the stream, AAC drains its delay line,
    // PCM holds no buffered state and emits nothing) ---
    {
        std::vector<EncodedAudioPacket> drainPkts;
        enc.Flush(drainPkts);

        {
            std::lock_guard slk(m_state.stats_mutex);
            for (const auto& p : drainPkts) {
                m_state.stats.audio_packets++;
                m_state.stats.audio_bytes += p.bytes.size();
            }
        }

        routeAudioPackets(drainPkts);
    }

    // --- Update final stats ---
    {
        std::lock_guard lk(m_state.stats_mutex);
        if (lastAudioPts > m_state.stats.audio_duration_ns) {
            m_state.stats.audio_duration_ns = lastAudioPts;
        }
    }

    // --- Push audio EOS sentinel ---
    {
        MuxItem eos;
        eos.payload = AudioEosSentinel{track_id_};
        std::lock_guard lk(m_state.mux_mutex);
        m_state.PushMuxItemLocked(std::move(eos)); // sentinel: bypasses the queue bound
    }
}

} // namespace recorder_core
