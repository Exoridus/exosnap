#pragma once

#include <string>

namespace recorder_core {

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

} // namespace recorder_core
