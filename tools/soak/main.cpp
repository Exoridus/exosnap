// exosnap-soak — headless reliability endurance ("soak") driver.
//
// Runs a long recording through the REAL RecorderSession pipeline (or the GPU-free
// synthetic twin), samples engine + host-process metrics on a fixed cadence into a
// JSON-Lines timeline, applies the advisory SoakAbortPolicy live, and writes a
// soak report (JSON + Markdown) at the end.
//
// Usage:
//   exosnap-soak --minutes 120 [--vcodec av1 --acodec opus --container mkv] --out C:\tmp\soak.mkv
//   exosnap-soak --synthetic --seconds 30 --realtime --out C:\tmp\syn.mkv
//   exosnap-soak --clapper --seconds 120         (emit start/end flash+beep; live only)
//   exosnap-soak --clapper --seconds 7200 --markers 3 --start-margin-seconds 10
//                --end-margin-seconds 10         (emit start/middle/end markers)
//   exosnap-soak --minutes 120 --audio-sources sys,mic --out C:\tmp\soak.mkv
//                                                 (SYS + MIC as separate tracks, not merged;
//                                                 omit --audio-sources for the legacy default of
//                                                 a single merged loopback track)
//
// Runtime gate: the REAL path needs an NVIDIA GPU. With no capture target it fails
// fast (non-zero exit) instead of hanging — a stray CI invocation dies deterministically.
// The --synthetic path runs anywhere.

#define WIN32_LEAN_AND_MEAN
#include <objbase.h>
#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/recorder_session.h>

#include "clapper_schedule.h"
#include "soak_metrics.h"
#include "soak_process_sampler.h"
#include "soak_runner.h"
#include "synthetic_session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>

using namespace exosnap::engine;

namespace {

std::atomic<RecorderSession*> g_session{nullptr};
std::atomic<exosnap::soak::SoakRunner*> g_runner{nullptr};

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (auto* s = g_session.load())
            s->Stop();
        return TRUE;
    }
    return FALSE;
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
        out = VideoCodec::Av1;
        return true;
    }
    if (s == "h264" || s == "avc") {
        out = VideoCodec::H264;
        return true;
    }
    if (s == "hevc" || s == "h265") {
        out = VideoCodec::Hevc;
        return true;
    }
    return false;
}
// Parses a comma-separated list of audio source names ("app", "sys", "mic")
// into un-merged AudioSourceRow entries (one resolved track per source, per
// the 0.9 release-gate soak requirement of SYS + MIC as separate tracks).
// Row order follows the given token order. Rejects empty/unknown tokens.
bool ParseAudioSources(const std::string& s, std::vector<AudioSourceRow>& out, std::string& error) {
    out.clear();
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t comma = s.find(',', start);
        const std::string token = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        AudioSourceRow row;
        row.enabled = true;
        row.merge_with_above = false;
        if (token == "app")
            row.kind = AudioSourceKind::App;
        else if (token == "sys")
            row.kind = AudioSourceKind::Sys;
        else if (token == "mic")
            row.kind = AudioSourceKind::Mic;
        else {
            error = "unknown audio source '" + token + "' (expected app, sys, or mic)";
            return false;
        }
        out.push_back(row);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    if (out.empty()) {
        error = "--audio-sources requires at least one source";
        return false;
    }
    return true;
}

bool ParseAudio(const std::string& s, AudioCodec& out) {
    if (s == "opus") {
        out = AudioCodec::Opus;
        return true;
    }
    if (s == "aac") {
        out = AudioCodec::Aac;
        return true;
    }
    if (s == "flac") {
        out = AudioCodec::Flac;
        return true;
    }
    if (s == "pcm") {
        out = AudioCodec::Pcm;
        return true;
    }
    return false;
}

// Clean-EOF + media-duration probe via the vendored libavformat (no system ffprobe).
struct MediaProbe {
    bool ok = false;
    double duration_s = 0.0;
};
MediaProbe ProbeMedia(const std::string& path) {
    MediaProbe p;
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) != 0)
        return p;
    if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0)
        p.duration_s = static_cast<double>(ctx->duration) / AV_TIME_BASE;
    AVPacket* pkt = av_packet_alloc();
    int ret = 0;
    while ((ret = av_read_frame(ctx, pkt)) >= 0)
        av_packet_unref(pkt);
    p.ok = (ret == AVERROR_EOF);
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return p;
}

