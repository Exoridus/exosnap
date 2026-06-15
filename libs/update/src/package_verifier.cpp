// package_verifier.cpp -- SHA-256 verification and installer handoff.

#include <update/package_verifier.h>

#define WIN32_LEAN_AND_MEAN
#include <bcrypt.h>
#include <shellapi.h>
#include <windows.h> // Must come before bcrypt.h (defines LONG/NTSTATUS)
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace exosnap::update {
namespace {

// Compute SHA-256 of a file using BCrypt (available Windows Vista+).
// Returns lowercase hex string, or empty on failure.
std::string Sha256HexOfFile(const std::string& path) noexcept {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        DWORD n = static_cast<DWORD>(f.gcount());
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf), n, 0);
    }

    uint8_t digest[32]{};
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    char hex[65]{};
    for (int i = 0; i < 32; ++i)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex);
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

    // Case-insensitive comparison (expected may be uppercase from manifest)
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    if (lower(actual) != lower(params.expected_sha256_hex)) {
        // Security: delete the tampered/corrupted file immediately
        std::filesystem::remove(params.file_path);
        return VerifyResult::PackageHashMismatch;
    }

    return VerifyResult::Ok;
}

// ---------------------------------------------------------------------------
// HandoffToInstaller
// ---------------------------------------------------------------------------
bool HandoffToInstaller(const std::string& installer_path) noexcept {
    // ShellExecuteW with "runas" to invoke UAC elevation for the NSIS/MSI installer.
    std::wstring wide(installer_path.begin(), installer_path.end());
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns > 32 on success
    return reinterpret_cast<intptr_t>(result) > 32;
}

} // namespace exosnap::update
