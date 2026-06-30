#include "CapabilitySummary.h"

#include <capability/support_level.h>

namespace exosnap::diagnostics {

std::string SupportLevelString(capability::SupportLevel level) {
    switch (level) {
    case capability::SupportLevel::Available:
        return "Available";
    case capability::SupportLevel::ValidUnvalidated:
        return "Valid (unvalidated)";
    case capability::SupportLevel::NotImplemented:
        return "Not implemented";
    case capability::SupportLevel::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

std::string VideoCodecDisplayName(capability::VideoCodec v) {
    switch (v) {
    case capability::VideoCodec::H264Nvenc:
        return "H.264 (NVENC)";
    case capability::VideoCodec::HevcNvenc:
        return "HEVC (NVENC)";
    case capability::VideoCodec::Av1Nvenc:
        return "AV1 (NVENC)";
    }
    return "Unknown";
}

std::string AudioCodecDisplayName(capability::AudioCodec a) {
    switch (a) {
    case capability::AudioCodec::Opus:
        return "Opus";
    case capability::AudioCodec::AacMf:
        return "AAC (Media Foundation)";
    case capability::AudioCodec::Pcm:
        return "PCM";
    case capability::AudioCodec::Flac:
        return "FLAC";
    }
    return "Unknown";
}

std::string ContainerDisplayName(capability::Container c) {
    switch (c) {
    case capability::Container::Matroska:
        return "MKV / Matroska";
    case capability::Container::Mp4:
        return "MP4";
    case capability::Container::WebM:
        return "WebM";
    }
    return "Unknown";
}

CapabilitySummary CapabilitySummary::FromCapabilitySet(const capability::CapabilitySet& caps) {
    CapabilitySummary summary;

    // OS
    summary.entries.push_back(
        {"OS Version",
         caps.runtime.os.version_string.empty() ? caps.runtime.os.failure_detail : caps.runtime.os.version_string,
         "info", true});
    summary.entries.push_back(
        {"OS Build", std::to_string(caps.runtime.os.build_number), "info", caps.runtime.os.build_number > 0});

    // GPU
    if (!caps.runtime.nvidia.adapter_name.empty()) {
        summary.entries.push_back({"GPU", caps.runtime.nvidia.adapter_name, "info", true});
    } else if (!caps.gpu_adapter_name.empty()) {
        summary.entries.push_back({"GPU", caps.gpu_adapter_name, "info", true});
    }

    // Displays — per-monitor HDR status (informational; basis for a future HDR
    // recording gate). A high peak luminance implies an HDR-capable panel even
    // when Windows HDR is currently off.
    for (const auto& disp : caps.runtime.displays) {
        std::string value = disp.hdr_active ? "HDR on" : "HDR off";
        if (disp.bits_per_color > 0)
            value += " \xC2\xB7 " + std::to_string(disp.bits_per_color) + "-bit";
        if (disp.max_luminance_nits > 0.0f)
            value += " \xC2\xB7 peak " + std::to_string(static_cast<int>(disp.max_luminance_nits)) + " nits";
        summary.entries.push_back({disp.name.empty() ? "Display" : disp.name, value, "info", true});
    }

    // NVIDIA driver
    if (caps.runtime.nvidia.nvenc_api_version > 0) {
        summary.entries.push_back(
            {"NVIDIA Driver", "NVENC API v" + std::to_string(caps.runtime.nvidia.nvenc_api_version), "info", true});
    } else if (!caps.runtime.nvidia.failure_detail.empty()) {
        summary.entries.push_back({"NVIDIA Driver", caps.runtime.nvidia.failure_detail, "unavailable", false});
    }

    // NVENC
    summary.entries.push_back({"NVENC Available", caps.nvenc_dll_present ? "Yes" : "No",
                               caps.nvenc_dll_present ? "available" : "unavailable", caps.nvenc_dll_present});

    // Webcam (Media Foundation) — S4: mfplat.dll presence probe
    {
        const bool wc_ok = caps.mf_webcam_available;
        const std::string wc_detail =
            wc_ok ? "Yes"
                  : (!caps.runtime.mf_webcam.failure_detail.empty() ? caps.runtime.mf_webcam.failure_detail
                                                                    : "No — Media Feature Pack not installed");
        summary.entries.push_back({"Webcam (MF)", wc_detail, wc_ok ? "available" : "unavailable", wc_ok});
    }

    // Video codecs
    for (const auto& v :
         {capability::VideoCodec::H264Nvenc, capability::VideoCodec::HevcNvenc, capability::VideoCodec::Av1Nvenc}) {
        const auto& ann = caps.QueryVideoCodec(v);
        summary.entries.push_back({VideoCodecDisplayName(v), SupportLevelString(ann.level),
                                   capability::IsSelectable(ann.level) ? "available" : "unavailable",
                                   capability::IsSelectable(ann.level)});
    }

    // VP9 placeholder
    summary.entries.push_back({"VP9", "Not probed", "unavailable", false});

    // Audio codecs
    for (const auto& a : {capability::AudioCodec::Opus, capability::AudioCodec::AacMf, capability::AudioCodec::Pcm,
                          capability::AudioCodec::Flac}) {
        const auto& ann = caps.QueryAudioCodec(a);
        summary.entries.push_back({AudioCodecDisplayName(a), SupportLevelString(ann.level),
                                   capability::IsSelectable(ann.level) ? "available" : "unavailable",
                                   capability::IsSelectable(ann.level)});
    }

    // Containers
    for (const auto& c : {capability::Container::Matroska, capability::Container::Mp4, capability::Container::WebM}) {
        const auto& ann = caps.QueryContainer(c);
        summary.entries.push_back({ContainerDisplayName(c), SupportLevelString(ann.level),
                                   capability::IsSelectable(ann.level) ? "available" : "unavailable",
                                   capability::IsSelectable(ann.level)});
    }

    return summary;
}

} // namespace exosnap::diagnostics
