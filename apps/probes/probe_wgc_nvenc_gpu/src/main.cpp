#include "wgc_nvenc_gpu_probe.h"

#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
//
// Usage:
//   probe_wgc_nvenc_gpu              -- capture first monitor found
//   probe_wgc_nvenc_gpu --list       -- list available targets and exit
//   probe_wgc_nvenc_gpu <index>      -- capture target at <index> (0-based)
//
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    winrt::init_apartment();

    auto targets = wgc_nvenc_gpu::WgcNvencGpuProbe::EnumerateTargets();

    if (targets.empty()) {
        fprintf(stderr, "[probe] ERROR: no capture targets found.\n");
        return 1;
    }

    // --list mode
    if (argc >= 2 && std::string(argv[1]) == "--list") {
        fprintf(stdout, "[probe] available capture targets (%zu):\n", targets.size());
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& t = targets[i];
            const char* kind = (t.kind == wgc_nvenc_gpu::CaptureTarget::Kind::Monitor) ? "monitor" : "window";
            fwprintf(stdout, L"  [%zu] %hs -- %s\n", i, kind, t.description.c_str());
        }
        fflush(stdout);
        return 0;
    }

    // Select target
    size_t idx = 0;
    if (argc >= 2) {
        char* end = nullptr;
        long v = std::strtol(argv[1], &end, 10);
        if (end == argv[1] || v < 0 || static_cast<size_t>(v) >= targets.size()) {
            fprintf(stderr, "[probe] ERROR: invalid target index '%s' (0..%zu)\n", argv[1], targets.size() - 1);
            return 1;
        }
        idx = static_cast<size_t>(v);
    }

    wgc_nvenc_gpu::WgcNvencGpuProbe probe(targets[idx]);
    return probe.Run();
}
