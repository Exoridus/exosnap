#pragma once

#include "adapter_enum.h"
#include "encoder_format_capability.h"

#include <string>

namespace exosnap::capability {

// Real per-adapter encoder capability, tied to the device opened by the probe.
struct AdapterEncoderCapability {
    // Boot-scoped identity of the device the probe actually opened.
    int64_t adapter_luid = 0;
    // True only when codec enumeration returned an authoritative result.
    // Individual format queries can still be unavailable. False means the
    // codec-advertisement flags are not meaningful.
    bool probed = false;

    // Human-readable encoder backend name, e.g. "NVENC". Empty when this
    // vendor has no wired encoder backend in this build.
    std::string backend_label;

    // Provenance sentence shown in the UI, e.g. "probed via NVENC encode
    // GUIDs" or "encoder backend not yet supported". Always populated.
    std::string provenance;

    bool h264 = false;
    bool hevc = false;
    bool av1 = false;

    CodecFormatCapabilities formats_h264;
    CodecFormatCapabilities formats_hevc;
    CodecFormatCapabilities formats_av1;

    // Convenience flags for the exact AYUV/profile/4:4:4 evidence above.
    // False alone does not distinguish an unavailable query from rejection.
    bool yuv444_h264 = false;
    bool yuv444_hevc = false;

    // Authoritative per-codec facts from this adapter and its advertised codec GUIDs.
    int max_bframes_h264 = 0;
    int max_bframes_hevc = 0;
    int max_bframes_av1 = 0;
    int bframe_ref_mode_h264 = 0;
    int bframe_ref_mode_hevc = 0;
    int bframe_ref_mode_av1 = 0;
    bool lookahead_h264 = false;
    bool lookahead_hevc = false;
    bool lookahead_av1 = false;
    bool temporal_aq_h264 = false;
    bool temporal_aq_hevc = false;
    bool temporal_aq_av1 = false;
};

// Vendor -> backend label, pure (no probing). NVIDIA => "NVENC"; AMD, Intel,
// and Other => "" (MVP has no AMF/QSV/software backend wired).
std::string EncoderBackendLabelForVendor(AdapterVendor vendor) noexcept;

// Probes real encoder capability for one adapter:
//   - NVIDIA: opens a real NVENC session on a D3D11 device created against
//     THIS adapter's LUID (re-enumerates DXGI to locate it — see adapter_enum.cpp)
//     and enumerates codecs, input formats, required profiles and encode caps.
//     Best-effort: no NVENC DLL / no device / no session / vendor header
//     absent (headless build) all leave probed=false.
//   - AMD / Intel / Other: no probe is attempted — MVP has no AMD/AMF,
//     Intel/QSV, or software (x264/SVT-AV1) encoder backend wired. This is a
//     deliberate non-probe (probed=false, honest "not yet supported" message),
//     never a fabricated result.
AdapterEncoderCapability ProbeAdapterEncoderCapability(const AdapterInfo& adapter);

} // namespace exosnap::capability
