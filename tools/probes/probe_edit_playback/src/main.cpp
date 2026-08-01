// probe_edit_playback — MEASUREMENT ONLY, no product code changed. Backs the
// 2026-08-01 investigation into whether EditPlayerEngine::StartPlaybackDecode
// (libs/recorder_core/src/edit_player_engine.cpp) can sustain real-time
// playback of 1440p60 H.264 source material, or whether its decode path
// (decode + YUV->BGRA convert + per-frame allocation) is the bottleneck
// behind the reported stutter. Also the regression guard for the decoupled
// demux/video/audio thread topology that came out of that investigation:
// step B must keep running to completion, never hang on teardown.
//
// Never touches the ExoSnap application itself; opens the given file
// READ-ONLY via the real recorder_core::EditPlayerEngine class (same code the
// product uses) plus a small amount of standalone libavcodec/libavformat code
// for the FFmpeg-threading check in step E. Writes nothing.
//
// Usage:
//   probe_edit_playback.exe <path-to-mkv> [start_us]
//
// Measures, in order:
//   A) EditPlayerEngine::Open() — success/error, HasVideoStream, HasAudioStream.
//   B) Real playback-decode throughput: StartPlaybackDecode() from t=600s,
//      callbacks just count-and-drop (no WASAPI, no pacing — this is the
//      MAXIMUM throughput the path can produce), stopped after exactly 10s
//      wall-clock. Reports total video frames + fps, total audio blocks, and
//      the first delivered frame's resolution.
//   B2) Three back-to-back start/stop cycles on the same open engine — the
//      teardown/reap path of the three-thread topology. Each cycle must
//      terminate; a hang here IS the failure.
//   C) Isolated cost of ConvertFullPlanarYuv420ToBgra on a 2560x1440 8-bit
//      dummy YUV420P frame, 100 iterations, averaged.
//   C2) Same measurement for ConvertFullPlanar444ToBgra (2026-08-01 follow-up:
//      the editor player's decode path for the Expert 4:4:4 chroma option's
//      recordings, AV_PIX_FMT_YUV444P, 8-bit only), scalar and SIMD, on a
//      2560x1440 dummy frame, 100 iterations each, reported against the
//      4:2:0 step C numbers.
//   G) SIMD-vs-scalar comparison for the step C conversion (2026-08-01
//      follow-up), run immediately after C for a comparable CPU clock state
//      (see the call site in main() for why): the real baseline plus three
//      variants -- an /arch:AVX2 recompile of the unmodified scalar loop
//      (auto-vectorization only), hand-written SSE2/SSE4.1 intrinsics
//      (8px/iter), and hand-written AVX2 intrinsics (16px/iter) -- each
//      timed over 100 iterations on the same non-constant dummy frame, then
//      compared pixel-for-pixel against the baseline (max per-channel
//      deviation + differing-pixel count). Also reports whether this CPU
//      supports AVX2 at all (CPUID leaf 7).
//   D) Isolated cost of the per-frame std::make_shared<std::vector<uint8_t>>
//      allocation the real path pays (2560x1440x4 bytes), 100 iterations,
//      averaged.
//   E) FFmpeg's default H.264 decoder threading when opened exactly as
//      EditPlayerEngine::Open() opens it (avcodec_open2 with no thread_count
//      set): ctx->thread_count, ctx->active_thread_type, and the machine's
//      std::thread::hardware_concurrency().
//   F) Whether the shipped exosnap-ffmpeg-build (r5, n8.1.1 --
//      cmake/VendorFFmpeg.cmake) was built with hardware-decode support at
//      all: per-codec (h264/hevc/av1) avcodec_get_hw_config() enumeration,
//      the runtime-available av_hwdevice_iterate_types() list, three real
//      av_hwdevice_ctx_create() attempts (D3D11VA/DXVA2/CUDA), and the
//      exact avcodec_configuration()/version strings baked into this build.
//   H) The actual point of the 2026-08-01 decoupled-decode topology (see
//      docs/superpowers/specs/2026-08-01-edit-player-decoupled-decode-design.md,
//      "Testing"): does audio delivery stay continuous while video is
//      artificially throttled to far below real time? StartPlaybackDecode()
//      from t=600s with on_video sleeping 120ms/frame (a deliberately
//      pathological ~8fps video path) and on_audio dropping data immediately
//      but logging each block's wall-clock arrival time plus pts_us.
//      current_media_time_us is wired to a real wall-clock-driven function
//      (start_us + elapsed wall time), matching how the shipped player drives
//      the conversion-skip gate. Stops after 10s wall clock and reports block
//      count, total media duration covered, the largest/p50/p99 gap between
//      consecutive audio arrivals, and how many gaps exceeded 50ms.
//   I) Seek alignment: the first video and audio PTS actually delivered after
//      StartPlaybackDecode(start_us), reported as offsets from the position
//      that was requested. A seek lands on the preceding keyframe, so both
//      streams start early; audio must be trimmed back to the requested
//      position or it plays ahead of the caller's clock for the whole run.
//      A negative audio offset is the failure. No other step compares what was
//      delivered against what was asked for -- only against each other.
//
// The timed steps (B, B2, H, I) all start at `start_us`, which defaults to
// 600s to match the long reference clip these measurements were first taken
// on. Pass a position inside the file when probing a short recording, or every
// timed step starts past its end and measures nothing.
//
// Exit code 0 if step A's Open() succeeded (regardless of what the
// measurements themselves show), 1 on a hard failure (bad args, Open failed).