std::string Timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

void WriteReport(const exosnap::soak::SoakSummary& summary, const std::string& report_dir,
                 const std::vector<std::pair<std::string, std::string>>& metadata) {
    std::filesystem::create_directories(report_dir);
    const std::string base = report_dir + "/soak-report-" + Timestamp();
    {
        std::ofstream j(base + ".json", std::ios::binary);
        j << exosnap::soak::SummaryToJson(summary, metadata);
    }
    {
        std::ofstream m(base + ".md", std::ios::binary);
        m << exosnap::soak::SummaryToMarkdown(summary, metadata);
    }
    std::fprintf(stdout, "[soak] report: %s.json / .md\n", base.c_str());
}

// --- Clapper: full-frame white flash + loud beep on a fixed schedule (live only) ---
void EmitFlash(int flash_ms) {
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, sw, sh, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd)
        return;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(+[](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                          if (msg == WM_PAINT) {
                              PAINTSTRUCT ps;
                              HDC dc = BeginPaint(h, &ps);
                              FillRect(dc, &ps.rcPaint, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                              EndPaint(h, &ps);
                              return 0;
                          }
                          return DefWindowProcW(h, msg, wp, lp);
                      }));
    ShowWindow(hwnd, SW_SHOWNA);
    UpdateWindow(hwnd);
    Beep(1000, static_cast<DWORD>(flash_ms));
    Sleep(static_cast<DWORD>(flash_ms));
    DestroyWindow(hwnd);
}

