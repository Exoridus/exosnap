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
//   --seconds <N>          recording duration (default 4)
//   --out     <path>       output file (default: %TEMP%\probe_<combo>.<ext>)
//
// For MP4 the engine records to a transient MKV and this tool performs the
// remux-on-stop step (RemuxToProgressiveMp4), exactly like the app layer.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h> // CoInitializeEx / COINIT_MULTITHREADED (excluded by LEAN_AND_MEAN)

#include <recorder_core/codec_types.h>
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
    if (s == "mkv" || s == "matroska") { out = Container::Matroska; return true; }
    if (s == "webm") { out = Container::WebM; return true; }
    if (s == "mp4") { out = Container::Mp4; return true; }
    return false;
}

bool ParseVideo(const std::string& s, VideoCodec& out) {
    if (s == "av1") { out = VideoCodec::Av1Nvenc; return true; }
    if (s == "h264" || s == "avc") { out = VideoCodec::H264Nvenc; return true; }
    if (s == "hevc" || s == "h265") { out = VideoCodec::HevcNvenc; return true; }
    return false;
}

bool ParseAudio(const std::string& s, AudioCodec& out, bool& record_audio) {
    record_audio = true;
    if (s == "none") { record_audio = false; out = AudioCodec::Opus; return true; }
    if (s == "opus") { out = AudioCodec::Opus; return true; }
    if (s == "aac") { out = AudioCodec::AacMf; return true; }
    if (s == "pcm") { out = AudioCodec::Pcm; return true; }
    if (s == "flac") { out = AudioCodec::Flac; return true; }
    return false;
}

const char* PhaseName(ErrorPhase p) {
    switch (p) {
    case ErrorPhase::None: return "None";
    case ErrorPhase::Prepare: return "Prepare";
    case ErrorPhase::VideoCapture: return "VideoCapture";
    case ErrorPhase::VideoEncode: return "VideoEncode";
    case ErrorPhase::AudioCapture: return "AudioCapture";
    case ErrorPhase::AudioEncode: return "AudioEncode";
    case ErrorPhase::Mux: return "Mux";
    case ErrorPhase::Finalize: return "Finalize";
    case ErrorPhase::Shutdown: return "Shutdown";
    default: return "?";
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // The engine worker threads init their own COM apartments; init MTA here so
    // EnumerateTargets() and any main-thread COM use are safe.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::string container_s = "mkv", vcodec_s = "av1", acodec_s = "opus", out_s;
    int seconds = 4;
    int bitdepth = 8;
    size_t target_idx = 0;
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--list") list = true;
        else if (a == "--container") container_s = next();
        else if (a == "--vcodec") vcodec_s = next();
        else if (a == "--acodec") acodec_s = next();
        else if (a == "--seconds") seconds = std::atoi(next().c_str());
        else if (a == "--bitdepth") bitdepth = std::atoi(next().c_str());
        else if (a == "--target") target_idx = static_cast<size_t>(std::atoi(next().c_str()));
        else if (a == "--out") out_s = next();
        else { fprintf(stderr, "[probe_record] unknown arg: %s\n", a.c_str()); return 64; }
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
    bool record_audio = true;
    if (!ParseContainer(container_s, container) || !ParseVideo(vcodec_s, vcodec) ||
        !ParseAudio(acodec_s, acodec, record_audio)) {
        fprintf(stderr, "[probe_record] ERROR: bad container/vcodec/acodec\n");
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
    cfg.frame_rate_num = 60;
    cfg.frame_rate_den = 1;
    cfg.cfr = true;

    RecorderSession session;
    RecorderResult vr{};
    if (!session.Validate(cfg, &vr)) {
        fprintf(stderr, "[probe_record] VALIDATE REJECTED [%s]: %s (hr=0x%08X)\n", PhaseName(vr.error_phase),
                vr.error_detail.c_str(), static_cast<unsigned>(vr.error_code));
        return 2;
    }

    fprintf(stdout, "[probe_record] recording %s/%s/%s for %ds on target [%zu] -> %s\n", container_s.c_str(),
            vcodec_s.c_str(), acodec_s.c_str(), seconds, target_idx, out_path.string().c_str());
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