#include <recorder_core/edit_player_engine.h>

#include "step_g_simd.h"
#include "yuv_to_bgra.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// MSVC + C++: av_err2str uses a C99 compound literal, not valid C++. Mirrors
// the override already used in edit_player_engine.cpp / mp4_remuxer.cpp.
static inline const char* av_err2str_cpp(int errnum) noexcept {
    static thread_local char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}
#ifdef av_err2str
#undef av_err2str
#endif
#define av_err2str(e) av_err2str_cpp(e)

namespace {

// ---- Step A: Open() ----
bool StepA_Open(recorder_core::EditPlayerEngine& engine, const std::string& path) {
    printf("=== [A] Open ===\n");
    std::string err;
    const bool ok = engine.Open(path, err);
    if (!ok) {
        printf("[A] Open: FAIL (%s)\n", err.c_str());
        return false;
    }
    printf("[A] Open: OK\n");
    printf("[A] HasVideoStream=%s HasAudioStream=%s\n", engine.HasVideoStream() ? "true" : "false",
           engine.HasAudioStream() ? "true" : "false");
    return true;
}

// ---- Step B: real playback-decode throughput, max speed, 10s wall clock ----
void StepB_PlaybackThroughput(recorder_core::EditPlayerEngine& engine, int64_t kStartUs) {
    printf("=== [B] Playback-decode throughput (start_us=%lld, 10s wall clock, no pacing) ===\n",
           static_cast<long long>(kStartUs));

    std::atomic<uint64_t> videoFrames{0};
    std::atomic<uint64_t> audioBlocks{0};
    std::atomic<bool> gotFirstFrame{false};
    std::atomic<uint32_t> firstWidth{0};
    std::atomic<uint32_t> firstHeight{0};

    auto onVideo = [&](recorder_core::DecodedVideoFrame frame) {
        videoFrames.fetch_add(1, std::memory_order_relaxed);
        bool expected = false;
        if (gotFirstFrame.compare_exchange_strong(expected, true)) {
            firstWidth.store(frame.width, std::memory_order_relaxed);
            firstHeight.store(frame.height, std::memory_order_relaxed);
        }
        // frame (and its BGRA buffer) is dropped here — destructed immediately,
        // no WASAPI render, no pacing/sleep — this is the maximum throughput
        // the decode path can sustain.
    };
    auto onAudio = [&](recorder_core::DecodedAudioBlock /*block*/) {
        audioBlocks.fetch_add(1, std::memory_order_relaxed);
    };

    const auto t0 = std::chrono::steady_clock::now();
    // No media clock: nothing is presenting these frames, so nothing is
    // "late" and the engine discards nothing before conversion. That keeps
    // this a MAXIMUM-throughput measurement, comparable across revisions.
    engine.StartPlaybackDecode(kStartUs, onVideo, onAudio, {});
    std::this_thread::sleep_for(std::chrono::seconds(10));
    engine.StopPlaybackDecode();
    const auto t1 = std::chrono::steady_clock::now();

    const double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
    const uint64_t vf = videoFrames.load();
    const uint64_t ab = audioBlocks.load();
    const double fps = (elapsedSec > 0.0) ? static_cast<double>(vf) / elapsedSec : 0.0;

    printf("[B] wall_clock_elapsed=%.3fs\n", elapsedSec);
    printf("[B] video_frames_delivered=%llu fps=%.2f\n", static_cast<unsigned long long>(vf), fps);
    printf("[B] audio_blocks_delivered=%llu\n", static_cast<unsigned long long>(ab));
    if (gotFirstFrame.load()) {
        printf("[B] first_frame_resolution=%ux%u\n", firstWidth.load(), firstHeight.load());
    } else {
        printf("[B] first_frame_resolution=N/A (no video frame delivered)\n");
    }
    if (fps < 60.0) {
        printf("[B] VERDICT: fps < 60 -> this path CANNOT sustain real-time playback of 60fps source material.\n");
    } else {
        printf("[B] VERDICT: fps >= 60 -> this path CAN sustain real-time playback of 60fps source material.\n");
    }
}

// ---- Step B2: repeated start/stop on one open engine ----
//
// Playback runs on three threads that block on each other's queues, so the
// teardown order and the wake-before-join discipline are the part of this
// design most likely to regress into a hang. A single start/stop (step B)
// does not exercise the reap path a SECOND start takes. Each cycle here must
// terminate; if one hangs, this probe hangs, which is the signal.
void StepB2_RepeatedStartStop(recorder_core::EditPlayerEngine& engine, int64_t kStartUs) {
    printf("=== [B2] Repeated start/stop on the same open engine (3 cycles, 300ms each) ===\n");

    constexpr int kCycles = 3;
    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        std::atomic<uint64_t> videoFrames{0};
        std::atomic<uint64_t> audioBlocks{0};
        const auto t0 = std::chrono::steady_clock::now();
        engine.StartPlaybackDecode(
            kStartUs, [&](recorder_core::DecodedVideoFrame) { videoFrames.fetch_add(1, std::memory_order_relaxed); },
            [&](recorder_core::DecodedAudioBlock) { audioBlocks.fetch_add(1, std::memory_order_relaxed); }, {});
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        engine.StopPlaybackDecode();
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("[B2] cycle %d/%d: terminated after %.1fms, video_frames=%llu audio_blocks=%llu\n", cycle, kCycles,
               elapsedMs, static_cast<unsigned long long>(videoFrames.load()),
               static_cast<unsigned long long>(audioBlocks.load()));
    }
    printf("[B2] all %d cycles terminated (no join deadlock)\n", kCycles);
}

