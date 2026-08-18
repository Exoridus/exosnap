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
    std::string name; // GDI device name, e.g. "\\.\DISPLAY7" (DXGI_OUTPUT_DESC1::DeviceName)

    // The same display's DisplayConfig monitorFriendlyDeviceName, e.g. "27GL850",
    // paired with `name` over one QueryDisplayConfig active path — the pairing
    // envctl already uses (tools/envctl/win32/env_win32_display.cpp).
    //
    // It exists because the two APIs that describe a monitor key it differently:
    // DXGI answers in GDI device names, and Qt's QScreen::name() answers in
    // friendly names on Windows. Carrying both is what lets a Qt-side screen be
    // joined to its DXGI facts by NAME rather than by list position. Empty when
    // DisplayConfig did not answer for this path, and an empty name matches
    // nothing — an unmatched display is a better answer than a guessed one.
    std::string friendly_name;

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

    // Windows "Automatic Color Management" state for this display
    // (DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO::wideColorEnforced). Distinct from
    // hdr_active: a live probe (2026-07-24) confirmed this toggles independently of
    // the HDR on/off switch and is not visible via IDXGIOutput6::GetDesc1 at all.
    // Not yet consulted by any decision — captured for a future Diagnostics fact.
    bool wide_color_enforced = false;
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

// Per-codec NVENC advanced-encode capability facts (B-frames, Lookahead,
// Temporal-AQ) — see docs/superpowers/plans/2026-07-23-encoder-quality-harness-s1-capability-probe.md.
// Only meaningful when NvidiaRuntimeFacts::nvenc_codec_probed is true AND the
// specific codec was advertised; every field defaults to "unsupported" so an
// unprobed or unadvertised codec never claims a generation-dependent feature.
struct NvencAdvancedEncodeFacts {
    int max_bframes = 0;
    int bframe_ref_mode = 0; // NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE: 0/1/2, SDK semantics
    bool lookahead = false;
    bool temporal_aq = false;
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

    // Per-GPU 4:4:4 (YUV444, 8-bit) encode support, probed via
    // NV_ENC_CAPS_SUPPORT_YUV444_ENCODE on the matched H.264/HEVC codec GUIDs.
    // Only meaningful when nvenc_codec_probed is true. AV1 has no 4:4:4 path so no
    // flag exists for it. Default false so a GPU that is not probed never claims
    // 4:4:4 beyond the ValidUnvalidated static baseline.
    bool nvenc_yuv444_h264 = false;
    bool nvenc_yuv444_hevc = false;

    // Per-codec advanced-encode facts (B-frames, Lookahead, Temporal-AQ).
    // Populated only for codecs this GPU actually advertised (nvenc_h264/_hevc/_av1);
    // unlike nvenc_yuv444_*, AV1 gets a real probe too (NVENC AV1 B-frames use the
    // same frameIntervalP mechanic as H.264/HEVC, just without a 4:4:4 path).
    NvencAdvancedEncodeFacts nvenc_adv_h264;
    NvencAdvancedEncodeFacts nvenc_adv_hevc;
    NvencAdvancedEncodeFacts nvenc_adv_av1;
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
    MfWebcamRuntimeFacts mf_webcam; // S4: webcam MF probe
    OsRuntimeFacts os;
    std::vector<DisplayHdrFacts> displays;
};

// Cheap adapter-identity read: just enough to know whether the GPU/driver
// underlying a persisted capability cache entry still matches the current
// system. DXGI-only (adapter LUID + WDDM user-mode driver version); no NVENC
// session, no Media Foundation — same cost class as QueryDisplayFacts(), safe
// to call synchronously on the UI thread. See CapabilityBuilder::QueryAdapterIdentity.
struct AdapterIdentity {
    int64_t adapter_luid = 0; // 0 when no real (non-software) adapter was found
    // WDDM user-mode driver version, formatted "A.B.C.D" (from
    // IDXGIAdapter::CheckInterfaceSupport). Empty when unavailable — a driver
    // that does not answer this (deprecated-but-still-functional) query
    // degrades the cache to matching on LUID + app version + schema alone.
    std::string driver_version;
};

} // namespace exosnap::capability
