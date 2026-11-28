#pragma once

#include "FileAcePolicy.h"

namespace exosnap::diagnostics::ipc {

// Use only after the current registry key's owner has passed the machine trust check.
// Inherited grants on every existing descendant are independently evaluated.
inline bool IsRegistryCreatorTemplate(ACE_HEADER& header) {
    if (header.AceType != ACCESS_ALLOWED_ACE_TYPE || header.AceFlags != CONTAINER_INHERIT_ACE ||
        InspectFileAce(header, MAXDWORD) != FileAceGrant::CheckTrustee)
        return false;
    auto& ace = reinterpret_cast<ACCESS_ALLOWED_ACE&>(header);
    return (ace.Mask & ~static_cast<DWORD>(KEY_ALL_ACCESS)) == 0 && IsWellKnownSid(&ace.SidStart, WinCreatorOwnerSid);
}

} // namespace exosnap::diagnostics::ipc
