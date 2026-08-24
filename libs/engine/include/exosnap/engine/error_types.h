#pragma once

#include <string>

namespace exosnap::engine {

enum class ErrorPhase {
    None,
    Prepare,
    VideoCapture,
    VideoEncode,
    AudioCapture,
    AudioEncode,
    Mux,
    Finalize,
    Shutdown,
};

} // namespace exosnap::engine
