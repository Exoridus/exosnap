// probe_record — headless recording probe driving the real RecorderSession pipeline.
//
// Usage:
//   probe_record --list
//   probe_record --container mkv --vcodec hevc --acodec aac --seconds 4 --out C:\tmp\x.mkv
//
// Options:
//   --list                 enumerate capture targets and exit
//   --target  <index>      capture target index (default 0 = first monitor)
//   --container mkv|mp4|webm
//   --vcodec  av1|h264|hevc
//   --acodec  opus|aac|pcm|flac|none
//   --bitdepth 8|10        encoder bit depth (default 8; 10 = HEVC Main10 / AV1 10-bit, P010)
//   --chroma  420|444      chroma subsampling (default 420; 444 = 8-bit H.264/HEVC only,
//                          AYUV input + High 4:4:4 / HEVC FREXT profile)
//   --range   full|limited Y'CbCr quantization range (default limited = 16-235; full = 0-255)
//   --hdrmode off|tonemap|hdr10  HDR handling (default tonemap). hdr10 keeps the native
//                          PQ/BT.2020 signal when the target display is HDR-active + the
//                          codec is HEVC/AV1 (bit depth is pinned to 10-bit automatically).
//   --preset  p1..p7       NVENC speed/quality preset (default p4; p1 fastest/lowest quality,
//                          p7 slowest/best quality; applies uniformly to all 3 NVENC codecs)
//   --seconds <N>          recording duration (default 4)
//   --out     <path>       output file (default: %TEMP%\probe_<combo>.<ext>)
//
// For MP4 the engine records to a transient MKV and this tool performs the
// remux-on-stop step (RemuxToProgressiveMp4), exactly like the app layer.

#define WIN32_LEAN_AND_MEAN
#include <objbase.h> // CoInitializeEx / COINIT_MULTITHREADED (excluded by LEAN_AND_MEAN)
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <recorder_core/codec_types.h>
#include <recorder_core/hdr_color_space.h>
#include <recorder_core/hdr_native.h>
#include <recorder_core/mp4_remuxer.h>
#include <recorder_core/recorder_session.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using namespace recorder_core;

namespace {

const char* ContainerExt(Container c) {
    switch (c) {
    case Container::WebM:
        return "webm";
    case Container::Matroska:
        return "mkv";
    case Container::Mp4:
        return "mp4";
    }
    return "bin";
}

bool ParseContainer(const std::string& s, Container& out) {
    if (s == "mkv" || s == "matroska") {
        out = Container::Matroska;
        return true;
    }
    if (s == "webm") {
        out = Container::WebM;
        return true;
    }
    if (s == "mp4") {
        out = Container::Mp4;
        return true;
    }
    return false;
}

bool ParseVideo(const std::string& s, VideoCodec& out) {
    if (s == "av1") {
        out = VideoCodec::Av1Nvenc;
        return true;
    }
    if (s == "h264" || s == "avc") {
        out = VideoCodec::H264Nvenc;
        return true;
    }
    if (s == "hevc" || s == "h265") {
        out = VideoCodec::HevcNvenc;
        return true;
    }
    return false;
}

bool ParsePreset(const std::string& s, NvencPreset& out) {
    if (s == "p1") {
        out = NvencPreset::P1;
        return true;
    }
    if (s == "p2") {
        out = NvencPreset::P2;
        return true;
    }
    if (s == "p3") {
        out = NvencPreset::P3;
        return true;
    }
    if (s == "p4") {
        out = NvencPreset::P4;
        return true;
    }
    if (s == "p5") {
        out = NvencPreset::P5;
        return true;
    }
    if (s == "p6") {
        out = NvencPreset::P6;
        return true;
    }
    if (s == "p7") {
        out = NvencPreset::P7;
        return true;
    }
    return false;
}

bool ParseHdrMode(const std::string& s, HdrMode& out) {
    if (s == "off") {
        out = HdrMode::Off;
        return true;
    }
    if (s == "tonemap" || s == "sdr") {
        out = HdrMode::TonemapSdr;
        return true;
    }
    if (s == "hdr10" || s == "native") {
        out = HdrMode::Hdr10;
        return true;
    }
    return false;
}

// Read the HDR facts of the monitor owning hmonitor (IDXGIOutput6::GetDesc1),
// matching how the app resolves them for a native HDR10 session. Best-effort;
// leaves facts.hdr_active false when the display cannot be matched/queried.
HdrDisplayFacts QueryMonitorHdrFacts(HMONITOR hmonitor) {
    using Microsoft::WRL::ComPtr;
    HdrDisplayFacts facts;
    if (hmonitor == nullptr) {
        return facts;
    }
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return facts;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            ComPtr<IDXGIOutput6> out6;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmonitor && SUCCEEDED(output.As(&out6))) {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(out6->GetDesc1(&d))) {
                    facts.hdr_active = recorder_core::IsHdrColorSpace(d.ColorSpace);
                    facts.red_primary_x = d.RedPrimary[0];
                    facts.red_primary_y = d.RedPrimary[1];
                    facts.green_primary_x = d.GreenPrimary[0];
                    facts.green_primary_y = d.GreenPrimary[1];
                    facts.blue_primary_x = d.BluePrimary[0];
                    facts.blue_primary_y = d.BluePrimary[1];
                    facts.white_point_x = d.WhitePoint[0];
                    facts.white_point_y = d.WhitePoint[1];
                    facts.max_luminance_nits = d.MaxLuminance;
                    facts.min_luminance_nits = d.MinLuminance;
                }
                return facts;
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return facts;
}

