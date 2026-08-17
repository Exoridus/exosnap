// tools/envctl/core/env_provider.h -- the seam between transaction logic and the host.
//
// Everything above this interface is pure and unit-tested against a fake; the
// real WinAPI lives in tools/envctl/win32. The interface is deliberately narrow:
// four calls, no handles, no ownership, no lifetimes crossing it.

#pragma once

#include <string>
#include <vector>

#include "env_types.h"

namespace exosnap::envctl {

struct ReadResult {
    bool ok{false};             // the value below is meaningful.
    bool device_present{false}; // the underlying device exists RIGHT NOW.
    std::string value;
    std::string error;
};

// The result of asking the host to change something.
//
// `accepted` means "the setter returned success". It emphatically does NOT mean
// "the machine changed". Windows display and audio setters routinely return
// S_OK / DISP_CHANGE_SUCCESSFUL and leave the state alone (policy override,
// driver clamp, a mode the panel silently substitutes). Verification is the
// transaction's job and is never skipped -- see EnvironmentTransaction, which
// re-reads every property after every apply and treats a mismatch as failure.
struct ApplyResult {
    bool accepted{false};
    std::string error;
};

class IEnvironmentProvider {
  public:
    virtual ~IEnvironmentProvider() = default;

    // Every property this provider can see on this machine right now, with its
    // capability classification. Mutation is only ever attempted for entries
    // whose capability satisfies IsMutable().
    virtual std::vector<PropertyDescriptor> Describe() const = 0;

    virtual ReadResult Read(const PropertyId& id) const = 0;

    virtual ApplyResult Apply(const PropertyId& id, const std::string& value) = 0;

    // Cheap existence probe used by the restore pre-flight. A restore never
    // starts while any device it owes a value to is missing.
    virtual bool DevicePresent(const std::string& device_alias) const = 0;
};

} // namespace exosnap::envctl