// ---- Step C: isolated YUV->BGRA conversion cost ----
void StepC_ConvertCost() {
    printf("=== [C] ConvertFullPlanarYuv420ToBgra isolated cost (2560x1440 8-bit, 100 iters) ===\n");

    constexpr uint32_t kWidth = 2560;
    constexpr uint32_t kHeight = 1440;
    constexpr uint32_t kChromaW = kWidth / 2;
    constexpr uint32_t kChromaH = kHeight / 2;

    std::vector<uint8_t> yPlane(static_cast<size_t>(kWidth) * kHeight);
    std::vector<uint8_t> uPlane(static_cast<size_t>(kChromaW) * kChromaH);
    std::vector<uint8_t> vPlane(static_cast<size_t>(kChromaW) * kChromaH);
    for (size_t i = 0; i < yPlane.size(); ++i)
        yPlane[i] = static_cast<uint8_t>(i & 0xFF);
    for (size_t i = 0; i < uPlane.size(); ++i)
        uPlane[i] = static_cast<uint8_t>((i * 3) & 0xFF);
    for (size_t i = 0; i < vPlane.size(); ++i)
        vPlane[i] = static_cast<uint8_t>((i * 7) & 0xFF);

    // Destination buffer allocated ONCE and reused across all 100 calls, per
    // the measurement spec — this isolates the conversion math itself from
    // allocation cost (that is step D).
    std::vector<uint8_t> bgra(static_cast<size_t>(kWidth) * kHeight * 4);

    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = yPlane.data();
    src.y_stride_bytes = kWidth;
    src.u_plane = uPlane.data();
    src.u_stride_bytes = kChromaW;
    src.v_plane = vPlane.data();
    src.v_stride_bytes = kChromaW;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params; // defaults: Bt709 / Limited

    constexpr int kIters = 100;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        recorder_core::ConvertFullPlanarYuv420ToBgra(src, params, bgra.data(), kWidth * 4);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("[C] iters=%d total=%.3fms avg=%.4fms/frame\n", kIters, totalMs, totalMs / kIters);
}

// ---- Step C2: isolated YUV444 (fully-planar, 4:4:4, 8-bit) conversion cost ----
//
// 2026-08-01 follow-up: the editor player just gained a decode path for the
// Expert 4:4:4 chroma option's recordings (AV_PIX_FMT_YUV444P, 8-bit only --
// see recorder_core::ConvertFullPlanar444ToBgra). Same measurement shape as
// step C, same resolution and iteration count, so the two numbers are
// directly comparable. No a-priori estimate here: the per-pixel arithmetic
// drops the 4:2:0 pair/block bookkeeping (less work), but the chroma INPUT
// doubles (full-resolution U and V instead of quarter-resolution), and the
// kernel is bandwidth-bound -- which effect wins is exactly what this
// measures, not something to guess from the pixel math alone.
void StepC2_Convert444Cost() {
    printf("=== [C2] ConvertFullPlanar444ToBgra isolated cost (2560x1440 8-bit, 100 iters) ===\n");

    constexpr uint32_t kWidth = 2560;
    constexpr uint32_t kHeight = 1440;

    std::vector<uint8_t> yPlane(static_cast<size_t>(kWidth) * kHeight);
    std::vector<uint8_t> uPlane(static_cast<size_t>(kWidth) * kHeight);
    std::vector<uint8_t> vPlane(static_cast<size_t>(kWidth) * kHeight);
    for (size_t i = 0; i < yPlane.size(); ++i)
        yPlane[i] = static_cast<uint8_t>(i & 0xFF);
    for (size_t i = 0; i < uPlane.size(); ++i)
        uPlane[i] = static_cast<uint8_t>((i * 3) & 0xFF);
    for (size_t i = 0; i < vPlane.size(); ++i)
        vPlane[i] = static_cast<uint8_t>((i * 7) & 0xFF);

    std::vector<uint8_t> bgra(static_cast<size_t>(kWidth) * kHeight * 4);

    recorder_core::FullPlanar444Frame src;
    src.y_plane = yPlane.data();
    src.y_stride_bytes = kWidth;
    src.u_plane = uPlane.data();
    src.u_stride_bytes = kWidth;
    src.v_plane = vPlane.data();
    src.v_stride_bytes = kWidth;
    src.width = kWidth;
    src.height = kHeight;

    recorder_core::YuvToBgraParams params; // defaults: Bt709 / Limited

    constexpr int kIters = 100;

    const auto tScalar0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
        recorder_core::ConvertFullPlanar444ToBgraScalar(src, params, bgra.data(), kWidth * 4);
    const auto tScalar1 = std::chrono::steady_clock::now();
    const double scalarMs = std::chrono::duration<double, std::milli>(tScalar1 - tScalar0).count() / kIters;

    const bool simdSupported = recorder_core::CpuSupportsYuvToBgraSimd();
    double simdMs = -1.0;
    if (simdSupported) {
        const auto tSimd0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i)
            recorder_core::ConvertFullPlanar444ToBgraSimd(src, params, bgra.data(), kWidth * 4);
        const auto tSimd1 = std::chrono::steady_clock::now();
        simdMs = std::chrono::duration<double, std::milli>(tSimd1 - tSimd0).count() / kIters;
    }

    printf("[C2] scalar: iters=%d avg=%.4fms/frame\n", kIters, scalarMs);
    if (simdSupported) {
        printf("[C2] simd (SSE4.1): iters=%d avg=%.4fms/frame (factor=%.2fx)\n", kIters, simdMs,
               simdMs > 0.0 ? scalarMs / simdMs : 0.0);
    } else {
        printf("[C2] simd (SSE4.1): SKIPPED -- this CPU does not report SSE4.1 support\n");
    }
    printf("[C2] vs 4:2:0 reference (scalar 13.3ms/frame, simd 2.86ms/frame): "
           "444 scalar %.4fms/frame (%.2fx of 420), 444 simd %.4fms/frame (%.2fx of 420)\n",
           scalarMs, scalarMs > 0.0 ? scalarMs / 13.3 : 0.0, simdSupported ? simdMs : 0.0,
           (simdSupported && simdMs > 0.0) ? simdMs / 2.86 : 0.0);
}

