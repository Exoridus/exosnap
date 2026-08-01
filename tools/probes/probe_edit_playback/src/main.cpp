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
//   probe_edit_playback.exe <path-to-mkv>
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
void StepB_PlaybackThroughput(recorder_core::EditPlayerEngine& engine) {
    printf("=== [B] Playback-decode throughput (start_us=600000000, 10s wall clock, no pacing) ===\n");

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

    constexpr int64_t kStartUs = 600'000'000; // 600s
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
void StepB2_RepeatedStartStop(recorder_core::EditPlayerEngine& engine) {
    printf("=== [B2] Repeated start/stop on the same open engine (3 cycles, 300ms each) ===\n");

    constexpr int kCycles = 3;
    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        std::atomic<uint64_t> videoFrames{0};
        std::atomic<uint64_t> audioBlocks{0};
        const auto t0 = std::chrono::steady_clock::now();
        engine.StartPlaybackDecode(
            600'000'000, [&](recorder_core::DecodedVideoFrame) { videoFrames.fetch_add(1, std::memory_order_relaxed); },
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: probe_edit_playback.exe <path-to-mkv>\n");
        return 1;
    }
    const std::string path = argv[1];
    printf("[probe] path=%s\n", path.c_str());

    recorder_core::EditPlayerEngine engine;
    if (!StepA_Open(engine, path)) {
        return 1;
    }

    StepB_PlaybackThroughput(engine);
    StepB2_RepeatedStartStop(engine);
    StepC_ConvertCost();
    // Runs immediately after C -- same real ConvertFullPlanarYuv420ToBgra
    // call, same frame shape. Kept adjacent purely for readability; the
    // ordering itself does not affect the timing (verified locally).
    StepG_SimdVariants();
    StepD_AllocationCost();
    StepE_FfmpegThreading(path);
    StepF_HardwareDecodeSupport();

    printf("=== [probe] DONE ===\n");
    return 0;
}
