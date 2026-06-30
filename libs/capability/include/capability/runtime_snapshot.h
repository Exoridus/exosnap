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

    // Per-GPU NVENC codec-GUID probe (Recommended-Codec / truthful detection).
    // nvenc_codec_probed is true only when a real NVENC session was opened and its
    // EncodeGUIDs enumerated successfully; the per-codec flags are then authoritative.
    // When false (no DLL / no device / no session — i.e. headless CI or a probe
    // failure) the per-codec flags are meaningless and the static baseline is kept.
    bool nvenc_codec_probed = false;
    bool nvenc_av1 = false;
    bool nvenc_hevc = false;
    bool nvenc_h264 = false;
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

// S4: Media Foundation webcam probe facts.
// Available = mfplat.dll is loadable on this system.
// Absent on Windows-N without the Media Feature Pack.
struct MfWebcamRuntimeFacts {
    bool available = false;
    std::string failure_detail; // populated only when unavailable
};

struct RuntimeCapabilitySnapshot {
    NvidiaRuntimeFacts nvidia;
    MfAacRuntimeFacts mf_aac;
    MfWebcamRuntimeFacts mf_webcam; // S4: webcam MF probe
    OsRuntimeFacts os;
    std::vector<DisplayHdrFacts> displays;
};

} // namespace exosnap::capability
