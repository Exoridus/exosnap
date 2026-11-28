#pragma once

#include "Handle.h"

namespace exosnap::diagnostics::ipc {

// A guardian stalled before entry cannot observe its collector dying. Until
// the GUID handoff completes, the launcher owns that child's termination.
class StartingGuardian {
  public:
    explicit StartingGuardian(Handle process) : process_(std::move(process)) {
    }
    ~StartingGuardian() {
        if (process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process_.get(), 5000);
        }
    }
    [[nodiscard]] HANDLE get() const {
        return process_.get();
    }
    [[nodiscard]] Handle handoff() {
        return std::move(process_);
    }

  private:
    Handle process_;
};

} // namespace exosnap::diagnostics::ipc
