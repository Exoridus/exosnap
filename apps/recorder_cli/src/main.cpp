#include <capability/capability_builder.h>
#include <capability/config_types.h>
#include <capability/resolver.h>
#include <capability/support_level.h>
#include <capability/translation.h>
#include <capability/user_config.h>

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

static const char* support_level_string(exosnap::capability::SupportLevel level) {
    using L = exosnap::capability::SupportLevel;
    switch (level) {
        case L::Available:        return "Available";
        case L::ValidUnvalidated: return "ValidUnvalidated";
        case L::NotImplemented:   return "NotImplemented";
        case L::Invalid:          return "Invalid";
        default:                  return "Unknown";
    }
}

static const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

static void print_support_line(
    const char* dimension,
    const std::string& name,
    const exosnap::capability::SupportAnnotation& annotation) {
    std::printf(
        "  %-10s %-20s : %-16s",
        dimension,
        name.c_str(),
        support_level_string(annotation.level));
    if (!annotation.reason.empty()) {
        std::printf(" | %s", annotation.reason.c_str());
    }
    std::puts("");
}

static void print_adjustments(const exosnap::capability::ResolveResult& result) {
    if (result.adjustments.empty()) {
        return;
    }

    std::puts("  Adjustments:");
    for (const auto& adjustment : result.adjustments) {
        std::printf(
            "    - %s: %s -> %s",
            adjustment.field.c_str(),
            adjustment.from.c_str(),
            adjustment.to.c_str());
        if (!adjustment.reason.empty()) {
            std::printf(" (%s)", adjustment.reason.c_str());
        }
        std::puts("");
    }
}

static void print_warnings(const exosnap::capability::ResolveResult& result) {
    if (result.warnings.empty()) {
        return;
    }

    std::puts("  Warnings:");
    for (const auto& warning : result.warnings) {
        if (warning.code.empty()) {
            std::printf("    - %s\n", warning.message.c_str());
        } else {
            std::printf("    - [%s] %s\n", warning.code.c_str(), warning.message.c_str());
        }
    }
}

static void print_invalidity(const exosnap::capability::ResolveResult& result) {
    if (result.invalidity.empty()) {
        return;
    }

    std::puts("  Invalidity:");
    for (const auto& invalid : result.invalidity) {
        std::printf("    - %s: %s\n", invalid.field.c_str(), invalid.message.c_str());
    }
}

static exosnap::capability::SupportAnnotation query_combo_for_config(
    const exosnap::capability::CapabilitySet& caps,
    const exosnap::capability::UserRecorderConfig& config) {
    return caps.QueryCombo(
        config.container,
        config.video_codec,
        config.audio_codec,
        config.chroma,
        config.bit_depth);
}

static int run_capabilities_mode() {
    using namespace exosnap::capability;

    try {
        const CapabilitySet caps = CapabilityBuilder::BuildFromHardwareQuery();
        const RuntimeCapabilitySnapshot& runtime = caps.runtime;

        std::puts("\n=== Runtime Facts ===");
        std::printf("  %-25s : %s\n", "OS version", runtime.os.version_string.c_str());
        std::printf("  %-25s : %s\n", "Adapter", runtime.nvidia.adapter_name.c_str());
        std::printf("  %-25s : %s\n", "NVENC DLL present", yes_no(runtime.nvidia.nvenc_dll_present));
        std::printf(
            "  %-25s : %s (version: 0x%08X)\n",
            "NVENC API version valid",
            yes_no(runtime.nvidia.nvenc_api_version_valid),
            runtime.nvidia.nvenc_api_version);
        std::printf("  %-25s : %s\n", "MF AAC MFTEnumEx found", yes_no(runtime.mf_aac.mftenum_found));
        std::printf("  %-25s : %s\n", "MF AAC CLSID instantiable", yes_no(runtime.mf_aac.clsid_instantiable));
        std::printf("  %-25s : %s\n", "MF AAC effective", yes_no(runtime.mf_aac.available()));

        std::puts("\n=== Dimension Support ===");
        for (const auto container : AllContainers()) {
            print_support_line("Container", std::string(ToString(container)), caps.QueryContainer(container));
        }
        std::puts("");
        for (const auto video : AllVideoCodecs()) {
            print_support_line("VideoCodec", std::string(ToString(video)), caps.QueryVideoCodec(video));
        }
        std::puts("");
        for (const auto audio : AllAudioCodecs()) {
            std::string audio_name = std::string(ToString(audio));
            if (audio == AudioCodec::AacMf) {
                audio_name = "AAC (MF)";
            }
            print_support_line("AudioCodec", audio_name, caps.QueryAudioCodec(audio));
        }
        std::puts("");
        for (const auto chroma : AllChromaModes()) {
            print_support_line("Chroma", std::string(ToString(chroma)), caps.QueryChroma(chroma));
        }
        std::puts("");
        for (const auto bit_depth : AllBitDepths()) {
            print_support_line("BitDepth", std::string(ToString(bit_depth)), caps.QueryBitDepth(bit_depth));
        }

        const UserRecorderConfig default_user_config{};
        const SettingsResolver resolver(caps);
        const ResolveResult validation = resolver.ValidateConfig(default_user_config);
        const SupportAnnotation combo = query_combo_for_config(caps, validation.resolved_config);

        std::puts("\n=== Primary Profile Validation (MKV + AV1 NVENC + AAC-MF + 4:2:0 + 8-bit) ===");
        std::printf("  %-15s : %s\n", "Combo support", support_level_string(combo.level));
        std::printf("  %-15s : %s\n", "Selectable", yes_no(IsSelectable(combo.level)));
        std::printf("  %-15s : %s\n", "Status", validation.succeeded ? "READY" : "BLOCKED");

        print_adjustments(validation);
        print_warnings(validation);
        print_invalidity(validation);

        return validation.succeeded ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Capability initialization failed: %s\n", e.what());
        return 2;
    } catch (...) {
        std::fputs("Capability initialization failed: unknown error.\n", stderr);
        return 2;
    }
}

