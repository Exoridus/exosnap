#include <recorder_core/interfaces/VideoEncoderFactory.h>

#include "nvenc_video_encoder.h"

namespace recorder_core {

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(exosnap::capability::AdapterVendor vendor) const {
    switch (vendor) {
    case exosnap::capability::AdapterVendor::Nvidia:
        return std::make_unique<NvencVideoEncoder>();
    default:
        return nullptr;
    }
}

} // namespace recorder_core
