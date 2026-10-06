// Reads an 8-bit 4:2:0 Y4M reference through the production NVENC encoder.
// Elementary output is Annex-B for H.264/HEVC and IVF for AV1. Optional
// Matroska and MP4 output exercises the production writer and remux paths.
// Used by exo-dev encoder-quality-matrix without starting the application.
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
//   --vfr               configure the VFR submission regime
//   --bframes   <n>      explicit B-frame count, capability-validated by the encoder
//   --b-ref     off|each|middle, --lookahead, --lookahead-depth <n>
//   --spatial-aq, --temporal-aq, --multipass single|quarter|full
//
// Exit code 0 on success; prints one summary line and returns 1 on any failure.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "codec_private.h"
#include "elementary_stream_writer.h"
#include "matroska_stream_writer.h"
#include "nvenc_video_encoder.h"
#include "y4m_reader.h"
#include "yuv_convert.h"
#include <capability/adapter_capability.h>
#include <capability/adapter_enum.h>

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/color_metadata.h>
#include <exosnap/engine/mp4_remuxer.h>

extern "C" {
#include <libavutil/mathematics.h>
}

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
    std::string mkv_path;
    std::string mp4_path;
    bool capabilities = false;
    bool constant_frame_rate = true;
    VideoCodec vcodec = VideoCodec::Av1;
    NvencPreset preset = NvencPreset::P4;
    RateControlMode rc = RateControlMode::ConstantQuality;
    uint32_t cq = 24;
    uint32_t bitrate_kbps = 6000;
    float keyint_secs = 2.0f;
    NvencTuning tuning;
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

        if (arg == "--vfr") {
            out.constant_frame_rate = false;
        } else if (arg == "--capabilities") {
            out.capabilities = true;
        } else if (arg == "--mp4-output") {
            if (!needValue(out.mp4_path))
                return false;
        } else if (arg == "--mkv-output") {
            if (!needValue(out.mkv_path))
                return false;
        } else if (arg == "--y4m") {
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
            const auto value = std::stoul(v);
            if (value > 31 || v.front() == '-') {
                err = "--bframes requires 0..31";
                return false;
            }
            out.tuning.bframes = static_cast<uint32_t>(value);
        } else if (arg == "--b-ref") {
            std::string v;
            if (!needValue(v))
                return false;
            if (v == "off")
                out.tuning.b_ref_mode = NvencBRefMode::Off;
            else if (v == "each")
                out.tuning.b_ref_mode = NvencBRefMode::Each;
            else if (v == "middle")
                out.tuning.b_ref_mode = NvencBRefMode::Middle;
            else {
                err = "--b-ref requires off|each|middle";
                return false;
            }
        } else if (arg == "--lookahead-depth") {
            std::string v;
            if (!needValue(v))
                return false;
            const auto value = std::stoul(v);
            if (value < 1 || value > 31) {
                err = "--lookahead-depth requires 1..31";
                return false;
            }
            out.tuning.lookahead_depth = static_cast<uint32_t>(value);
        } else if (arg == "--spatial-aq") {
            out.tuning.spatial_aq = true;
        } else if (arg == "--multipass") {
            std::string v;
            if (!needValue(v))
                return false;
            if (v == "single")
                out.tuning.multipass = NvencMultipass::SinglePass;
            else if (v == "quarter")
                out.tuning.multipass = NvencMultipass::QuarterResolution;
            else if (v == "full")
                out.tuning.multipass = NvencMultipass::FullResolution;
            else {
                err = "--multipass requires single|quarter|full";
                return false;
            }
        } else if (arg == "--lookahead") {
            out.tuning.lookahead = true;
        } else if (arg == "--temporal-aq") {
            out.tuning.temporal_aq = true;
        } else {
            err = "unknown option " + arg;
            return false;
        }
    }
    if (!out.capabilities && (out.y4m_path.empty() || out.out_path.empty())) {
        err = "--y4m and --out are required";
        return false;
    }
    if (!out.mp4_path.empty() && (out.mkv_path.empty() || out.vcodec == VideoCodec::Av1)) {
        err = "--mp4-output requires --mkv-output and H.264 or HEVC";
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

bool WriteMatroska(const Options& opt, const Y4mHeader& header, const std::vector<EncodedVideoPacket>& packets) {
    if (packets.empty())
        return false;
    MatroskaStreamConfig config;
    config.output_path = opt.mkv_path;
    config.encode_width = header.width;
    config.encode_height = header.height;
    config.frame_rate_num = header.fps_num;
    config.frame_rate_den = header.fps_den;
    config.color = ColorMetadata::Sdr709();
    const auto& first = packets.front().bytes;
    std::vector<uint8_t> parameterSets;
    if (opt.vcodec == VideoCodec::H264) {
        config.video_codec_id = "V_MPEG4/ISO/AVC";
        if (!annexb::ExtractH264SpsAndPps(first.data(), first.size(), parameterSets) ||
            !annexb::BuildAvccFromAnnexBSpsAndPps(parameterSets, config.video_codec_private))
            return false;
    } else if (opt.vcodec == VideoCodec::Hevc) {
        config.video_codec_id = "V_MPEGH/ISO/HEVC";
        if (!annexb::ExtractHevcVpsSpsPps(first.data(), first.size(), parameterSets) ||
            !annexb::BuildHvccFromAnnexBVpsSpsPps(parameterSets, config.video_codec_private))
            return false;
    } else {
        config.video_codec_id = "V_AV1";
        char reason[256]{};
        if (!codec_private::DeriveAv1CodecPrivate(first.data(), first.size(), config.video_codec_private, reason,
                                                  sizeof(reason)))
            return false;
    }
    MatroskaStreamWriter writer;
    if (!writer.Open(config)) {
        printf("[probe] Matroska Open: %s\n", writer.error().c_str());
        return false;
    }
    for (const auto& packet : packets) {
        MuxPacket mux;
        mux.track_num = 1;
        mux.pts_ns = packet.pts_ns;
        mux.dts_ns = packet.dts_ns;
        mux.is_key = packet.keyframe;
        if (opt.vcodec == VideoCodec::H264) {
            if (!annexb::ConvertAnnexBToAvcc(packet.bytes.data(), packet.bytes.size(), mux.bytes))
                return false;
        } else if (opt.vcodec == VideoCodec::Hevc) {
            if (!annexb::ConvertAnnexBToHevcSample(packet.bytes.data(), packet.bytes.size(), mux.bytes))
                return false;
        } else {
            mux.bytes = packet.bytes;
        }
        if (!writer.Push(std::move(mux))) {
            printf("[probe] Matroska Push: %s\n", writer.error().c_str());
            return false;
        }
    }
    if (!writer.Finalize()) {
        printf("[probe] Matroska Finalize: %s\n", writer.error().c_str());
        return false;
    }
    return true;
}

void PrintCapabilities() {
    for (const auto& adapter : exosnap::capability::EnumerateAdapters()) {
        const auto caps = exosnap::capability::ProbeAdapterEncoderCapability(adapter);
        printf("[probe] ADAPTER name=%s probed=%d backend=%s\n", adapter.name.c_str(), caps.probed ? 1 : 0,
               caps.backend_label.c_str());
        printf("[probe] CAPS codec=h264 supported=%d max_bframes=%d b_ref=%d lookahead=%d temporal_aq=%d\n", caps.h264,
               caps.max_bframes_h264, caps.bframe_ref_mode_h264, caps.lookahead_h264, caps.temporal_aq_h264);
        printf("[probe] CAPS codec=hevc supported=%d max_bframes=%d b_ref=%d lookahead=%d temporal_aq=%d\n", caps.hevc,
               caps.max_bframes_hevc, caps.bframe_ref_mode_hevc, caps.lookahead_hevc, caps.temporal_aq_hevc);
        printf("[probe] CAPS codec=av1 supported=%d max_bframes=%d b_ref=%d lookahead=%d temporal_aq=%d\n", caps.av1,
               caps.max_bframes_av1, caps.bframe_ref_mode_av1, caps.lookahead_av1, caps.temporal_aq_av1);
    }
}
} // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    bool parsed = false;
    try {
        parsed = ParseOptions(argc, argv, opt, err);
    } catch (const std::exception& error) {
        err = error.what();
    }
    if (!parsed) {
        printf("[probe] argument error: %s\n", err.c_str());
        return 1;
    }

    if (opt.capabilities) {
        PrintCapabilities();
        return 0;
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

    printf("[probe] applied encoder fields: vcodec=%d preset=%d rc=%d cq=%u bitrate_kbps=%u keyint_secs=%.2f cfr=%d\n",
           static_cast<int>(opt.vcodec), static_cast<int>(opt.preset), static_cast<int>(opt.rc), opt.cq,
           opt.bitrate_kbps, opt.keyint_secs, opt.constant_frame_rate ? 1 : 0);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (!CreateDevice(device, context))
        return 1;

    NvencVideoEncoder enc;
    enc.SetCodec(opt.vcodec);
    opt.tuning.preset = opt.preset;
    enc.SetTuning(opt.tuning);
    enc.SetCq(opt.cq);
    enc.SetRateControl(opt.rc, opt.bitrate_kbps);
    enc.SetKeyframeIntervalSecs(opt.keyint_secs);
    enc.SetConstantFrameRate(opt.constant_frame_rate);
    enc.SetColor(ColorMetadata::Sdr709());

    if (!enc.Open(device.Get(), err)) {
        printf("[probe] Open failed: %s\n", err.c_str());
        return 1;
    }
    if (!enc.Configure(header->width, header->height, header->fps_num, header->fps_den, err)) {
        printf("[probe] Configure failed: %s\n", err.c_str());
        return 1;
    }

    const auto init = enc.GetInitInfo();
    printf("[probe] RESOLVED bframes=%u b_ref=%.*s lookahead=%u spatial_aq=%d temporal_aq=%d multipass=%.*s slots=%u "
           "output_depth=%u\n",
           init.bframes, static_cast<int>(init.backend_b_ref_mode.size()), init.backend_b_ref_mode.data(),
           init.lookahead_frames, init.spatial_aq ? 1 : 0, init.temporal_aq ? 1 : 0,
           static_cast<int>(init.backend_multipass.size()), init.backend_multipass.data(), init.input_slots,
           init.output_depth);
    const int kSlotCount = enc.SlotCount();
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
    uint64_t peakBacklog = 0;

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
        peakBacklog = (std::max)(peakBacklog, enc.PendingFrames());
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

    const auto pendingAfterFlush = enc.PendingFrames();
    enc.Destroy();
    if (allPackets.size() != frameIdx || pendingAfterFlush != 0) {
        printf("[probe] incomplete drain: submitted=%llu packets=%zu pending=%llu\n",
               static_cast<unsigned long long>(frameIdx), allPackets.size(),
               static_cast<unsigned long long>(pendingAfterFlush));
        encodeError = true;
    }
    printf("[probe] BACKLOG peak=%llu after_flush=%llu\n", static_cast<unsigned long long>(peakBacklog),
           static_cast<unsigned long long>(pendingAfterFlush));

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
            // IVF timestamps remain presentation ticks while packets arrive in decode order.
            const auto ptsTicks =
                av_rescale_q(static_cast<int64_t>(pkt.pts_ns), AVRational{1, 1000000000},
                             AVRational{static_cast<int>(header->fps_den), static_cast<int>(header->fps_num)});
            const auto frameHeader =
                BuildIvfFrameHeader(static_cast<uint32_t>(pkt.bytes.size()), static_cast<uint64_t>(ptsTicks));
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
    if (!opt.mkv_path.empty() && !WriteMatroska(opt, *header, allPackets)) {
        printf("[probe] Matroska output failed\n");
        return 1;
    }
    if (!opt.mp4_path.empty()) {
        const auto result = RemuxToProgressiveMp4(opt.mkv_path, opt.mp4_path);
        if (!result.success) {
            printf("[probe] MP4 remux failed: %s\n", result.message.c_str());
            return 1;
        }
    }
    std::vector<double> outputLatency;
    for (const auto& packet : allPackets) {
        if (packet.encode_latency_ms >= 0)
            outputLatency.push_back(packet.encode_latency_ms);
    }
    if (!outputLatency.empty()) {
        std::sort(outputLatency.begin(), outputLatency.end());
        const auto percentile = [&outputLatency](double fraction) {
            return outputLatency[static_cast<size_t>(fraction * static_cast<double>(outputLatency.size() - 1))];
        };
        printf("[probe] ENCODER_LATENCY samples=%zu p50=%.3fms p99=%.3fms max=%.3fms\n", outputLatency.size(),
               percentile(0.50), percentile(0.99), outputLatency.back());
    }
    printf("[probe] RESULT: PASS\n");
    return 0;
}
