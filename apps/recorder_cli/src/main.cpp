#include <recorder_core/codec_types.h>
#include <recorder_core/error_types.h>
#include <recorder_core/recorder_session.h>
#include <recorder_core/session_stats.h>
#include <recorder_core/version.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return {};
    std::string s(static_cast<size_t>(sz), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                          s.data(), sz, nullptr, nullptr);
    return s;
}

static const char* to_string(recorder_core::ErrorPhase phase) {
    using P = recorder_core::ErrorPhase;
    switch (phase) {
        case P::None:         return "None";
        case P::Prepare:      return "Prepare";
        case P::VideoCapture: return "VideoCapture";
        case P::VideoEncode:  return "VideoEncode";
        case P::AudioCapture: return "AudioCapture";
        case P::AudioEncode:  return "AudioEncode";
        case P::Mux:          return "Mux";
        case P::Finalize:     return "Finalize";
        case P::Shutdown:     return "Shutdown";
        default:              return "Unknown";
    }
}

static const char* kind_string(recorder_core::CaptureTarget::Kind kind) {
    using K = recorder_core::CaptureTarget::Kind;
    switch (kind) {
        case K::Monitor: return "Monitor";
        case K::Window:  return "Window";
        default:         return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Argument parsing (optional flags: --list, --target N)
// ---------------------------------------------------------------------------

struct CliArgs {
    bool     list_only    = false;
    int      target_index = -1; // 1-based, -1 = prompt user
};

static CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            args.list_only = true;
        } else if (arg == "--target" && i + 1 < argc) {
            ++i;
            args.target_index = std::stoi(argv[i]);
        }
    }
    return args;
}

// ---------------------------------------------------------------------------
// Stats callback — prints one line per invocation (~250 ms from library)
// ---------------------------------------------------------------------------

