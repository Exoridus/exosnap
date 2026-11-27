#pragma once

#include "Handle.h"

namespace exosnap::diagnostics::ipc {

// The guardian allocates the session identity before the collector may open ETW.
// Neither the application nor a command-line option can select a trace to stop.
struct GuardedSession {
    Handle guardian;
    GUID id{};
    Handle heartbeat;
};

GuardedSession StartGuardian();
int RunGuardian();

} // namespace exosnap::diagnostics::ipc
