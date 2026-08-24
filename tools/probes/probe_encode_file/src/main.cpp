// probe_encode_file — reads a Y4M reference clip, drives it through the real
// NvencVideoEncoder (the same class video_thread.cpp uses), and writes a raw
// elementary stream: Annex-B concatenation for H.264/HEVC (NVENC already
// emits start-coded NAL units for those two codecs), IVF framing for AV1
// (whose raw OBU output is not self-delimited the way Annex-B is). No muxing
// — ffmpeg can decode either output directly. Backs
// scripts/dev/encoder_quality_matrix.py. Never touches the ExoSnap
// application itself — this is a standalone CLI dev tool.
//
// Usage:
//   probe_encode_file --y4m clip.y4m --out out.h264 --vcodec h264 --preset p4 --rc cq --cq 24
//   probe_encode_file --y4m clip.y4m --out out.ivf  --vcodec av1  --preset p7 --rc vbr --bitrate 8000 --keyint 2
//
// Options:
//   --y4m       <path>   input YUV4MPEG2 file (8-bit 4:2:0 only)
//   --out       <path>   output elementary-stream file (.h264/.h265 Annex-B, .ivf for AV1)
//   --vcodec    av1|h264|hevc
//   --preset    p1..p7   NVENC speed/quality preset (default p4)
//   --rc        cq|vbr|cbr  rate-control mode (default cq)
//   --cq        <n>      CQ value 1-51, used when --rc cq (default 24)
//   --bitrate   <kbps>   target bitrate, used when --rc vbr|cbr (default 6000)
//   --keyint    <secs>   keyframe interval in seconds (default 2.0)
//   --bframes   <n>      accepted but NOT YET applied by the encoder — printed as such
//   --lookahead           accepted but NOT YET applied by the encoder — printed as such
//   --temporal-aq         accepted but NOT YET applied by the encoder — printed as such
//
// Exit code 0 on success; prints one summary line and returns 1 on any failure.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "elementary_stream_writer.h"
#include "nvenc_video_encoder.h"
#include "y4m_reader.h"
#include "yuv_convert.h"

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/color_metadata.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace exosnap::engine;
using Microsoft::WRL::ComPtr;

namespace {

struct Options {
    std::string y4m_path;
    std::string out_path;
    VideoCodec vcodec = VideoCodec::Av1;
    NvencPreset preset = NvencPreset::P4;
    RateControlMode rc = RateControlMode::ConstantQuality;
    uint32_t cq = 24;
    uint32_t bitrate_kbps = 6000;
    float keyint_secs = 2.0f;
    int bframes = 0;
    bool lookahead = false;
    bool temporal_aq = false;
};

bool ParseVideoCodec(const std::string& s, VideoCodec& out) {
    if (s == "av1") {
        out = VideoCodec::Av1;
        return true;
    }
    if (s == "h264") {
        out = VideoCodec::H264;
        return true;
    }
    if (s == "hevc" || s == "h265") {
        out = VideoCodec::Hevc;
        return true;
    }
    return false;
}

bool ParsePreset(const std::string& s, NvencPreset& out) {
    static const std::pair<const char*, NvencPreset> kPresets[] = {
        {"p1", NvencPreset::P1}, {"p2", NvencPreset::P2}, {"p3", NvencPreset::P3}, {"p4", NvencPreset::P4},
        {"p5", NvencPreset::P5}, {"p6", NvencPreset::P6}, {"p7", NvencPreset::P7},
    };
    for (const auto& [name, val] : kPresets) {
        if (s == name) {
            out = val;
            return true;
        }
    }
    return false;
}

bool ParseRateControl(const std::string& s, RateControlMode& out) {
    if (s == "cq") {
        out = RateControlMode::ConstantQuality;
        return true;
    }
    if (s == "vbr") {
        out = RateControlMode::VariableBitrate;
        return true;
    }
    if (s == "cbr") {
        out = RateControlMode::ConstantBitrate;
        return true;
    }
    return false;
}

bool ParseOptions(int argc, char** argv, Options& out, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto needValue = [&](std::string& target) -> bool {
            if (i + 1 >= argc) {
                err = "missing value for " + arg;
                return false;
            }
            target = argv[++i];
            return true;
        };

        if (arg == "--y4m") {
            if (!needValue(out.y4m_path))
                return false;
        } else if (arg == "--out") {
            if (!needValue(out.out_path))
                return false;
        } else if (arg == "--vcodec") {
            std::string v;
            if (!needValue(v) || !ParseVideoCodec(v, out.vcodec)) {
                err = "--vcodec requires av1|h264|hevc";
                return false;
            }
        } else if (arg == "--preset") {
            std::string v;
            if (!needValue(v) || !ParsePreset(v, out.preset)) {
                err = "--preset requires p1..p7";
                return false;
            }
        } else if (arg == "--rc") {
            std::string v;
            if (!needValue(v) || !ParseRateControl(v, out.rc)) {
                err = "--rc requires cq|vbr|cbr";
                return false;
            }
        } else if (arg == "--cq") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.cq = static_cast<uint32_t>(std::stoul(v));
        } else if (arg == "--bitrate") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.bitrate_kbps = static_cast<uint32_t>(std::stoul(v));
        } else if (arg == "--keyint") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.keyint_secs = std::stof(v);
        } else if (arg == "--bframes") {
            std::string v;
            if (!needValue(v)) {
                return false;
            }
            out.bframes = std::stoi(v);
        } else if (arg == "--lookahead") {
            out.lookahead = true;
        } else if (arg == "--temporal-aq") {
            out.temporal_aq = true;
        } else {
            err = "unknown option " + arg;
            return false;
        }
    }
    if (out.y4m_path.empty() || out.out_path.empty()) {
        err = "--y4m and --out are required";
        return false;
    }
    return true;
}

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

bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size < 0)
        return false;
    out.resize(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(out.data(), size);
    return static_cast<bool>(f) || f.eof();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!ParseOptions(argc, argv, opt, err)) {
        printf("[probe] argument error: %s\n", err.c_str());
        return 1;
    }

    printf("[probe] reading %s\n", opt.y4m_path.c_str());
    std::string fileData;
    if (!ReadWholeFile(opt.y4m_path, fileData)) {
        printf("[probe] failed to read %s\n", opt.y4m_path.c_str());
        return 1;
    }

    const auto header = ParseY4mHeader(fileData, err);
    if (!header.has_value()) {
        printf("[probe] y4m header error: %s\n", err.c_str());
        return 1;
    }
    printf("[probe] %ux%u @ %u/%u fps\n", header->width, header->height, header->fps_num, header->fps_den);

    printf("[probe] applied encoder fields: vcodec=%d preset=%d rc=%d cq=%u bitrate_kbps=%u keyint_secs=%.2f\n",
           static_cast<int>(opt.vcodec), static_cast<int>(opt.preset), static_cast<int>(opt.rc), opt.cq,
           opt.bitrate_kbps, opt.keyint_secs);
    if (opt.bframes != 0 || opt.lookahead || opt.temporal_aq) {
        printf("[probe] NOTE: --bframes/--lookahead/--temporal-aq were requested (bframes=%d lookahead=%d "
               "temporal_aq=%d) but NvencVideoEncoder has no setter for them yet — NOT applied. This run measures "
               "the baseline encoder only.\n",
               opt.bframes, opt.lookahead ? 1 : 0, opt.temporal_aq ? 1 : 0);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context))
        return 1;

    NvencVideoEncoder enc;
    enc.SetCodec(opt.vcodec);
    enc.SetPreset(opt.preset);
    enc.SetCq(opt.cq);
    enc.SetRateControl(opt.rc, opt.bitrate_kbps);
    enc.SetKeyframeIntervalSecs(opt.keyint_secs);
    enc.SetColor(ColorMetadata::Sdr709());

    if (!enc.Open(device.Get(), err)) {
        printf("[probe] Open failed: %s\n", err.c_str());
        return 1;
    }
    if (!enc.Configure(header->width, header->height, header->fps_num, header->fps_den, err)) {
        printf("[probe] Configure failed: %s\n", err.c_str());
        return 1;
    }

    constexpr int kSlotCount = 8;
    std::vector<ComPtr<ID3D11Texture2D>> textures(kSlotCount);
    for (int i = 0; i < kSlotCount; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = header->width;
        desc.Height = header->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device->CreateTexture2D(&desc, nullptr, textures[static_cast<size_t>(i)].GetAddressOf());
        if (FAILED(hr)) {
            printf("[probe] CreateTexture2D[%d] failed 0x%08lX\n", i, static_cast<unsigned long>(hr));
            return 1;
        }
        if (!enc.RegisterSlotTexture(i, textures[static_cast<size_t>(i)].Get(), err)) {
            printf("[probe] RegisterSlotTexture[%d] failed: %s\n", i, err.c_str());
            return 1;
        }
    }

    std::vector<EncodedVideoPacket> allPackets;
    std::vector<uint8_t> nv12;
    size_t offset = header->header_bytes;
    uint64_t frameIdx = 0;
    bool encodeError = false;

    // Per-frame encoder cost: the wait for a free input slot (the encoder's own
    // backpressure) plus submit and reap. Reading the Y4M, the I420->NV12
    // conversion and the CPU-side texture upload are this probe's work and have
    // no counterpart in the capture pipeline, so they sit between the two spans
    // and are excluded.
    std::vector<double> frameMs;

    for (;;) {
        const auto frame = ReadY4mFrame(fileData, offset, header->width, header->height, err);
        if (!frame.has_value()) {
            if (!err.empty()) {
                printf("[probe] frame %llu: %s\n", static_cast<unsigned long long>(frameIdx), err.c_str());
                encodeError = true;
            }
            break; // clean EOF or error — either way, stop reading
        }
        offset = frame->next_offset;

        const auto slotWaitStart = std::chrono::steady_clock::now();
        int32_t slot = enc.AcquireFreeSlot();
        if (slot < 0) {
            std::vector<EncodedVideoPacket> reaped;
            std::string rerr;
            enc.ReapCompleted(reaped, rerr, 50);
            for (auto& p : reaped)
                allPackets.push_back(std::move(p));
            slot = enc.AcquireFreeSlot();
            if (slot < 0) {
                printf("[probe] frame %llu: no free input slot even after ReapCompleted\n",
                       static_cast<unsigned long long>(frameIdx));
                encodeError = true;
                break;
            }
        }

        const double slotWaitMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - slotWaitStart).count();

        ConvertI420ToNv12(reinterpret_cast<const uint8_t*>(fileData.data()) + frame->data_offset, header->width,
                          header->height, nv12);
        context->UpdateSubresource(textures[static_cast<size_t>(slot)].Get(), 0, nullptr, nv12.data(), header->width,
                                   0);

        const uint64_t ptsNs =
            frameIdx * 1'000'000'000ull * header->fps_den / (header->fps_num == 0 ? 1 : header->fps_num);
        std::vector<EncodedVideoPacket> pkts;
        std::string encErr;
        const auto submitStart = std::chrono::steady_clock::now();
        if (!enc.EncodeFrame(slot, ptsNs, header->width, header->height, pkts, encErr)) {
            printf("[probe] frame %llu: EncodeFrame failed: %s\n", static_cast<unsigned long long>(frameIdx),
                   encErr.c_str());
            encodeError = true;
            break;
        }
        for (auto& p : pkts)
            allPackets.push_back(std::move(p));

        std::vector<EncodedVideoPacket> reaped;
        std::string rerr;
        if (!enc.ReapCompleted(reaped, rerr, 0)) {
            printf("[probe] frame %llu: ReapCompleted failed: %s\n", static_cast<unsigned long long>(frameIdx),
                   rerr.c_str());
            encodeError = true;
            break;
        }
        for (auto& p : reaped)
            allPackets.push_back(std::move(p));

        frameMs.push_back(
            slotWaitMs +
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitStart).count());
        ++frameIdx;
    }

    if (!encodeError) {
        std::vector<EncodedVideoPacket> flushed;
        std::string flushErr;
        if (!enc.Flush(flushed, flushErr)) {
            printf("[probe] Flush reported an error: %s\n", flushErr.c_str());
            encodeError = true;
        }
        for (auto& p : flushed)
            allPackets.push_back(std::move(p));
    }

    enc.Destroy();

    if (encodeError) {
        printf("[probe] RESULT: FAIL\n");
        return 1;
    }

    std::ofstream out(opt.out_path, std::ios::binary);
    if (!out) {
        printf("[probe] failed to open %s for writing\n", opt.out_path.c_str());
        return 1;
    }

    if (opt.vcodec == VideoCodec::Av1) {
        const auto fileHeader = BuildIvfFileHeader(header->width, header->height, header->fps_num, header->fps_den,
                                                   static_cast<uint32_t>(allPackets.size()));
        out.write(reinterpret_cast<const char*>(fileHeader.data()), static_cast<std::streamsize>(fileHeader.size()));
        for (size_t i = 0; i < allPackets.size(); ++i) {
            const auto& pkt = allPackets[i];
            const auto frameHeader = BuildIvfFrameHeader(static_cast<uint32_t>(pkt.bytes.size()), i);
            out.write(reinterpret_cast<const char*>(frameHeader.data()),
                      static_cast<std::streamsize>(frameHeader.size()));
            out.write(reinterpret_cast<const char*>(pkt.bytes.data()), static_cast<std::streamsize>(pkt.bytes.size()));
        }
    } else {
        // H.264/HEVC: NVENC already emits Annex-B start-coded NAL units —
        // straight concatenation is a valid, directly ffprobe-decodable
        // elementary stream.
        for (const auto& pkt : allPackets)
            out.write(reinterpret_cast<const char*>(pkt.bytes.data()), static_cast<std::streamsize>(pkt.bytes.size()));
    }
    out.close();

    size_t totalBytes = 0;
    for (const auto& pkt : allPackets)
        totalBytes += pkt.bytes.size();

    printf("[probe] frames encoded: %zu, total bytes: %zu, wrote %s\n", allPackets.size(), totalBytes,
           opt.out_path.c_str());
    if (!frameMs.empty()) {
        std::vector<double> sorted = frameMs;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](double pct) {
            const size_t idx = static_cast<size_t>(pct / 100.0 * static_cast<double>(sorted.size() - 1) + 0.5);
            return sorted[idx < sorted.size() ? idx : sorted.size() - 1];
        };
        double total = 0.0;
        for (double v : frameMs)
            total += v;
        const double mean = total / static_cast<double>(frameMs.size());
        printf("[probe] TIMING frames=%zu mean=%.3fms p50=%.3fms p99=%.3fms max=%.3fms sustained=%.1ffps\n",
               frameMs.size(), mean, at(50.0), at(99.0), sorted.back(), mean > 0.0 ? 1000.0 / mean : 0.0);
    }
    printf("[probe] RESULT: PASS\n");
    return 0;
}
