#include <exosnap/engine/interfaces/VideoEncoderFactory.h>

#include "nvenc_video_encoder.h"

namespace exosnap::engine {
namespace {

EncoderBackend BackendForVendor(exosnap::capability::AdapterVendor vendor) noexcept {
    switch (vendor) {
    case exosnap::capability::AdapterVendor::Nvidia:
        return EncoderBackend::Nvenc;
    default:
        return EncoderBackend::None;
    }
}

} // namespace

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(exosnap::capability::AdapterVendor vendor,
                                                           const RecorderConfig& config) const {
    switch (BackendForVendor(vendor)) {
    case EncoderBackend::Nvenc: {
        auto encoder = std::make_unique<NvencVideoEncoder>();
        // P1-P7 is read only from the NVENC alternative; another backend's
        // tuning (or none) must never leak into the NVENC encoder.
        if (const NvencTuning* tuning = GetNvencTuning(config.backend_tuning)) {
            encoder->SetPreset(tuning->preset);
        }
        return encoder;
    }
    default:
        return nullptr;
    }
}

std::unique_ptr<IVideoEncoder>
VideoEncoderFactory::CreateForAdapter(uint32_t pci_vendor_id, const RecorderConfig& config, std::string& error) const {
    error.clear();
    auto encoder = Create(exosnap::capability::ClassifyVendor(pci_vendor_id), config);
    if (!encoder) {
        error = "No video encoder available for recording adapter PCI vendor " + std::to_string(pci_vendor_id);
    }
    return encoder;
}

} // namespace exosnap::engine