// ---------------------------------------------------------------------------
// Argument parsing (optional flags: --list, --target N, --capabilities)
// ---------------------------------------------------------------------------

struct CliArgs {
    bool     list_only    = false;
    bool     capabilities = false;
    int      target_index = -1; // 1-based, -1 = prompt user
};

static CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--list") {
            args.list_only = true;
        } else if (arg == "--capabilities") {
            args.capabilities = true;
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
    using namespace exosnap::capability;

    std::printf("recorder_cli — exosnap recorder_core harness (version: %s)\n",
                std::string(recorder_core::version()).c_str());

    const CliArgs cli = parse_args(argc, argv);
    if (cli.capabilities) {
        return run_capabilities_mode();
    }

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

    // 3. Build output location
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
    try {
        const CapabilitySet caps = CapabilityBuilder::BuildFromHardwareQuery();
        UserRecorderConfig user_config{};
        SettingsResolver resolver(caps);
        const ResolveResult validation = resolver.ValidateConfig(user_config);
        if (!validation.succeeded) {
            std::fputs("Default recording configuration is not available on this machine:\n", stderr);
            for (const auto& invalid : validation.invalidity) {
                std::fprintf(stderr, "  - %s: %s\n", invalid.field.c_str(), invalid.message.c_str());
            }
            for (const auto& adjustment : validation.adjustments) {
                std::fprintf(
                    stderr,
                    "  - adjustment %s: %s -> %s (%s)\n",
                    adjustment.field.c_str(),
                    adjustment.from.c_str(),
                    adjustment.to.c_str(),
                    adjustment.reason.c_str());
            }
            for (const auto& warning : validation.warnings) {
                std::fprintf(stderr, "  - warning %s: %s\n", warning.code.c_str(), warning.message.c_str());
            }
            return 1;
        }

        ResolveResult translation_validation;
        try {
            config = ToRecorderCoreConfig(user_config, caps, &translation_validation);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Default recording configuration cannot be translated: %s\n", e.what());
            for (const auto& invalid : translation_validation.invalidity) {
                std::fprintf(stderr, "  - %s: %s\n", invalid.field.c_str(), invalid.message.c_str());
            }
            for (const auto& adjustment : translation_validation.adjustments) {
                std::fprintf(
                    stderr,
                    "  - adjustment %s: %s -> %s (%s)\n",
                    adjustment.field.c_str(),
                    adjustment.from.c_str(),
                    adjustment.to.c_str(),
                    adjustment.reason.c_str());
            }
            for (const auto& warning : translation_validation.warnings) {
                std::fprintf(stderr, "  - warning %s: %s\n", warning.code.c_str(), warning.message.c_str());
            }
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Capability initialization failed: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fputs("Capability initialization failed: unknown error.\n", stderr);
        return 1;
    }

    config.output_path = output_path;
    config.target      = chosen_target;

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
