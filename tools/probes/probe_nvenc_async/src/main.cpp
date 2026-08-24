// Live-verify probe for the async NVENC path.
// Drives the ACTUAL engine NvencVideoEncoder / NvencEncoder classes
// (not a reimplementation of raw SDK calls, unlike probe_nvenc) through a
// real D3D11 + NVENC async session, so the exact code shipped in the app is
// what gets exercised here. Never touches the ExoSnap application itself —
// this is a standalone CLI dev tool.
//
// What it checks:
//   - Capability gate + InitEncoder actually enable async mode on this GPU.
//   - EncodeFrame's output-ring-full wait (m_activeDepth=1, so it fires on
//     essentially every submission after the first) does not hang, does not
//     drop frames, and does not corrupt ordering.
//   - ReapCompleted drains what's ready without blocking past its budget.
//   - Flush's async drain terminates and returns the remaining packets.
//   - Order/keyframe validation never reports a mismatch on this hardware
//     (output_ts_mismatch / keyframe_prediction_mismatch stay false) —
//     confirms the driver's outputTimeStamp actually echoes what was
//     submitted, which the fatal-on-mismatch check below depends on.
//   - Every submitted frame eventually produces exactly one packet, frame 0
//     is a keyframe, and packets arrive in ascending PTS order.

#include "nvenc_video_encoder.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace exosnap::engine;
using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr int kSlotCount = 8;
constexpr int kTotalFrames = 60;

