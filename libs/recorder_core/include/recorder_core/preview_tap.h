#pragma once

#include <cstdint>

#include <dxgiformat.h>

// ---------------------------------------------------------------------------
// The WYSIWYG preview tap publishes the engine's pre-encode surface to the
// preview renderer through a shared texture (see preview_shared_texture.h and
// ADR 0040). For an SDR or tone-mapped session that surface is already an SDR
// image; a native HDR10 session encodes straight from linear scRGB FP16, so the
// tapped surface needs a display transform on the consumer side before it can
// be drawn into an SDR swap chain.
//
// This header is the contract between the producer (VideoThread) and the
// consumer (the preview renderer): which transform the consumer must apply,
// and the pure decision of whether a session's surface can be tapped at all.
// D3D-free so the decision is unit-pinned.
// ---------------------------------------------------------------------------

namespace recorder_core {

// How the consumer must transform the shared surface before display.
enum class PreviewTapTransform : uint8_t {
    None,     // SDR surface (BGRA8 / R10G10B10A2): draw as-is
    ScrgbHdr, // linear scRGB FP16 from an HDR desktop: highlight roll-off +
              // sRGB OETF (HdrToneMapper with sdr_scrgb_source = false)
    ScrgbSdr, // linear scRGB FP16 from an SDR Advanced-Color desktop: sRGB OETF
              // only, no roll-off (HdrToneMapper with sdr_scrgb_source = true)
};

// Travels with the shared NT handle to the consumer. Small; passed by value.
struct PreviewTapDesc {
    PreviewTapTransform transform = PreviewTapTransform::None;
    // Display peak in reference-white multiples (HdrPeakScale in hdr_tonemap.h).
    // Meaningful for ScrgbHdr only; 1.0 otherwise.
    float peak_scale = 1.0f;
    // The OS SDR reference white in the same units (SdrPaperWhiteScale). An HDR
    // desktop renders SDR content at that level rather than at scRGB's nominal
    // 80 nits, so the consumer must divide by it before rolling off. Meaningful
    // for ScrgbHdr only -- an SDR Advanced-Color desktop is display-referred and
    // needs no correction -- and 1.0 everywhere else.
    float paper_white_scale = 1.0f;
};

struct PreviewTapPlan {
    // False only for the already-PQ R10G10B10A2 native sub-path: its surface is
    // non-linear PQ with no linear intermediate, and the preview keeps its own
    // WGC capture there (see the capture-hubs design doc).
    bool tap_enabled = true;
    PreviewTapDesc desc{};
};

// Pure: the display transform for a RAW captured desktop frame (an idle
// DXGI-hub source, no session policy applied). An FP16 desktop is linear scRGB:
// tone-mapped when the display is actively HDR, encoded without the roll-off
// when it is an SDR Advanced-Color desktop (no headroom to roll off -- the
// roll-off would crush its reference white; see OdCaptureMode::SdrScrgb).
// BGRA8 and the 10 bpc SDR desktop draw as-is. display_max_luminance_nits feeds
// HdrPeakScale and is only trusted while the display is HDR-active.
[[nodiscard]] PreviewTapDesc ResolveRawCaptureTapDesc(DXGI_FORMAT format, bool display_hdr_active,
                                                      float sdr_white_level_nits,
                                                      float display_max_luminance_nits) noexcept;

// Pure: decide whether a session's pre-encode surface can be tapped and which
// transform the consumer must apply. hdr_peak_scale is the session's already
// resolved HdrPeakScale() value; it is passed through for ScrgbHdr.
[[nodiscard]] inline PreviewTapPlan ResolvePreviewTapPlan(bool hdr_native_active, bool pq_input_is_pq,
                                                          float hdr_peak_scale) noexcept {
    PreviewTapPlan plan;
    if (!hdr_native_active) {
        return plan; // SDR / tone-mapped sessions tap an SDR surface: no transform
    }
    if (pq_input_is_pq) {
        plan.tap_enabled = false; // already-PQ 10-bit desktop: nothing linear to tap
        return plan;
    }
    plan.desc.transform = PreviewTapTransform::ScrgbHdr;
    plan.desc.peak_scale = hdr_peak_scale;
    return plan;
}

// Pure: whether the DXGI capture hub's publish loop must (re)create the shared
// texture and re-announce a fresh PreviewTapDesc to the preview consumer, vs.
// publishing the next frame into the existing shared texture unchanged. True
// whenever the shared texture is not yet valid, its dimensions/format no
// longer match the captured frame, or the display's HDR facts have changed
// since the tap was last resolved. An Advanced-Color desktop keeps delivering
// the same FP16 format across a live Windows-HDR (or Auto-HDR) toggle, so
// dimensions/format alone cannot detect that the previously-resolved tap
// (peak_scale / transform) has gone stale — the hdr_active / max_luminance
// comparison is what catches that case (see DxgiCaptureHubService::WorkerProc).
[[nodiscard]] inline bool ShouldRepublishCaptureTap(bool shared_valid, uint32_t shared_width, uint32_t shared_height,
                                                    DXGI_FORMAT shared_format, uint32_t frame_width,
                                                    uint32_t frame_height, DXGI_FORMAT frame_format,
                                                    bool last_hdr_active, float last_max_luminance_nits,
                                                    bool current_hdr_active,
                                                    float current_max_luminance_nits) noexcept {
    if (!shared_valid || frame_width != shared_width || frame_height != shared_height ||
        frame_format != shared_format) {
        return true;
    }
    return current_hdr_active != last_hdr_active || current_max_luminance_nits != last_max_luminance_nits;
}

} // namespace recorder_core
