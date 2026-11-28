// package_verifier.cpp -- SHA-256 verification and installer handoff.

#include <update/package_verifier.h>

#include <cstdint>
#include <update/file_sha256.h>
#include <update/update_types.h>

#include "url_utils.h"

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace exosnap::update {
namespace {

// Case-insensitive equality of two hex digests (the manifest value may be upper-
// or lower-case; the computed one is lowercase).
bool HexEqualNoCase(std::string a, std::string b) noexcept {
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    };
    lower(a);
    lower(b);
    return a == b;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// VerifyPackage
// ---------------------------------------------------------------------------
VerifyResult VerifyPackage(const VerifyParams& params) noexcept {
    if (!std::filesystem::exists(params.file_path))
        return VerifyResult::PackageNotFound;

    std::string actual = Sha256HexOfFile(params.file_path);
    if (actual.empty()) {
        // Read error — treat as hash mismatch, delete partial file
        std::filesystem::remove(params.file_path);
        return VerifyResult::PackageHashMismatch;
    }

    if (!HexEqualNoCase(actual, params.expected_sha256_hex)) {
        // Security: delete the tampered/corrupted file immediately
        std::filesystem::remove(params.file_path);
        return VerifyResult::PackageHashMismatch;
    }

    return VerifyResult::Ok;
}

// ---------------------------------------------------------------------------
// VerifyPackageHandle
// ---------------------------------------------------------------------------
VerifyResult VerifyPackageHandle(void* file_handle, const std::string& expected_sha256_hex) noexcept {
    if (file_handle == nullptr || file_handle == INVALID_HANDLE_VALUE)
        return VerifyResult::PackageNotFound;

    const std::string actual = Sha256HexOfHandle(file_handle);
    if (actual.empty())
        return VerifyResult::PackageHashMismatch; // read error; caller owns the handle/file

    if (!HexEqualNoCase(actual, expected_sha256_hex))
        return VerifyResult::PackageHashMismatch; // NOT deleted here — caller holds the lock

    return VerifyResult::Ok;
}

// ---------------------------------------------------------------------------
// HandoffToInstaller
// ---------------------------------------------------------------------------
bool HandoffToInstaller(const std::string& installer_path) noexcept {
    // ShellExecuteW with "runas" to invoke UAC elevation for the NSIS/MSI installer.
    // The path arrives as UTF-8, so it needs CP_UTF8 widening -- a byte-wise
    // widen turns any non-ASCII character in the user's profile path into a path
    // ShellExecuteW cannot resolve, and the handoff then fails before the UAC
    // prompt with nothing to show for it.
    const std::wstring wide = Utf8ToWide(installer_path);
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns > 32 on success
    return reinterpret_cast<intptr_t>(result) > 32;
}

} // namespace exosnap::update