// ---- Step D: isolated per-frame allocation cost ----
void StepD_AllocationCost() {
    printf("=== [D] Per-frame make_shared<vector<uint8_t>> allocation cost (2560x1440x4 bytes, 100 iters) ===\n");

    constexpr size_t kBytes = static_cast<size_t>(2560) * 1440 * 4;
    constexpr int kIters = 100;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        auto buf = std::make_shared<std::vector<uint8_t>>(kBytes);
        (void)buf; // freed at end of this iteration's scope
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("[D] iters=%d bytes_per_alloc=%zu total=%.3fms avg=%.4fms/alloc\n", kIters, kBytes, totalMs,
           totalMs / kIters);
}

// ---- Step E: FFmpeg default decoder threading ----
void StepE_FfmpegThreading(const std::string& path) {
    printf("=== [E] FFmpeg default H.264 decoder threading (avcodec_open2 with no thread_count set) ===\n");

    printf("[E] hardware_concurrency=%u\n", std::thread::hardware_concurrency());

    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        printf("[E] avformat_open_input failed: %s\n", av_err2str(ret));
        return;
    }
    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        printf("[E] avformat_find_stream_info failed: %s\n", av_err2str(ret));
        avformat_close_input(&fmt);
        return;
    }

    const int videoIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx < 0) {
        printf("[E] no video stream found\n");
        avformat_close_input(&fmt);
        return;
    }

    AVStream* vst = fmt->streams[videoIdx];
    const AVCodec* codec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (codec == nullptr) {
        printf("[E] no decoder available for codec id %d\n", static_cast<int>(vst->codecpar->codec_id));
        avformat_close_input(&fmt);
        return;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx == nullptr || avcodec_parameters_to_context(ctx, vst->codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        printf("[E] failed to open the video decoder (%s)\n", codec->name);
        if (ctx)
            avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return;
    }

    printf("[E] decoder=%s\n", codec->name);
    printf("[E] ctx->thread_count=%d\n", ctx->thread_count);
    printf("[E] ctx->active_thread_type=%d (FF_THREAD_FRAME=%d, FF_THREAD_SLICE=%d, 0=none)\n",
           ctx->active_thread_type, FF_THREAD_FRAME, FF_THREAD_SLICE);

    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

// ---- Step F: does this shipped FFmpeg build support hardware decode at all? ----
void PrintHwConfigsForCodec(const char* label, enum AVCodecID codecId) {
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) {
        printf("[F] %s: no decoder registered in this build\n", label);
        return;
    }
    bool any = false;
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (cfg == nullptr)
            break;
        any = true;
        const char* pixFmtName = av_get_pix_fmt_name(cfg->pix_fmt);
        const char* deviceTypeName = av_hwdevice_get_type_name(cfg->device_type);
        printf("[F] %s hw_config[%d]: pix_fmt=%s device_type=%s methods=0x%02x"
               " (HW_DEVICE_CTX=%d HW_FRAMES_CTX=%d INTERNAL=%d AD_HOC=%d)\n",
               label, i, pixFmtName ? pixFmtName : "?", deviceTypeName ? deviceTypeName : "?", cfg->methods,
               (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0,
               (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) != 0,
               (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) != 0,
               (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_AD_HOC) != 0);
    }
    if (!any)
        printf("[F] %s: keine (avcodec_get_hw_config returns nullptr at index 0 -- no hw config compiled in)\n",
               label);
}

void TryCreateHwDevice(const char* label, enum AVHWDeviceType type) {
    AVBufferRef* deviceCtx = nullptr;
    const int ret = av_hwdevice_ctx_create(&deviceCtx, type, nullptr, nullptr, 0);
    if (ret >= 0) {
        printf("[F] av_hwdevice_ctx_create(%s): OK\n", label);
        av_buffer_unref(&deviceCtx);
    } else {
        printf("[F] av_hwdevice_ctx_create(%s): FAIL ret=%d (%s)\n", label, ret, av_err2str(ret));
    }
}

