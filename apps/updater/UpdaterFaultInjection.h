#pragma once

// UpdaterFaultInjection.h -- one named, verification-only failure seam.
//
// WHY THIS EXISTS. Declining a UAC prompt is a product state with its own
// recovery path (FailureCase::UacDeclined, C1, Retry re-handoffs), and until now
// the only way to reach it was a person clicking No on the Secure Desktop. That
// made the release gate for it unrunnable without an operator standing at the
// machine, and it left the gate as the campaign's only reason to raise a prompt
// at all.
//
// WHAT IT MAY AND MAY NOT DO. The seam can only turn a step into a FAILURE that
// the product already models. It cannot skip a verification, cannot make an
// install succeed, cannot relax a signature check, and cannot reach any state
// that is not already reachable by a user answering a dialog. A fault that could
// make something succeed would be a security defect regardless of how it is
// gated; this one produces the same ERROR_CANCELLED the elevation call returns
// when a person declines, at the same call site, and nothing else.
//
// HOW IT IS ARMED. The EXOSNAP_UPDATER_FAULT environment variable, read at the
// call site rather than cached, so a launcher can arm it for exactly one child
// process. It is deliberately not a command-line flag: the updater is launched by
// the application through a handoff document, so a flag would have to travel
// through the document and become part of a signed, versioned contract.

#include <QString>

namespace exosnap::updater {

// The faults a verification run may inject. Every value must name a state the
// product reaches on its own; adding one that does not is the line this seam
// exists to stay behind.
enum class InjectedFault {
    None,
    // ShellExecuteExW(runas) behaves as if the operator declined: the elevation
    // call fails with ERROR_CANCELLED and the worker reports UacDeclined.
    UacDeclined,
};

// The canonical spelling of each fault, matched case-insensitively. An
// unrecognised value is None -- a typo must not silently arm a different fault,
// and a run that meant to inject one and did not fails its gate on the missing
// state rather than on a state nobody asked for.
[[nodiscard]] InjectedFault ParseInjectedFault(const QString& value);

// What EXOSNAP_UPDATER_FAULT names right now, or None when it is unset or empty.
[[nodiscard]] InjectedFault CurrentInjectedFault();

// The environment variable name, in one place, so the reader and the tests
// cannot drift apart.
[[nodiscard]] const char* InjectedFaultVariableName();

} // namespace exosnap::updater
