#include "CanonicalMachine.h"

#include <capability/capability_builder.h>

namespace exosnap::visual {
namespace {

constexpr const char* kAdapterName = "NVIDIA GeForce RTX 5070 Ti";
constexpr uint32_t kNvidiaVendorId = 0x10DEu;
// Renders as the vendor's own "581.29" through VendorDriverVersion(); the WDDM
// form is what Windows reports and therefore what a snapshot has to carry.
constexpr const char* kDriverVersion = "32.0.15.8129";

capability::NvencAdvancedEncodeFacts AdvancedEncode(int max_bframes) {
    capability::NvencAdvancedEncodeFacts facts;
    facts.max_bframes = max_bframes;
    facts.bframe_ref_mode = 2;
    facts.lookahead = true;
    facts.temporal_aq = true;
    return facts;
}

capability::DisplayHdrFacts PrimaryDisplay() {
    capability::DisplayHdrFacts display;
    display.name = "\\\\.\\DISPLAY1";
    display.friendly_name = "ExoSnap Reference Display";
    display.bits_per_color = 8;
    display.max_luminance_nits = 400.0f;
    display.min_luminance_nits = 0.5f;
    display.max_full_frame_nits = 400.0f;
    return display;
}

} // namespace

capability::RuntimeCapabilitySnapshot CanonicalMachineRuntimeSnapshot() {
    capability::RuntimeCapabilitySnapshot snapshot;

    snapshot.adapter.adapter_luid = 0x0000'0001'0000'2A17;
    snapshot.adapter.vendor_id = kNvidiaVendorId;
    snapshot.adapter.driver_version = kDriverVersion;

    snapshot.nvidia.nvenc_dll_present = true;
    snapshot.nvidia.nvenc_api_version_valid = true;
    snapshot.nvidia.nvenc_api_version = 0x0000'000Cu;
    snapshot.nvidia.adapter_name = kAdapterName;
    // Probed, not assumed: the per-codec flags below are only authoritative when
    // this is true, and the whole point of the fixture is that they are read.
    snapshot.nvidia.nvenc_codec_probed = true;
    snapshot.nvidia.nvenc_h264 = true;
    snapshot.nvidia.nvenc_hevc = true;
    snapshot.nvidia.nvenc_av1 = true;
    snapshot.nvidia.nvenc_yuv444_h264 = true;
    snapshot.nvidia.nvenc_yuv444_hevc = true;
    snapshot.nvidia.nvenc_adv_h264 = AdvancedEncode(4);
    snapshot.nvidia.nvenc_adv_hevc = AdvancedEncode(4);
    snapshot.nvidia.nvenc_adv_av1 = AdvancedEncode(3);

    snapshot.mf_webcam.available = true;

    snapshot.os.build_number = 26200;
    snapshot.os.version_string = "Windows 11";

    snapshot.displays.push_back(PrimaryDisplay());
    return snapshot;
}

capability::CapabilitySet CanonicalMachineCapabilities() {
    capability::CapabilitySet caps =
        capability::CapabilityBuilder::BuildEffectiveCapabilities(CanonicalMachineRuntimeSnapshot());
    // BuildEffectiveCapabilities deliberately leaves this false -- only a real,
    // just-completed probe may set it. The fixture stands in for exactly that,
    // and without it the recording coordinator refuses to authorize a start, so
    // every Record-adjacent scenario would still render a machine mid-scan.
    caps.probed = true;
    return caps;
}

std::vector<capability::AdapterInfo> CanonicalMachineAdapters() {
    capability::AdapterInfo adapter;
    adapter.name = kAdapterName;
    adapter.vendor = capability::AdapterVendor::Nvidia;
    adapter.kind = capability::AdapterKind::Discrete;
    adapter.vendor_id = kNvidiaVendorId;
    adapter.device_id = 0x2C05u;
    adapter.luid = 0x0000'0001'0000'2A17;
    adapter.dedicated_video_memory_bytes = 16ull * 1024 * 1024 * 1024;
    adapter.shared_system_memory_bytes = 16ull * 1024 * 1024 * 1024;
    return {adapter};
}

std::vector<capability::AdapterEncoderCapability> CanonicalMachineAdapterCapabilities() {
    capability::AdapterEncoderCapability capability;
    capability.probed = true;
    capability.backend_label = "NVENC";
    capability.provenance = "probed via NVENC encode GUIDs";
    capability.h264 = true;
    capability.hevc = true;
    capability.av1 = true;
    capability.yuv444_h264 = true;
    capability.yuv444_hevc = true;
    capability.max_bframes_h264 = 4;
    capability.max_bframes_hevc = 4;
    capability.max_bframes_av1 = 3;
    capability.lookahead_h264 = true;
    capability.lookahead_hevc = true;
    capability.lookahead_av1 = true;
    capability.temporal_aq_h264 = true;
    capability.temporal_aq_hevc = true;
    capability.temporal_aq_av1 = true;
    return {capability};
}

} // namespace exosnap::visual
