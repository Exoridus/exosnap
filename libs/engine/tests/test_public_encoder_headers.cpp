#include <exosnap/engine/interfaces/IVideoEncoder.h>
#include <exosnap/engine/interfaces/VideoEncoderFactory.h>
#include <exosnap/engine/recorder_session.h>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace {

using Factory = exosnap::engine::VideoEncoderFactory;
using CreateSignature = std::unique_ptr<exosnap::engine::IVideoEncoder> (Factory::*)(
    uint32_t, const exosnap::engine::RecorderConfig&) const;

static_assert(std::is_same_v<decltype(&Factory::Create), CreateSignature>);

} // namespace
