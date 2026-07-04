#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::capability {

// Per-display HDR facts, read from IDXGIOutput6::GetDesc1. Informational (shown in
// Diagnostics); also the basis for gating the HDR recording mode.
//
// The chromaticity primaries + luminance range mirror DXGI_OUTPUT_DESC1 exactly.
// They are the display's *capabilities*, not the content's mastering values; a
// future change feeds them into the container mastering-display metadata (the
// display's capabilities are the usual approximation for the content's
// mastering values). This only captures them for now.
struct DisplayHdrFacts {
    std::string name;            // device name, e.g. "\\.\DISPLAY7"
    bool hdr_active = false;     // Windows HDR currently ON (PQ/BT.2020 colour space)
    uint32_t bits_per_color = 0; // panel link bit depth

    // CIE 1931 xy chromaticity primaries + white point (DXGI_OUTPUT_DESC1).
    float red_primary_x = 0.0f;
    float red_primary_y = 0.0f;
    float green_primary_x = 0.0f;
    float green_primary_y = 0.0f;
    float blue_primary_x = 0.0f;
    float blue_primary_y = 0.0f;
    float white_point_x = 0.0f;
    float white_point_y = 0.0f;

    // Luminance range in nits (cd/m²).
    float max_luminance_nits = 0.0f; // reported peak luminance (high ⇒ HDR-capable panel)
    float min_luminance_nits = 0.0f;
    float max_full_frame_nits = 0.0f;
};

// Display↔Capture mapping (pure): resolve the DisplayHdrFacts for a capture
// target identified by its Windows display device name (e.g. "\\.\DISPLAY7").
// The caller is responsible for the impure step of turning a monitor CaptureTarget
// (an HMONITOR) into its device name via GetMonitorInfoW; this lookup stays a pure,
// UI-agnostic, thread-free query over the already-probed facts. Returns nullptr
// when no display with that name is present.
// Do not retain the returned pointer across a snapshot re-probe — it points into
// `displays` and is invalidated when that vector is rebuilt/reassigned.
[[nodiscard]] inline const DisplayHdrFacts* FindDisplayByName(const std::vector<DisplayHdrFacts>& displays,
                                                              std::string_view device_name) noexcept {
    for (const auto& d : displays) {
        if (d.name == device_name) {
            return &d;
        }
    }
    return nullptr;
}

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
