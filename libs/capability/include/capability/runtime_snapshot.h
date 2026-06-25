#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace exosnap::capability {

// Per-display HDR facts, read from IDXGIOutput6::GetDesc1. Informational (shown in
// Diagnostics); also the basis for gating a future HDR recording mode.
struct DisplayHdrFacts {
    std::string name;                // device name, e.g. "\\.\DISPLAY7"
    bool hdr_active = false;         // Windows HDR currently ON (PQ/BT.2020 colour space)
    uint32_t bits_per_color = 0;     // panel link bit depth
    float max_luminance_nits = 0.0f; // reported peak luminance (high ⇒ HDR-capable panel)
    float min_luminance_nits = 0.0f;
    float max_full_frame_nits = 0.0f;
};

struct NvidiaRuntimeFacts {
    bool nvenc_dll_present = false;
    bool nvenc_api_version_valid = false;
    uint32_t nvenc_api_version = 0;
    std::string adapter_name;
    std::string failure_detail;
};

struct MfAacRuntimeFacts {
    bool mftenum_found = false;
    bool clsid_instantiable = false;
    std::string failure_detail;

    bool available() const noexcept {
        return mftenum_found || clsid_instantiable;
    }
};

struct OsRuntimeFacts {
    uint32_t build_number = 0;
    std::string version_string;
    std::string failure_detail;
};

struct RuntimeCapabilitySnapshot {
    NvidiaRuntimeFacts nvidia;
    MfAacRuntimeFacts mf_aac;
    OsRuntimeFacts os;
    std::vector<DisplayHdrFacts> displays;
};

} // namespace exosnap::capability
