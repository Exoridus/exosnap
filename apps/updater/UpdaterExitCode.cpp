#include "UpdaterExitCode.h"

namespace exosnap::updater {

int UpdaterExitCodeFor(const exosnap::update::UpdateFlowState& state) {
    using exosnap::update::UpdatePhase;

    switch (state.phase) {
    case UpdatePhase::Completed:
    // B4: installed and verified; only the automatic start did not happen. The
    // update applied, so this is a success -- the difference is visible in the
    // flow state's failureCase, which is where a caller that cares looks.
    case UpdatePhase::RestartPending:
        return static_cast<int>(UpdaterExit::Success);
    case UpdatePhase::RebootRequired:
        return static_cast<int>(UpdaterExit::RebootRequired);
    case UpdatePhase::UpToDate:
        return static_cast<int>(UpdaterExit::UpToDate);
    case UpdatePhase::Failed:
        return static_cast<int>(UpdaterExit::UpdateFailed);
    // An intentional cancellation is terminal and shares the code with "closed
    // without an outcome", because it is the same statement: nothing was
    // installed, nothing failed.
    case UpdatePhase::Cancelled:
    // Every non-terminal phase means the process is ending without an outcome:
    // the window was closed while resting, while a check ran, or mid-pipeline.
    case UpdatePhase::Idle:
    case UpdatePhase::Checking:
    case UpdatePhase::UpdateAvailable:
    case UpdatePhase::Downloading:
    case UpdatePhase::ReadyToApply:
    case UpdatePhase::WaitingForParent:
    case UpdatePhase::Applying:
    case UpdatePhase::Verifying:
    case UpdatePhase::Launching:
        return static_cast<int>(UpdaterExit::Cancelled);
    }
    return static_cast<int>(UpdaterExit::Cancelled);
}

} // namespace exosnap::updater