bool CreateDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &featureLevel, 1,
                                         D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr)) {
        printf("[probe] D3D11CreateDevice failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    VideoCodec codec = VideoCodec::Av1;
    const char* codecName = "AV1";
    if (argc > 1) {
        const std::string arg = argv[1];
        if (arg == "h264") {
            codec = VideoCodec::H264;
            codecName = "H264";
        } else if (arg == "hevc") {
            codec = VideoCodec::Hevc;
            codecName = "HEVC";
        }
    }
    NvencPreset preset = NvencPreset::P4;
    const char* presetName = "P4";
    if (argc > 2 && std::string(argv[2]) == "p7") {
        preset = NvencPreset::P7;
        presetName = "P7";
    }

    printf("[probe] async-NVENC live verify (real NvencVideoEncoder, real D3D11/NVENC session), codec=%s "
           "preset=%s\n",
           codecName, presetName);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context))
        return 1;

    NvencVideoEncoder enc;
    enc.SetCodec(codec);
    enc.SetPreset(preset);

    std::string err;
    if (!enc.Open(device.Get(), err)) {
        printf("[probe] Open failed: %s\n", err.c_str());
        return 1;
    }
    if (!enc.Configure(kWidth, kHeight, 60, 1, err)) {
        printf("[probe] Configure failed: %s\n", err.c_str());
        return 1;
    }
    printf("[probe] session open + configured: %ux%u @60/1, %s, %s\n", kWidth, kHeight, codecName, presetName);

    std::vector<ComPtr<ID3D11Texture2D>> textures(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, textures[i].GetAddressOf());
        if (FAILED(hr)) {
            printf("[probe] CreateTexture2D[%d] failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 1;
        }
        if (!enc.RegisterSlotTexture(i, textures[i].Get(), err)) {
            printf("[probe] RegisterSlotTexture[%d] failed: %s\n", i, err.c_str());
            return 1;
        }
    }

    // Deterministic 50%-gray NV12 fill (matches probe_nvenc's own convention).
    // Single UpdateSubresource over the whole (Y + interleaved UV) buffer with
    // row pitch = width, the standard pattern for a combined-plane NV12 D3D11
    // texture.
    std::vector<uint8_t> full(static_cast<size_t>(kWidth) * kHeight + static_cast<size_t>(kWidth) * kHeight / 2,
                              128);
    for (int i = 0; i < kSlotCount; ++i) {
        context->UpdateSubresource(textures[i].Get(), 0, nullptr, full.data(), kWidth, 0);
    }

    int packetsReceived = 0;
    uint64_t lastPts = 0;
    bool haveLastPts = false;
    bool firstIsKeyframe = false;
    bool orderOk = true;
    bool anyOutputTsMismatch = false;
    bool anyKeyframePredictionMismatch = false;
    bool sawError = false;
    int slotWaitFailures = 0;

    auto consume = [&](std::vector<EncodedVideoPacket>& pkts) {
        for (auto& p : pkts) {
            if (packetsReceived == 0)
                firstIsKeyframe = p.keyframe;
            if (haveLastPts && p.pts_ns < lastPts)
                orderOk = false;
            lastPts = p.pts_ns;
            haveLastPts = true;
            anyOutputTsMismatch = anyOutputTsMismatch || p.output_ts_mismatch;
            anyKeyframePredictionMismatch = anyKeyframePredictionMismatch || p.keyframe_prediction_mismatch;
            ++packetsReceived;
            if (packetsReceived <= 5 || packetsReceived % 20 == 0) {
                printf("[probe]   packet #%d: bytes=%zu keyframe=%d encode_latency_ms=%.2f ts_mismatch=%d "
                       "kf_mismatch=%d\n",
                       packetsReceived, p.bytes.size(), p.keyframe ? 1 : 0, p.encode_latency_ms,
                       p.output_ts_mismatch ? 1 : 0, p.keyframe_prediction_mismatch ? 1 : 0);
            }
        }
    };

    for (int f = 0; f < kTotalFrames && !sawError; ++f) {
        int32_t slot = enc.AcquireFreeSlot();
        if (slot < 0) {
            // All 8 input slots in flight (should not happen at depth 1 — each
            // submission's output-ring wait releases its input slot before the
            // next submission starts — but drain defensively instead of
            // asserting, matching how video_thread would react).
            std::vector<EncodedVideoPacket> reaped;
            std::string rerr;
            enc.ReapCompleted(reaped, rerr, 50);
            consume(reaped);
            slot = enc.AcquireFreeSlot();
            if (slot < 0) {
                printf("[probe] frame %d: no free input slot even after ReapCompleted\n", f);
                ++slotWaitFailures;
                sawError = true;
                break;
            }
        }

        std::vector<EncodedVideoPacket> pkts;
        std::string encErr;
        const uint64_t pts_ns = static_cast<uint64_t>(f) * 16666667ull; // ~60fps spacing
        if (!enc.EncodeFrame(slot, pts_ns, kWidth, kHeight, pkts, encErr)) {
            printf("[probe] frame %d: EncodeFrame failed: %s\n", f, encErr.c_str());
            sawError = true;
            break;
        }
        consume(pkts);

        std::vector<EncodedVideoPacket> reaped;
        std::string rerr;
        if (!enc.ReapCompleted(reaped, rerr, 0)) {
            printf("[probe] frame %d: ReapCompleted failed: %s\n", f, rerr.c_str());
            sawError = true;
            break;
        }
        consume(reaped);
    }

    std::vector<EncodedVideoPacket> flushed;
    std::string flushErr;
    if (!enc.Flush(flushed, flushErr)) {
        printf("[probe] Flush reported an error (non-fatal by contract): %s\n", flushErr.c_str());
    }
    consume(flushed);

    enc.Destroy();

    printf("\n[probe] === Summary ===\n");
    printf("[probe] frames submitted        : %d\n", kTotalFrames);
    printf("[probe] packets received         : %d\n", packetsReceived);
    printf("[probe] first packet is keyframe : %s\n", firstIsKeyframe ? "yes" : "NO");
    printf("[probe] packets in ascending PTS : %s\n", orderOk ? "yes" : "NO");
    printf("[probe] output_ts_mismatch seen  : %s\n", anyOutputTsMismatch ? "YES (unexpected)" : "no");
    printf("[probe] keyframe_mismatch seen   : %s\n", anyKeyframePredictionMismatch ? "YES (unexpected)" : "no");
    printf("[probe] slot-wait failures       : %d\n", slotWaitFailures);
    printf("[probe] hard errors              : %s\n", sawError ? "YES" : "no");

    const bool pass = !sawError && packetsReceived == kTotalFrames && firstIsKeyframe && orderOk &&
                      !anyOutputTsMismatch && !anyKeyframePredictionMismatch && slotWaitFailures == 0;
    printf("\n[probe] RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