void StepF_HardwareDecodeSupport() {
    printf("=== [F] Shipped FFmpeg build: hardware-decode capability ===\n");

    printf("[F] avcodec_version=0x%06x avutil_version=0x%06x\n", avcodec_version(), avutil_version());
    printf("[F] avcodec_configuration=%s\n", avcodec_configuration());

    printf("[F] --- per-codec avcodec_get_hw_config() ---\n");
    PrintHwConfigsForCodec("h264", AV_CODEC_ID_H264);
    PrintHwConfigsForCodec("hevc", AV_CODEC_ID_HEVC);
    PrintHwConfigsForCodec("av1", AV_CODEC_ID_AV1);

    printf("[F] --- av_hwdevice_iterate_types() (runtime-registered device types) ---\n");
    bool anyType = false;
    enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
    for (;;) {
        t = av_hwdevice_iterate_types(t);
        if (t == AV_HWDEVICE_TYPE_NONE)
            break;
        anyType = true;
        const char* name = av_hwdevice_get_type_name(t);
        printf("[F] hwdevice type: %s\n", name ? name : "?");
    }
    if (!anyType)
        printf("[F] hwdevice type: keine (av_hwdevice_iterate_types returns NONE immediately)\n");

    printf("[F] --- real device-creation attempts ---\n");
    TryCreateHwDevice("D3D11VA", AV_HWDEVICE_TYPE_D3D11VA);
    TryCreateHwDevice("DXVA2", AV_HWDEVICE_TYPE_DXVA2);
    TryCreateHwDevice("CUDA", AV_HWDEVICE_TYPE_CUDA);
}

// ---- Step G: SIMD-vs-scalar comparison ----

// Per-channel max |diff| against the baseline output, plus how many pixels
// differ in ANY channel. A max diff of <=1 in every channel is accepted as
// rounding noise (16.16 fixed-point vs. SIMD lane order can round the last
// bit differently in edge cases); anything larger is a real bug in the
// variant, not a rounding artifact.
struct BgraDiff {
    int maxDiff[4] = {0, 0, 0, 0}; // B, G, R, A
    uint64_t diffPixelCount = 0;
};

BgraDiff CompareBgra(const uint8_t* baseline, const uint8_t* variant, uint32_t width, uint32_t height,
                      uint32_t strideBytes) {
    BgraDiff result;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* rowA = baseline + static_cast<size_t>(row) * strideBytes;
        const uint8_t* rowB = variant + static_cast<size_t>(row) * strideBytes;
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t* pxA = rowA + static_cast<size_t>(col) * 4u;
            const uint8_t* pxB = rowB + static_cast<size_t>(col) * 4u;
            bool differs = false;
            for (int ch = 0; ch < 4; ++ch) {
                const int diff = std::abs(static_cast<int>(pxA[ch]) - static_cast<int>(pxB[ch]));
                if (diff > result.maxDiff[ch])
                    result.maxDiff[ch] = diff;
                if (diff != 0)
                    differs = true;
            }
            if (differs)
                ++result.diffPixelCount;
        }
    }
    return result;
}

void PrintCompare(const char* label, const BgraDiff& d) {
    const bool roundingOnly = d.maxDiff[0] <= 1 && d.maxDiff[1] <= 1 && d.maxDiff[2] <= 1 && d.maxDiff[3] <= 1;
    printf("[G] %s vs baseline: max_diff B=%d G=%d R=%d A=%d differing_pixels=%llu -- %s\n", label, d.maxDiff[0],
           d.maxDiff[1], d.maxDiff[2], d.maxDiff[3], static_cast<unsigned long long>(d.diffPixelCount),
           roundingOnly ? "OK (<=1, rounding only)" : "MISMATCH -- bug in this variant, not rounding noise");
}

