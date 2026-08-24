#include <exosnap/engine/interfaces/VideoEncoderFactory.h>

#include "nvenc_video_encoder.h"

namespace exosnap::engine {

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(exosnap::capability::AdapterVendor vendor) const {
    switch (vendor) {
    case exosnap::capability::AdapterVendor::Nvidia:
        return std::make_unique<NvencVideoEncoder>();
    default:
        return nullptr;
    }
}

} // namespace exosnap::engine
