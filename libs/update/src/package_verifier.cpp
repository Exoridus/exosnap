// package_verifier.cpp -- SHA-256 verification and installer handoff.

#include <update/package_verifier.h>

// clang-format off
// windows.h must come first: it defines LONG/NTSTATUS used by bcrypt.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#pragma comment(lib, "bcrypt.lib")
// clang-format on

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace exosnap::update {
namespace {

// Compute SHA-256 over a caller-supplied byte source using BCrypt (Vista+).
// `read_block(buf, cap, produced)` fills up to `cap` bytes, sets `produced`, and
// returns false on a read error (produced == 0 signals EOF). Returns lowercase
// hex, or empty on any failure. Both the file-path and the open-HANDLE hashers
// share this so the digest of a path and of a held handle are computed
// identically.
template <class ReadBlock> std::string Sha256HexOfSource(ReadBlock&& read_block) noexcept {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    char buf[65536];
    for (;;) {
        size_t produced = 0;
        if (!read_block(buf, sizeof(buf), produced)) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return {};
        }
        if (produced == 0)
            break;
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf), static_cast<ULONG>(produced), 0);
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

// Compute SHA-256 of a file opened by path. Returns lowercase hex, or empty.
std::string Sha256HexOfFile(const std::string& path) noexcept {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return Sha256HexOfSource([&f](char* buf, size_t cap, size_t& produced) noexcept {
        // fail() (short of eof) is a genuine read error; eof with a final
        // partial block is normal. gcount() carries the last read's byte count.
        f.read(buf, static_cast<std::streamsize>(cap));
        produced = static_cast<size_t>(f.gcount());
        return !f.bad();
    });
}

// Compute SHA-256 of an already-open file HANDLE, from the start. The handle is
// borrowed (never closed here). Returns lowercase hex, or empty on any error.
std::string Sha256HexOfHandle(HANDLE h) noexcept {
    LARGE_INTEGER origin{};
    if (::SetFilePointerEx(h, origin, nullptr, FILE_BEGIN) == 0)
        return {};
    return Sha256HexOfSource([h](char* buf, size_t cap, size_t& produced) noexcept {
        DWORD read = 0;
        if (::ReadFile(h, buf, static_cast<DWORD>(cap), &read, nullptr) == 0)
            return false;
        produced = static_cast<size_t>(read);
        return true;
    });
}

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

    const std::string actual = Sha256HexOfHandle(static_cast<HANDLE>(file_handle));
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
    std::wstring wide(installer_path.begin(), installer_path.end());
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns > 32 on success
    return reinterpret_cast<intptr_t>(result) > 32;
}

} // namespace exosnap::update
