#pragma once

namespace exosnap::engine {

// How a preflight meter service reports the result of opening its audio
// endpoint.
//
// Both meter services already do the WASAPI work on their own worker thread —
// what made the start expensive for a GUI caller was that `Start()` then blocked
// on the worker's promise until the endpoint was open. Measured on the Record
// page's activation binding: 106-147 ms at startup and ~43 ms on every return to
// Record, i.e. 3-9 dropped frames at 60 Hz, in a path where nothing depends on
// the answer.
enum class MeterStartMode {
    // Block until the worker has opened (or failed to open) the endpoint, and
    // return that verdict. The contract every caller had before this enum
    // existed, and the one the service's own tests assert against.
    AwaitOpen,
    // Return as soon as the worker thread is running. `true` then means "the
    // worker started", not "the endpoint is open"; a failed open clears
    // `IsRunning()` shortly afterwards, which is what a later start attempt
    // already consults. Only for callers on a thread that must not block —
    // there is no path in the product where a meter's readiness gates anything,
    // least of all recording admission, which stops the meters outright.
    Deferred,
};

} // namespace exosnap::engine
