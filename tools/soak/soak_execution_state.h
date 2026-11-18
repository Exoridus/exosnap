#pragma once

#include <cstdint>

namespace exosnap::soak {

// Keep this lease on its creating thread: Windows execution state is thread-local.
class SoakExecutionState {
  public:
    SoakExecutionState();
    ~SoakExecutionState();
    SoakExecutionState(const SoakExecutionState&) = delete;
    SoakExecutionState& operator=(const SoakExecutionState&) = delete;

  private:
    uint32_t previous_state_ = 0;
};

} // namespace exosnap::soak
