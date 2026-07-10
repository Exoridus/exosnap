#pragma once

#include "adapter_enum.h"

#include <string>

namespace exosnap::capability {

// Real per-adapter encoder capability. Deliberately independent of
// capability::VideoCodec (that enum's members are named *Nvenc — meaningless
// for an AMD or Intel adapter); plain bool flags mirror the existing
// NvidiaRuntimeFacts convention in runtime_snapshot.h.
struct AdapterEncoderCapability {
    // True only when a real hardware probe executed and returned an
    // authoritative result. False means the per-codec flags below are not
    // meaningful (either the probe failed, or — for non-NVIDIA vendors — no
    // probe was attempted at all because no backend is wired yet).
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

    // Per-codec 8-bit 4:4:4 (YUV444) encode support, from
    // NV_ENC_CAPS_SUPPORT_YUV444_ENCODE. Only meaningful when probed==true; only
    // queried for a codec this adapter actually advertised. No AV1 field on
    // purpose — NVENC AV1 is 4:2:0 Main only.
    bool yuv444_h264 = false;
    bool yuv444_hevc = false;
};

// Vendor -> backend label, pure (no probing). NVIDIA => "NVENC"; AMD, Intel,
// and Other => "" (MVP has no AMF/QSV/software backend wired).
std::string EncoderBackendLabelForVendor(AdapterVendor vendor) noexcept;

// Probes real encoder capability for one adapter:
//   - NVIDIA: opens a real NVENC session on a D3D11 device created against
//     THIS adapter's LUID (re-enumerates DXGI to locate it — see adapter_enum.cpp)
//     and enumerates EncodeGUIDs, the same technique as runtime_query.cpp's
//     existing system-wide probe (A2), but targeted at a specific adapter
//     instead of "the first NVIDIA adapter DXGI happens to return first".
//     Best-effort: no NVENC DLL / no device / no session / vendor header
//     absent (headless build) all leave probed=false.
//   - AMD / Intel / Other: no probe is attempted — MVP has no AMD/AMF,
//     Intel/QSV, or software (x264/SVT-AV1) encoder backend wired. This is a
//     deliberate non-probe (probed=false, honest "not yet supported" message),
//     never a fabricated result.
AdapterEncoderCapability ProbeAdapterEncoderCapability(const AdapterInfo& adapter);

} // namespace exosnap::capability
