#include <recorder_core/interfaces/VideoEncoderFactory.h>

// Placeholder body for the parallel-split base commit -- Agent A wires the
// Nvidia -> NvencVideoEncoder branch (needs nvenc_video_encoder.h, which the
// factory header itself must NOT include, to keep IVideoEncoder consumers
// decoupled from the concrete backend).

namespace recorder_core {

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(exosnap::capability::AdapterVendor vendor) const {
    (void)vendor;
    return nullptr;
}

} // namespace recorder_core