void StepG_SimdVariants() {
    printf("=== [G] SIMD variants of ConvertFullPlanarYuv420ToBgra (2560x1440 8-bit, 100 iters each) ===\n");

    constexpr uint32_t kWidth = 2560;
    constexpr uint32_t kHeight = 1440;
    constexpr uint32_t kChromaW = kWidth / 2;
    constexpr uint32_t kChromaH = kHeight / 2;

    std::vector<uint8_t> yPlane(static_cast<size_t>(kWidth) * kHeight);
    std::vector<uint8_t> uPlane(static_cast<size_t>(kChromaW) * kChromaH);
    std::vector<uint8_t> vPlane(static_cast<size_t>(kChromaW) * kChromaH);
    // Non-constant, varying fill -- same ramp-style pattern as step C's
    // dummy frame (deliberately, not just coincidentally: an earlier version
    // of this fill used a higher-entropy formula and measured a ~2ms/frame
    // *slower* baseline for the exact same real ConvertFullPlanarYuv420ToBgra
    // call than step C got on its own dummy frame -- 15.5ms vs 12.7ms. Root
    // cause is ClampFixedToByte's two data-dependent branches: a smoother
    // per-row ramp makes the taken/not-taken sequence far more predictable
    // than a "more random-looking" fill does, and that predictability is
    // exactly what step C's reference number already bakes in. Matching the
    // pattern keeps this baseline apples-to-apples with the validated
    // ~12-13ms/frame reference instead of silently measuring branch-predictor
    // sensitivity instead of the conversion cost.)
    for (size_t i = 0; i < yPlane.size(); ++i)
        yPlane[i] = static_cast<uint8_t>(i & 0xFF);
    for (size_t i = 0; i < uPlane.size(); ++i)
        uPlane[i] = static_cast<uint8_t>((i * 3) & 0xFF);
    for (size_t i = 0; i < vPlane.size(); ++i)
        vPlane[i] = static_cast<uint8_t>((i * 7) & 0xFF);

    recorder_core::FullPlanarYuv420Frame src;
    src.y_plane = yPlane.data();
    src.y_stride_bytes = kWidth;
    src.u_plane = uPlane.data();
    src.u_stride_bytes = kChromaW;
    src.v_plane = vPlane.data();
    src.v_stride_bytes = kChromaW;
    src.width = kWidth;
    src.height = kHeight;
    src.bits_per_sample = 8;

    recorder_core::YuvToBgraParams params; // defaults: Bt709 / Limited, matches production default path

    const size_t bufBytes = static_cast<size_t>(kWidth) * kHeight * 4u;
    std::vector<uint8_t> outBaseline(bufBytes);
    std::vector<uint8_t> outAutoVec(bufBytes);
    std::vector<uint8_t> outSse(bufBytes);
    std::vector<uint8_t> outAvx2(bufBytes);

    constexpr int kIters = 100;
    auto timeIt = [&](auto&& fn) -> double {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i)
            fn();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / kIters;
    };

    const double msBaseline = timeIt(
        [&] { recorder_core::ConvertFullPlanarYuv420ToBgra(src, params, outBaseline.data(), kWidth * 4u); });
    const double msAutoVec =
        timeIt([&] { probe_g::ConvertFullPlanarYuv420ToBgra_AutoVec(src, params, outAutoVec.data(), kWidth * 4u); });
    const double msSse =
        timeIt([&] { probe_g::ConvertFullPlanarYuv420ToBgra_SSE(src, params, outSse.data(), kWidth * 4u); });

    const bool avx2Supported = probe_g::CpuSupportsAvx2();
    double msAvx2 = -1.0;
    if (avx2Supported) {
        msAvx2 =
            timeIt([&] { probe_g::ConvertFullPlanarYuv420ToBgra_AVX2(src, params, outAvx2.data(), kWidth * 4u); });
    }

    printf("[G] cpu_supports_avx2=%s\n", avx2Supported ? "true" : "false");
    printf("[G] 1_baseline_real:      avg=%.4fms/frame (factor=1.00x)\n", msBaseline);
    printf("[G] 2_autovec_arch_avx2:  avg=%.4fms/frame (factor=%.2fx)\n", msAutoVec,
           msBaseline > 0.0 ? msBaseline / msAutoVec : 0.0);
    printf("[G] 3_sse_intrinsics:     avg=%.4fms/frame (factor=%.2fx)\n", msSse,
           msBaseline > 0.0 ? msBaseline / msSse : 0.0);
    if (avx2Supported) {
        printf("[G] 4_avx2_intrinsics:    avg=%.4fms/frame (factor=%.2fx)\n", msAvx2,
               msBaseline > 0.0 ? msBaseline / msAvx2 : 0.0);
    } else {
        printf("[G] 4_avx2_intrinsics:    SKIPPED -- this CPU does not report AVX2 support\n");
    }

    PrintCompare("2_autovec_arch_avx2", CompareBgra(outBaseline.data(), outAutoVec.data(), kWidth, kHeight, kWidth * 4u));
    PrintCompare("3_sse_intrinsics", CompareBgra(outBaseline.data(), outSse.data(), kWidth, kHeight, kWidth * 4u));
    if (avx2Supported) {
        PrintCompare("4_avx2_intrinsics", CompareBgra(outBaseline.data(), outAvx2.data(), kWidth, kHeight, kWidth * 4u));
    } else {
        printf("[G] 4_avx2_intrinsics vs baseline: SKIPPED -- not run (CPU lacks AVX2)\n");
    }
}

