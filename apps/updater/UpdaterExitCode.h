#pragma once

// UpdaterExitCode.h -- what exosnap-updater.exe tells its caller.
//
// Before this existed the process returned 0 for every run that reached
// app.exec(), including one whose update had failed: "Close" goes to
// QCoreApplication::quit(), and quit() is a normal exit. The app dutifully
// logged a number that meant nothing, and a release script could not fail on a
// failed update because there was nothing to fail on.
//
// The codes are few and stable on purpose. Each one answers a different question
// for whoever launched the process, and no two of them can be conflated:
// "did the update apply", "is a machine restart outstanding", "was there
// anything to do at all", "did I invoke this wrong", "did the user walk away".

#include <update/update_flow_state.h>

namespace exosnap::updater {

enum class UpdaterExit : int {
    // The update applied and was verified. Includes the soft success where the
    // automatic relaunch did not open (B4): the new version IS installed, and a
    // caller that treated that as a failure would be wrong about the machine.
    Success = 0,
    // A terminal failure. `failureCase` in the flow state says which one; the
    // exit code deliberately does not encode the matrix, because a caller that
    // branches on twelve numbers has re-implemented the matrix badly.
    UpdateFailed = 1,
    // The command line could not be understood. Unchanged from the historic
    // behaviour, so nothing that already checks for 2 has to be touched.
    UsageError = 2,
    // A manual check found nothing newer. Not a failure, and not a success
    // either -- no update was applied, so it must not be reported as one.
    UpToDate = 3,
    // The MSI upgrade applied and Windows must restart to finish (C3). A
    // terminal SUCCESS with an outstanding machine action, which is exactly why
    // it is not folded into 0.
    RebootRequired = 4,
    // The run stopped because it was asked to, or the window was closed before
    // any outcome. Neither a success nor a failure: nothing was installed and
    // nothing broke. A user cancellation that reported 1 would send a release
    // script looking for a fault that does not exist.
    Cancelled = 5,
};

// The exit code for the state the process ends on. Pure, so the whole contract
// is pinned by a table test rather than by launching the executable in every
// phase.
[[nodiscard]] int UpdaterExitCodeFor(const exosnap::update::UpdateFlowState& state);

} // namespace exosnap::updater