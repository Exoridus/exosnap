#pragma once

#include "Bootstrap_h.h"
#include "Handle.h"
#include <array>
#include <bit>
#include <cstdint>
#include <string>

namespace exosnap::diagnostics::ipc {

inline constexpr GUID kBootstrapClass{0x70e064ca, 0xa7d4, 0x43ea, {0xa0, 0x75, 0xce, 0x43, 0x03, 0xbf, 0x12, 0xe5}};
inline constexpr wchar_t kBootstrapMoniker[] = L"Elevation:Administrator!new:{70E064CA-A7D4-43EA-A075-CE4303BF12E5}";
inline constexpr LONG kBootstrapVersion = 1;

// The same 128-bit one-use token identifies the existing local pipe on both sides.
inline LONG WireLong(ULONG value) { return std::bit_cast<LONG>(value); }
inline ULONG WireBits(LONG value) { return std::bit_cast<ULONG>(value); }

inline std::wstring BootstrapNonce(const std::array<ULONG, 4>& words) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring nonce;
    nonce.reserve(32);
    for (auto word : words)
        for (int shift = 28; shift >= 0; shift -= 4)
            nonce.push_back(hex[(word >> shift) & 15]);
    return nonce;
}

using CollectFunction = int (*)(const std::wstring&, DWORD, uint64_t);
int RunBootstrapServer(CollectFunction collect);
} // namespace exosnap::diagnostics::ipc
