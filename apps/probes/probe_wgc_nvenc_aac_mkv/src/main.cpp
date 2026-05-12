#include "wgc_nvenc_aac_mkv_probe.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
//
// Usage:
//   probe_wgc_nvenc_aac_mkv              -- capture first monitor, 30 seconds
//   probe_wgc_nvenc_aac_mkv --list       -- list available targets and exit
//   probe_wgc_nvenc_aac_mkv <index>      -- capture target at <index> (0-based)
//
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    winrt::init_apartment();

    auto targets = wgc_nvenc_aac_mkv::WgcNvencAacMkvProbe::EnumerateTargets();

    if (targets.empty()) {
        fprintf(stderr, "[probe] ERROR: no capture targets found.\n");
        return 1;
    }

    if (argc >= 2 && std::string(argv[1]) == "--list") {
        fprintf(stdout, "[probe] available capture targets (%zu):\n", targets.size());
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& t = targets[i];
            const char* kind = (t.kind == wgc_nvenc_aac_mkv::CaptureTarget::Kind::Monitor)
                               ? "monitor" : "window";
            fwprintf(stdout, L"  [%zu] %hs -- %s\n", i, kind, t.description.c_str());
        }
        fflush(stdout);
        return 0;
    }

    size_t idx = 0;
    if (argc >= 2) {
        char* end = nullptr;
        long v = std::strtol(argv[1], &end, 10);
        if (end == argv[1] || v < 0 || static_cast<size_t>(v) >= targets.size()) {
            fprintf(stderr, "[probe] ERROR: invalid target index '%s' (0..%zu)\n",
                    argv[1], targets.size() - 1);
            return 1;
        }
        idx = static_cast<size_t>(v);
    }

    wgc_nvenc_aac_mkv::WgcNvencAacMkvProbe probe(targets[idx]);
    return probe.Run();
}