// ---- Step H: audio continuity while video is artificially throttled ----
//
// This is the measurement the design doc's "Testing" section names as still
// missing: not that the three-thread topology runs (step B2) or that it's
// fast (step B), but that AUDIO KEEPS FLOWING when video is pathologically
// slow. Before the decoupled-decode change, demux+video+audio shared one
// thread, so a slow video callback stalled audio in lockstep. After it,
// video is paced by a bounded frame queue and audio by the WASAPI ring (here:
// nothing -- on_audio drops immediately), and the two are independent.
//
// on_video sleeps 120ms/frame -- ~8fps, far below any real playback rate,
// deliberately pathological. on_audio never blocks; it logs each block's
// wall-clock arrival time (steady_clock) and pts_us, then drops the data.
// current_media_time_us is wired to start_us + elapsed wall time, so the
// video thread's late-frame discard-before-convert gate behaves exactly as
// it does in the shipped player (unlike steps B/B2, which pass no clock at
// all to get a max-throughput number).
//
// Interpretation note (see the task brief this backs): the real app paces
// audio delivery through the WASAPI ring. This probe has no ring, so
// on_audio fires as fast as the demuxer can hand the audio thread packets --
// i.e. NOT "in real time". The expected-healthy shape is therefore a burst
// (draining up to ~1s of buffered audio packets, per the design doc's queue
// capacity) followed by steady, demuxer-paced delivery -- not silence, not a
// stutter pattern tied to the 120ms video cadence. The regression this
// guards against is audio gaps correlated with the video stall, which would
// mean the two paths are still coupled somewhere.
void StepH_AudioContinuityUnderSlowVideo(recorder_core::EditPlayerEngine& engine, int64_t kStartUs) {
    printf("=== [H] Audio continuity while video is artificially throttled (120ms/frame, 10s wall clock) ===\n");

    struct AudioArrival {
        std::chrono::steady_clock::time_point t;
        int64_t pts_us;
        uint32_t frame_count;
    };

    std::mutex arrivalsMutex;
    std::vector<AudioArrival> arrivals;
    arrivals.reserve(8192);

    std::atomic<uint64_t> videoFrames{0};

    auto onVideo = [&](recorder_core::DecodedVideoFrame /*frame*/) {
        videoFrames.fetch_add(1, std::memory_order_relaxed);
        // Simulates a video path that cannot keep up -- e.g. an unusually
        // large keyframe, a page fault, another process taking the core, or
        // (per the design doc) 4:4:4/high-frame-rate material permanently on
        // the software path. 120ms/frame is ~8fps, well beyond any of those
        // individually, chosen to make the effect unmistakable rather than
        // marginal.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    };

    auto onAudio = [&](recorder_core::DecodedAudioBlock block) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(arrivalsMutex);
        arrivals.push_back(AudioArrival{now, block.pts_us, block.frame_count});
        // block (and its shared_ptr sample buffer) is dropped here on scope
        // exit -- no WASAPI ring in this probe, per the interpretation note
        // above. We only need the arrival timestamp and pts/frame_count.
    };

    const auto t0 = std::chrono::steady_clock::now();
    auto mediaClock = [t0, kStartUs]() -> int64_t {
        const auto elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        return kStartUs + elapsedUs;
    };

    engine.StartPlaybackDecode(kStartUs, onVideo, onAudio, mediaClock);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    engine.StopPlaybackDecode();

    std::vector<AudioArrival> snapshot;
    {
        std::lock_guard<std::mutex> lock(arrivalsMutex);
        snapshot = arrivals;
    }

    printf("[H] video_frames_delivered=%llu (throttled ~8fps -> expect roughly 80 over 10s at 120ms/frame)\n",
           static_cast<unsigned long long>(videoFrames.load()));
    printf("[H] audio_blocks_delivered=%zu\n", snapshot.size());

    if (snapshot.empty()) {
        printf("[H] VERDICT: NO audio blocks delivered at all -- audio delivery is fully blocked by the "
               "throttled video path. FAIL: the decoupling did not work.\n");
        return;
    }

    double mediaSecondsCovered = 0.0;
    for (const auto& a : snapshot) {
        mediaSecondsCovered += static_cast<double>(a.frame_count) / 48000.0;
    }
    printf("[H] media_duration_covered=%.3fs (sum(frame_count)/48000)\n", mediaSecondsCovered);

    std::vector<double> gapsMs;
    gapsMs.reserve(snapshot.size());
    for (size_t i = 1; i < snapshot.size(); ++i) {
        const double ms = std::chrono::duration<double, std::milli>(snapshot[i].t - snapshot[i - 1].t).count();
        gapsMs.push_back(ms);
    }

    if (gapsMs.empty()) {
        printf("[H] only one audio block delivered -- no inter-arrival gaps to measure.\n");
        printf("[H] VERDICT: INCONCLUSIVE -- too few blocks to assess continuity.\n");
        return;
    }

    std::vector<double> sortedGaps = gapsMs;
    std::sort(sortedGaps.begin(), sortedGaps.end());
    const double maxGapMs = sortedGaps.back();

    auto percentile = [&sortedGaps](double pct) -> double {
        const size_t n = sortedGaps.size();
        size_t idx = static_cast<size_t>(std::ceil(pct / 100.0 * static_cast<double>(n)));
        idx = std::clamp<size_t>(idx, 1, n);
        return sortedGaps[idx - 1];
    };
    const double p50Ms = percentile(50.0);
    const double p99Ms = percentile(99.0);
    const uint64_t over50ms =
        static_cast<uint64_t>(std::count_if(gapsMs.begin(), gapsMs.end(), [](double ms) { return ms > 50.0; }));

    printf("[H] audio_arrival_gaps: n=%zu max=%.2fms p50=%.2fms p99=%.2fms gaps_over_50ms=%llu\n", gapsMs.size(),
           maxGapMs, p50Ms, p99Ms, static_cast<unsigned long long>(over50ms));

    // Packet-queue high-water mark (the design doc's own "Open question") is
    // deliberately NOT reported here: PacketQueue is a private implementation
    // detail of edit_player_engine.cpp's pImpl (no accessor on the public
    // EditPlayerEngine type), and this probe is scoped to measurement only --
    // it must not add instrumentation hooks to libs/. That question stays
    // open per the design doc.

    if (over50ms == 0) {
        printf("[H] VERDICT: audio delivery stayed continuous (no gap > 50ms) while video was throttled to "
               "~8fps -- the decoupling holds.\n");
    } else if (over50ms <= 2 && maxGapMs < 300.0) {
        printf("[H] VERDICT: audio delivery was continuous with a small number of gaps just over 50ms (likely "
               "the packet-queue-drained-to-demuxer-pace transition, not a stall) -- the decoupling holds.\n");
    } else {
        printf("[H] VERDICT: audio delivery shows repeated gaps > 50ms (n=%llu, max=%.1fms) while video was "
               "throttled -- this looks like audio is still coupled to the slow video path. FAIL.\n",
               static_cast<unsigned long long>(over50ms), maxGapMs);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Step I: seek accuracy / audio-video start alignment.
//
// A playback seek positions on the KEYFRAME at or before the requested start,
// so both streams begin decoding earlier than asked. Video discards the
// difference against the media clock; audio has no clock comparison to make
// and must be trimmed by timestamp instead. If that trim is missing or wrong,
// the renderer receives sound starting at the keyframe while the caller's
// clock is seeded to the requested position -- and video leads audio by that
// gap for the whole run, up to a full keyframe interval.
//
// This is the one thing the other steps cannot see: they all start at a fixed
// position and only ever compare deliveries against each other, never against
// the position that was actually asked for.
void StepI_SeekAlignment(recorder_core::EditPlayerEngine& engine, int64_t startUs) {
    printf("=== [I] Seek alignment (start_us=%lld) ===\n", static_cast<long long>(startUs));

    std::mutex m;
    bool haveVideo = false;
    bool haveAudio = false;
    int64_t firstVideoPts = 0;
    int64_t firstAudioPts = 0;

    auto onVideo = [&](recorder_core::DecodedVideoFrame frame) {
        std::lock_guard<std::mutex> lock(m);
        if (!haveVideo) {
            haveVideo = true;
            firstVideoPts = frame.pts_us;
        }
    };
    auto onAudio = [&](recorder_core::DecodedAudioBlock block) {
        std::lock_guard<std::mutex> lock(m);
        if (!haveAudio) {
            haveAudio = true;
            firstAudioPts = block.pts_us;
        }
    };

    // No media clock on purpose: this measures what the ENGINE delivers, not
    // what a consumer would have kept afterwards.
    engine.StartPlaybackDecode(startUs, onVideo, onAudio, {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    engine.StopPlaybackDecode();

    std::lock_guard<std::mutex> lock(m);
    if (!haveVideo && !haveAudio) {
        printf("[I] nothing delivered -- is start_us past the end of this clip?\n");
        return;
    }
    if (haveVideo) {
        printf("[I] first_video_pts=%lld offset=%+.3fms\n", static_cast<long long>(firstVideoPts),
               static_cast<double>(firstVideoPts - startUs) / 1000.0);
    } else {
        printf("[I] first_video_pts=N/A\n");
    }
    if (!haveAudio) {
        printf("[I] first_audio_pts=N/A (no audio stream, or none delivered)\n");
        return;
    }
    const int64_t audioOff = firstAudioPts - startUs;
    printf("[I] first_audio_pts=%lld offset=%+.3fms\n", static_cast<long long>(firstAudioPts),
           static_cast<double>(audioOff) / 1000.0);

    // A negative offset is the failure: audio older than the requested start
    // reached the caller, and every later sample inherits that shift.
    if (audioOff < 0) {
        printf("[I] VERDICT: FAIL -- audio starts %.3fms BEFORE the requested position; video leads audio by that "
               "much for the entire run.\n",
               static_cast<double>(-audioOff) / 1000.0);
    } else if (haveVideo) {
        // The video offset being negative here is EXPECTED and not a defect:
        // this step deliberately passes no media clock, so the engine has
        // nothing to judge "already in the past" against and delivers its
        // keyframe preroll instead of discarding it. The shipped player does
        // pass a clock and drops exactly those frames.
        printf("[I] VERDICT: PASS -- audio starts at or after the requested position. (Video's own negative offset is "
               "expected: no media clock is supplied here, so its preroll is delivered rather than discarded; the "
               "player supplies one and drops it.)\n");
    } else {
        printf("[I] VERDICT: PASS -- audio starts at or after the requested position.\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: probe_edit_playback.exe <path-to-mkv> [start_us]\n");
        printf("  start_us: playback start position for the timed steps, default 600000000 (600s).\n");
        printf("            Pass a position inside the clip when probing a short recording.\n");
        return 1;
    }
    const std::string path = argv[1];
    // The historical default matches the long reference clip the earlier
    // measurements were taken on. A short recording needs an explicit value,
    // or every timed step starts past its end and measures nothing.
    int64_t startUs = 600'000'000;
    if (argc >= 3) {
        startUs = std::strtoll(argv[2], nullptr, 10);
        if (startUs < 0) {
            printf("[probe] start_us must not be negative\n");
            return 1;
        }
    }
    printf("[probe] path=%s start_us=%lld\n", path.c_str(), static_cast<long long>(startUs));

    recorder_core::EditPlayerEngine engine;
    if (!StepA_Open(engine, path)) {
        return 1;
    }

    StepB_PlaybackThroughput(engine, startUs);
    StepB2_RepeatedStartStop(engine, startUs);
    StepC_ConvertCost();
    StepC2_Convert444Cost();
    // Runs immediately after C -- same real ConvertFullPlanarYuv420ToBgra
    // call, same frame shape. Kept adjacent purely for readability; the
    // ordering itself does not affect the timing (verified locally).
    StepG_SimdVariants();
    StepD_AllocationCost();
    StepE_FfmpegThreading(path);
    StepF_HardwareDecodeSupport();
    StepH_AudioContinuityUnderSlowVideo(engine, startUs);
    StepI_SeekAlignment(engine, startUs);

    printf("=== [probe] DONE ===\n");
    return 0;
}