int RunClapper(const exosnap::soak::ClapperSchedule& schedule, int flash_ms) {
    std::fprintf(stdout, "[soak] clapper: %zu markers over %lld s at", schedule.marker_seconds.size(),
                 static_cast<long long>(schedule.total_seconds));
    for (const std::int64_t marker : schedule.marker_seconds)
        std::fprintf(stdout, " +%llds", static_cast<long long>(marker));
    std::fprintf(stdout, ". Point ExoSnap at the primary display.\n");
    std::fflush(stdout);

    const auto epoch = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < schedule.marker_seconds.size(); ++i) {
        std::this_thread::sleep_until(epoch + std::chrono::seconds(schedule.marker_seconds[i]));
        std::fprintf(stdout, "[soak] clapper: marker %zu/%zu at +%llds\n", i + 1, schedule.marker_seconds.size(),
                     static_cast<long long>(schedule.marker_seconds[i]));
        std::fflush(stdout);
        EmitFlash(flash_ms);
    }
    std::fprintf(stdout, "[soak] clapper: all markers emitted. Analyze with scripts/dev/av-sync-check.py\n");
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool synthetic = false, realtime = false, clapper = false, print_clapper_schedule = false;
    bool seconds_set = false, minutes_set = false;
    std::int64_t minutes = 0, seconds = 0, start_margin_seconds = 0, end_margin_seconds = 0;
    int marker_count = 2, flash_ms = 120, sample_ms = 1000;
    std::string container_s = "mkv", vcodec_s = "av1", acodec_s = "opus", out_s, report_dir, audio_sources_s;
    exosnap::soak::SoakThresholds thresholds;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--synthetic")
            synthetic = true;
        else if (a == "--realtime")
            realtime = true;
        else if (a == "--clapper")
            clapper = true;
        else if (a == "--minutes" || a == "--seconds" || a == "--markers" || a == "--flash-ms" || a == "--sample-ms" ||
                 a == "--start-margin-seconds" || a == "--end-margin-seconds") {
            const std::string value = next();
            std::int64_t parsed = 0;
            std::string parse_error;
            if (!exosnap::soak::ParsePositiveInt64(value, parsed, parse_error)) {
                std::fprintf(stderr, "[soak] %s: %s\n", a.c_str(), parse_error.c_str());
                return 64;
            }
            if (a == "--minutes") {
                minutes = parsed;
                minutes_set = true;
            } else if (a == "--seconds") {
                seconds = parsed;
                seconds_set = true;
            } else if (a == "--markers") {
                if (parsed > std::numeric_limits<int>::max()) {
                    std::fprintf(stderr, "[soak] --markers: value is out of range\n");
                    return 64;
                }
                marker_count = static_cast<int>(parsed);
            } else if (a == "--flash-ms") {
                if (parsed > std::numeric_limits<int>::max()) {
                    std::fprintf(stderr, "[soak] --flash-ms: value is out of range\n");
                    return 64;
                }
                flash_ms = static_cast<int>(parsed);
            } else if (a == "--sample-ms") {
                if (parsed > std::numeric_limits<int>::max()) {
                    std::fprintf(stderr, "[soak] --sample-ms: value is out of range\n");
                    return 64;
                }
                sample_ms = static_cast<int>(parsed);
            } else if (a == "--start-margin-seconds") {
                start_margin_seconds = parsed;
            } else {
                end_margin_seconds = parsed;
            }
        } else if (a == "--print-clapper-schedule")
            print_clapper_schedule = true;
        else if (a == "--container")
            container_s = next();
        else if (a == "--vcodec")
            vcodec_s = next();
        else if (a == "--acodec")
            acodec_s = next();
        else if (a == "--out")
            out_s = next();
        else if (a == "--report-dir")
            report_dir = next();
        else if (a == "--audio-sources")
            audio_sources_s = next();
        else if (a == "--max-drift-ms")
            thresholds.av_drift_abort_ms = std::atof(next().c_str());
        else if (a == "--max-skew-ms")
            thresholds.duration_skew_abort_ms = std::atof(next().c_str());
        else {
            std::fprintf(stderr, "[soak] unknown arg: %s\n", a.c_str());
            return 64;
        }
    }

    if (seconds_set && minutes_set) {
        std::fprintf(stderr, "[soak] choose exactly one of --seconds or --minutes\n");
        return 64;
    }
    if (minutes > std::numeric_limits<std::int64_t>::max() / 60) {
        std::fprintf(stderr, "[soak] --minutes: duration is out of range\n");
        return 64;
    }
    const std::int64_t duration_seconds = seconds_set ? seconds : (minutes_set ? minutes * 60 : 120);
    const double duration_s = static_cast<double>(duration_seconds);
    if (report_dir.empty())
        report_dir = out_s.empty() ? "." : std::filesystem::path(out_s).parent_path().string();
    if (report_dir.empty())
        report_dir = ".";

    if (clapper) {
        exosnap::soak::ClapperSchedule schedule;
        std::string schedule_error;
        if (!exosnap::soak::BuildClapperSchedule(duration_seconds, marker_count, start_margin_seconds,
                                                 end_margin_seconds, schedule, schedule_error)) {
            std::fprintf(stderr, "[soak] invalid clapper schedule: %s\n", schedule_error.c_str());
            return 64;
        }
        if (print_clapper_schedule) {
            std::fprintf(stdout, "clapper_schedule_seconds=");
            for (std::size_t i = 0; i < schedule.marker_seconds.size(); ++i)
                std::fprintf(stdout, "%s%lld", i == 0 ? "" : ",", static_cast<long long>(schedule.marker_seconds[i]));
            std::fprintf(stdout, "\n");
            return 0;
        }
        return RunClapper(schedule, flash_ms);
    }
    if (print_clapper_schedule) {
        std::fprintf(stderr, "[soak] --print-clapper-schedule requires --clapper\n");
        return 64;
    }

    Container container{};
    VideoCodec vcodec{};
    AudioCodec acodec{};
    if (!ParseContainer(container_s, container) || !ParseVideo(vcodec_s, vcodec) || !ParseAudio(acodec_s, acodec)) {
        std::fprintf(stderr, "[soak] bad container/vcodec/acodec\n");
        return 64;
    }
    std::vector<AudioSourceRow> audio_rows;
    if (!audio_sources_s.empty()) {
        std::string audio_error;
        if (!ParseAudioSources(audio_sources_s, audio_rows, audio_error)) {
            std::fprintf(stderr, "[soak] --audio-sources: %s\n", audio_error.c_str());
            return 64;
        }
    }
    if (out_s.empty()) {
        char tmp[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        out_s = (std::filesystem::path(tmp) / "exosnap-soak.mkv").string();
    }

    exosnap::soak::WinProcessSampler win_sampler;
    const std::string jsonl = out_s + ".timeline.jsonl";
    exosnap::soak::SoakRunner runner(thresholds, win_sampler, jsonl);
    g_runner.store(&runner);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    std::vector<std::pair<std::string, std::string>> metadata = {
        {"mode", synthetic ? "synthetic" : "real"},
        {"container", container_s},
        {"vcodec", vcodec_s},
        {"acodec", acodec_s},
        {"audio_sources", audio_sources_s.empty() ? "legacy-single-track" : audio_sources_s},
        {"target_seconds", std::to_string(duration_s)},
        {"volume", std::filesystem::path(out_s).root_name().string()},
        {"advisory", "thresholds advisory for 0.10 — not a release gate"},
    };

    bool ok = false;
    std::string err;

    if (synthetic) {
        exosnap::engine::testutil::SyntheticSessionConfig cfg;
        cfg.video_codec = vcodec;
        cfg.audio_codec = acodec;
        cfg.output_path = out_s;
        cfg.target_seconds = duration_s;
        cfg.realtime_pace = realtime;
        cfg.drive_stats_collector = true;
        exosnap::engine::testutil::SyntheticSession session(cfg);
        session.SetStatsCallback([&](const SessionStats& s) { runner.OnStats(s); });
        session.SetDiagnosticsCallback([&](const RecordingDiagnosticsSnapshot& d) { runner.OnDiagnostics(d); });
        runner.Start(sample_ms / 1000.0);
        const auto r = session.Run();
        runner.Stop();
        ok = r.success;
        err = r.error;
    } else {
        auto targets = RecorderSession::EnumerateTargets();
        if (targets.empty()) {
            std::fprintf(stderr, "[soak] no capture targets (needs a GPU/display). The real path requires "
                                 "hardware; use --synthetic on a headless host.\n");
            return 2;
        }
        RecorderConfig cfg;
        cfg.output_path = out_s;
        cfg.target = targets[0];
        cfg.container = container;
        cfg.video_codec = vcodec;
        cfg.audio_codec = acodec;
        cfg.record_audio = true;
        cfg.frame_rate_num = 60;
        cfg.frame_rate_den = 1;
        cfg.cfr = true;
        if (!audio_rows.empty()) {
            const bool window_target = cfg.target.kind == CaptureTarget::Kind::Window;
            cfg.audio_track_plan = ResolveAudioTracks(NormalizeSourceRowsForTarget(audio_rows, window_target));
        }

        RecorderSession session;
        RecorderResult vr{};
        if (!session.Validate(cfg, &vr)) {
            std::fprintf(stderr, "[soak] validate rejected: %s\n", vr.error_detail.c_str());
            return 2;
        }
        session.SetStatsCallback([&](const SessionStats& s) { runner.OnStats(s); });
        session.SetDiagnosticsCallback([&](const RecordingDiagnosticsSnapshot& d) {
            runner.OnDiagnostics(d);
            if (runner.aborted()) {
                if (auto* sp = g_session.load())
                    sp->Stop();
            }
        });
        g_session.store(&session);

        std::thread stopper([&session, duration_s] {
            std::this_thread::sleep_for(std::chrono::duration<double>(duration_s));
            session.Stop();
        });
        runner.Start(sample_ms / 1000.0);
        const RecorderResult r = session.Record(cfg);
        stopper.join();
        runner.Stop();
        g_session.store(nullptr);
        ok = r.succeeded;
        if (!ok)
            err = r.error_detail;
    }

    const MediaProbe probe = ProbeMedia(out_s);
    metadata.emplace_back("file", out_s);
    metadata.emplace_back("clean_eof", probe.ok ? "true" : "false");
    metadata.emplace_back("media_duration_s", std::to_string(probe.duration_s));
    metadata.emplace_back("session_ok", ok ? "true" : "false");
    if (!err.empty())
        metadata.emplace_back("error", err);

    const auto summary = runner.Summarize();
    WriteReport(summary, report_dir, metadata);

    if (runner.aborted()) {
        std::fprintf(stderr, "[soak] ABORTED (advisory): %s\n", runner.abort_decision().reason.c_str());
        return 3;
    }
    if (!ok) {
        std::fprintf(stderr, "[soak] session failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "[soak] OK — %s (clean_eof=%s, media=%.1fs)\n", out_s.c_str(), probe.ok ? "yes" : "no",
                 probe.duration_s);
    return 0;
}
