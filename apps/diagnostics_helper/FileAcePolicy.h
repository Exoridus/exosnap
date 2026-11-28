#pragma once

#include <windows.h>

#include <cstddef>

namespace exosnap::diagnostics::ipc {

enum class FileAceGrant { Denied, ReadOnly, CheckTrustee, Invalid };

inline FileAceGrant InspectFileAce(const ACE_HEADER& header, DWORD dangerous) {
    if (header.AceSize < sizeof(ACE_HEADER))
        return FileAceGrant::Invalid;
    if (header.AceType == ACCESS_DENIED_ACE_TYPE)
        return FileAceGrant::Denied;
    if (header.AceType != ACCESS_ALLOWED_ACE_TYPE && header.AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
        return FileAceGrant::Invalid;
    static_assert(offsetof(ACCESS_ALLOWED_CALLBACK_ACE, SidStart) == offsetof(ACCESS_ALLOWED_ACE, SidStart));
    constexpr size_t minimum = offsetof(ACCESS_ALLOWED_ACE, SidStart) + offsetof(SID, SubAuthority);
    if (header.AceSize < minimum)
        return FileAceGrant::Invalid;
    const auto& ace = reinterpret_cast<const ACCESS_ALLOWED_ACE&>(header);
    const auto& sid = *reinterpret_cast<const SID*>(&ace.SidStart);
    if (sid.Revision != SID_REVISION || sid.SubAuthorityCount > SID_MAX_SUB_AUTHORITIES ||
        header.AceSize < minimum + sizeof(DWORD) * sid.SubAuthorityCount)
        return FileAceGrant::Invalid;
    if (header.AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
        // A callback can restrict these rights further; it cannot add any that its mask omits.
        constexpr DWORD read_only = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | GENERIC_READ | GENERIC_EXECUTE;
        return (ace.Mask & ~read_only) == 0 ? FileAceGrant::ReadOnly : FileAceGrant::Invalid;
    }
    return (ace.Mask & dangerous) == 0 ? FileAceGrant::ReadOnly : FileAceGrant::CheckTrustee;
}

} // namespace exosnap::diagnostics::ipc
