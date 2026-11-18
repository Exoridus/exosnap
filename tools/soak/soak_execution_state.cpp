#include "soak_execution_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <stdexcept>

namespace exosnap::soak {

SoakExecutionState::SoakExecutionState()
    : previous_state_(SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) {
    if (previous_state_ == 0)
        throw std::runtime_error("Cannot keep the system and display awake for recording");
}

SoakExecutionState::~SoakExecutionState() {
    if (SetThreadExecutionState(previous_state_) == 0)
        std::fputs("[soak] failed to restore recording thread execution state\n", stderr);
}

} // namespace exosnap::soak