static void print_stats(const recorder_core::SessionStats& s) {
    std::printf(
        "\r[%.1fs] vcap=%llu vpkt=%llu apkt=%llu vbytes=%llu abytes=%llu",
        s.elapsed_seconds,
        static_cast<unsigned long long>(s.video_frames_captured),
        static_cast<unsigned long long>(s.encoded_video_packets),
        static_cast<unsigned long long>(s.audio_packets),
        static_cast<unsigned long long>(s.video_bytes),
        static_cast<unsigned long long>(s.audio_bytes));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Final result printer
// ---------------------------------------------------------------------------

static void print_result(const recorder_core::RecorderResult& result) {
    std::puts(""); // end progress line
    std::puts("---------- Recording Result ----------");
    std::printf("  succeeded          : %s\n", result.succeeded ? "true" : "false");
    std::printf("  error_phase        : %s\n", to_string(result.error_phase));
    std::printf("  error_code (hex)   : 0x%08lX\n", static_cast<unsigned long>(result.error_code));
    if (!result.error_detail.empty()) {
        std::printf("  error_detail       : %s\n", result.error_detail.c_str());
    }
    std::puts("---------- Session Stats  -----------");
    const auto& s = result.stats;
    std::printf("  elapsed_seconds    : %.3f\n",  s.elapsed_seconds);
    std::printf("  video_frames_capt  : %llu\n",  static_cast<unsigned long long>(s.video_frames_captured));
    std::printf("  encoded_video_pkts : %llu\n",  static_cast<unsigned long long>(s.encoded_video_packets));
    std::printf("  audio_packets      : %llu\n",  static_cast<unsigned long long>(s.audio_packets));
    std::printf("  video_bytes        : %llu\n",  static_cast<unsigned long long>(s.video_bytes));
    std::printf("  audio_bytes        : %llu\n",  static_cast<unsigned long long>(s.audio_bytes));
    std::printf("  output_file_bytes  : %llu\n",  static_cast<unsigned long long>(s.output_file_bytes));
    std::printf("  video_duration_ns  : %llu\n",  static_cast<unsigned long long>(s.video_duration_ns));
    std::printf("  audio_duration_ns  : %llu\n",  static_cast<unsigned long long>(s.audio_duration_ns));
    std::printf("  duration_skew_ms   : %.3f\n",  s.duration_skew_ms);
    std::printf("  dropped_frames     : %llu\n",  static_cast<unsigned long long>(s.dropped_or_skipped_video_frames));
    std::printf("  source_loss        : %s\n",    s.source_loss ? "true" : "false");
    std::puts("-------------------------------------");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::printf("recorder_cli — exosnap recorder_core harness (version: %s)\n",
                std::string(recorder_core::version()).c_str());

    const CliArgs cli = parse_args(argc, argv);

    // 1. Enumerate capture targets
    const auto targets = recorder_core::RecorderSession::EnumerateTargets();

    if (targets.empty()) {
        std::fputs("ERROR: No capture targets found.\n", stderr);
        return 1;
    }

    std::puts("\nAvailable capture targets:");
    for (size_t i = 0; i < targets.size(); ++i) {
        std::printf("  [%zu] %-8s  %s\n",
                    i + 1,
                    kind_string(targets[i].kind),
                    narrow(targets[i].description).c_str());
    }

    if (cli.list_only) {
        return 0;
    }

    // 2. Select target
    size_t selected_index = 0; // 0-based

    if (cli.target_index >= 1 && static_cast<size_t>(cli.target_index) <= targets.size()) {
        selected_index = static_cast<size_t>(cli.target_index) - 1;
        std::printf("\nUsing target [%d]: %s\n",
                    cli.target_index,
                    narrow(targets[selected_index].description).c_str());
    } else {
        // Interactive prompt
        std::printf("\nSelect target (1-%zu): ", targets.size());
        std::fflush(stdout);
        int choice = 0;
        if (!(std::cin >> choice) || choice < 1 || static_cast<size_t>(choice) > targets.size()) {
            std::fputs("ERROR: Invalid selection.\n", stderr);
            return 1;
        }
        selected_index = static_cast<size_t>(choice) - 1;
    }

    const recorder_core::CaptureTarget& chosen_target = targets[selected_index];

    // 3. Build config
    const std::filesystem::path output_dir  = "recorder_cli_output";
    const std::filesystem::path output_path = output_dir / "recorder_core_av1_aac.mkv";

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        std::fprintf(stderr, "ERROR: Could not create output directory '%s': %s\n",
                     output_dir.string().c_str(), ec.message().c_str());
        return 1;
    }

    recorder_core::RecorderConfig config;
    config.output_path   = output_path;
    config.target        = chosen_target;
    config.container     = recorder_core::Container::Matroska;
    config.video_codec   = recorder_core::VideoCodec::Av1Nvenc;
    config.audio_codec   = recorder_core::AudioCodec::AacMf;
    config.chroma        = recorder_core::ChromaSubsampling::Cs420;
    config.bit_depth     = recorder_core::BitDepth::Bit8;
    config.frame_rate_num = 60;
    config.frame_rate_den = 1;

    std::printf("Output path: %s\n", output_path.string().c_str());

    // 4. Validate
    recorder_core::RecorderSession session;

    recorder_core::RecorderResult validation_result;
    if (!session.Validate(config, &validation_result)) {
        std::fprintf(stderr, "ERROR: Validation failed\n");
        std::fprintf(stderr, "  error_phase  : %s\n", to_string(validation_result.error_phase));
        std::fprintf(stderr, "  error_code   : 0x%08lX\n",
                     static_cast<unsigned long>(validation_result.error_code));
        std::fprintf(stderr, "  error_detail : %s\n", validation_result.error_detail.c_str());
        return 1;
    }
    std::puts("Validation: OK");

    // 5. Set stats callback before Record()
    session.SetStatsCallback(print_stats);

    // 6. Launch stop thread — sleeps 30s then calls Stop()
    constexpr int kRecordingDurationSeconds = 30;
    std::thread stop_thread([&session, kRecordingDurationSeconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(kRecordingDurationSeconds));
        std::puts("\n[stop thread] Duration elapsed — requesting stop.");
        session.Stop();
    });

    std::printf("\nRecording for up to %d seconds (Ctrl+C to abort)...\n",
                kRecordingDurationSeconds);

    // 7. Record() blocks until Stop() or fatal error
    recorder_core::RecorderResult result = session.Record(config);

    // 8. Join stop thread
    if (stop_thread.joinable()) {
        stop_thread.join();
    }

    // 9. Print final result
    print_result(result);

    return result.succeeded ? 0 : 1;
}