bool ParseAudio(const std::string& s, AudioCodec& out, bool& record_audio) {
    record_audio = true;
    if (s == "none") {
        record_audio = false;
        out = AudioCodec::Opus;
        return true;
    }
    if (s == "opus") {
        out = AudioCodec::Opus;
        return true;
    }
    if (s == "aac") {
        out = AudioCodec::Aac;
        return true;
    }
    if (s == "pcm") {
        out = AudioCodec::Pcm;
        return true;
    }
    if (s == "flac") {
        out = AudioCodec::Flac;
        return true;
    }
    return false;
}

const char* PhaseName(ErrorPhase p) {
    switch (p) {
    case ErrorPhase::None:
        return "None";
    case ErrorPhase::Prepare:
        return "Prepare";
    case ErrorPhase::VideoCapture:
        return "VideoCapture";
    case ErrorPhase::VideoEncode:
        return "VideoEncode";
    case ErrorPhase::AudioCapture:
        return "AudioCapture";
    case ErrorPhase::AudioEncode:
        return "AudioEncode";
    case ErrorPhase::Mux:
        return "Mux";
    case ErrorPhase::Finalize:
        return "Finalize";
    case ErrorPhase::Shutdown:
        return "Shutdown";
    default:
        return "?";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Per-monitor DPI awareness is REQUIRED for IDXGIOutput5::DuplicateOutput1:
    // it returns DXGI_ERROR_UNSUPPORTED for a non-DPI-aware process, which forces
    // the legacy BGRA8-only duplication and prevents any HDR (FP16/R10G10B10A2)
    // capture. The Qt app is DPI-aware via its manifest; this tool sets it here.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The engine worker threads init their own COM apartments; init MTA here so
    // EnumerateTargets() and any main-thread COM use are safe.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::string container_s = "mkv", vcodec_s = "av1", acodec_s = "opus", out_s, range_s = "limited";
    std::string preset_s = "p4";
    std::string hdr_s = "tonemap";
    int seconds = 4;
    int bitdepth = 8;
    int chroma = 420;
    size_t target_idx = 0;
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--list")
            list = true;
        else if (a == "--container")
            container_s = next();
        else if (a == "--vcodec")
            vcodec_s = next();
        else if (a == "--acodec")
            acodec_s = next();
        else if (a == "--seconds")
            seconds = std::atoi(next().c_str());
        else if (a == "--bitdepth")
            bitdepth = std::atoi(next().c_str());
        else if (a == "--chroma")
            chroma = std::atoi(next().c_str());
        else if (a == "--range")
            range_s = next();
        else if (a == "--hdrmode" || a == "--hdr")
            hdr_s = next();
        else if (a == "--preset")
            preset_s = next();
        else if (a == "--target")
            target_idx = static_cast<size_t>(std::atoi(next().c_str()));
        else if (a == "--out")
            out_s = next();
        else {
            fprintf(stderr, "[probe_record] unknown arg: %s\n", a.c_str());
            return 64;
        }
    }

    auto targets = RecorderSession::EnumerateTargets();
    if (targets.empty()) {
        fprintf(stderr, "[probe_record] ERROR: no capture targets found\n");
        return 1;
    }

    if (list) {
        fprintf(stdout, "[probe_record] %zu capture target(s):\n", targets.size());
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& t = targets[i];
            const char* kind = (t.kind == CaptureTarget::Kind::Monitor) ? "monitor" : "window";
            fprintf(stdout, "  [%zu] %-7s  %s\n", i, kind, t.description.c_str());
        }
        return 0;
    }

    if (target_idx >= targets.size()) {
        fprintf(stderr, "[probe_record] ERROR: target index %zu out of range (0..%zu)\n", target_idx,
                targets.size() - 1);
        return 1;
    }

    Container container{};
    VideoCodec vcodec{};
    AudioCodec acodec{};
    NvencPreset preset{};
    bool record_audio = true;
    if (!ParseContainer(container_s, container) || !ParseVideo(vcodec_s, vcodec) ||
        !ParseAudio(acodec_s, acodec, record_audio)) {
        fprintf(stderr, "[probe_record] ERROR: bad container/vcodec/acodec\n");
        return 64;
    }
    if (!ParsePreset(preset_s, preset)) {
        fprintf(stderr, "[probe_record] ERROR: bad --preset (use p1..p7)\n");
        return 64;
    }

    std::filesystem::path out_path;
    if (!out_s.empty()) {
        out_path = out_s;
    } else {
        char tmp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        out_path = std::filesystem::path(tmp) /
                   ("probe_" + container_s + "_" + vcodec_s + "_" + acodec_s + "." + ContainerExt(container));
    }
    std::error_code rm_ec;
    std::filesystem::remove(out_path, rm_ec);

    RecorderConfig cfg;
    cfg.output_path = out_path;
    cfg.target = targets[target_idx];
    cfg.container = container;
    cfg.video_codec = vcodec;
    cfg.audio_codec = acodec;
    cfg.record_audio = record_audio;
    cfg.bit_depth = (bitdepth == 10) ? BitDepth::Bit10 : BitDepth::Bit8;
    if (chroma == 444) {
        cfg.chroma = ChromaSubsampling::Cs444; // 8-bit H.264/HEVC only; Validate() enforces
    } else if (chroma != 420) {
        fprintf(stderr, "[probe_record] ERROR: bad --chroma (use 420|444)\n");
        return 64;
    }
    if (range_s == "limited" || range_s == "tv") {
        cfg.color.range = ColorRange::Limited; // engine default (fix/color-range-signaling)
    } else if (range_s == "full" || range_s == "pc") {
        cfg.color.range = ColorRange::Full; // opt-in
    } else {
        fprintf(stderr, "[probe_record] ERROR: bad --range (use full|limited)\n");
        return 64;
    }
    cfg.nvenc_preset = preset;
    cfg.frame_rate_num = 60;
    cfg.frame_rate_den = 1;
    cfg.cfr = true;

    HdrMode hdr_mode{};
    if (!ParseHdrMode(hdr_s, hdr_mode)) {
        fprintf(stderr, "[probe_record] ERROR: bad --hdrmode (use off|tonemap|hdr10)\n");
        return 64;
    }
    cfg.hdr_mode = hdr_mode;
    // Native HDR10: mirror the app's session-start assembly — when the target
    // display is HDR-active and the codec can encode HDR10, derive BT.2020/PQ
    // colour metadata from the display facts and pin the encode to 10-bit.
    if (cfg.target.kind == CaptureTarget::Kind::Monitor) {
        const HdrDisplayFacts facts = QueryMonitorHdrFacts(reinterpret_cast<HMONITOR>(cfg.target.native_id));
        if (IsHdr10NativeEffective(cfg.hdr_mode, facts.hdr_active, cfg.video_codec)) {
            cfg.color = MakeHdr10ColorMetadata(facts);
            cfg.bit_depth = BitDepth::Bit10;
            fprintf(stdout, "[probe_record] native HDR10: bt2020/pq/10-bit, mastering max=%.0f min=%.4f nits\n",
                    static_cast<double>(facts.max_luminance_nits), static_cast<double>(facts.min_luminance_nits));
        }
    }

    RecorderSession session;
    RecorderResult vr{};
    if (!session.Validate(cfg, &vr)) {
        fprintf(stderr, "[probe_record] VALIDATE REJECTED [%s]: %s (hr=0x%08X)\n", PhaseName(vr.error_phase),
                vr.error_detail.c_str(), static_cast<unsigned>(vr.error_code));
        return 2;
    }

    fprintf(stdout, "[probe_record] recording %s/%s/%s preset=%s for %ds on target [%zu] -> %s\n", container_s.c_str(),
            vcodec_s.c_str(), acodec_s.c_str(), preset_s.c_str(), seconds, target_idx, out_path.string().c_str());
    fflush(stdout);

    std::thread stopper([&session, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        session.Stop();
    });

    const RecorderResult r = session.Record(cfg);
    stopper.join();

    if (!r.succeeded) {
        fprintf(stderr, "[probe_record] RECORD FAILED [%s]: %s (hr=0x%08X)\n", PhaseName(r.error_phase),
                r.error_detail.c_str(), static_cast<unsigned>(r.error_code));
        return 3;
    }

    // MP4 remux-on-stop: the engine wrote a transient MKV; remux it to the final
    // MP4 and drop the transient (mirrors RecordingCoordinator).
    if (container == Container::Mp4) {
        const std::filesystem::path transient = DeriveTransientMkvPath(out_path);
        fprintf(stdout, "[probe_record] remuxing transient %s -> %s\n", transient.string().c_str(),
                out_path.string().c_str());
        fflush(stdout);
        const RemuxResult rr = RemuxToProgressiveMp4(transient, out_path);
        if (!rr.success) {
            fprintf(stderr, "[probe_record] REMUX FAILED: %s (av_err=%d)\n", rr.message.c_str(), rr.av_error_code);
            return 4;
        }
        std::error_code ec;
        std::filesystem::remove(transient, ec);
    }

    std::error_code sz_ec;
    const auto bytes = std::filesystem::file_size(out_path, sz_ec);
    fprintf(stdout, "[probe_record] OK: %s (%llu bytes)\n", out_path.string().c_str(),
            static_cast<unsigned long long>(sz_ec ? 0 : bytes));
    return 0;
}
