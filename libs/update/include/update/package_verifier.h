#pragma once
// package_verifier.h -- SHA-256 package hash verification and handoff.
//
// Security contract (ADR-0012):
//   - SHA-256 of the downloaded bytes is checked against manifest sha256_hex.
//   - No partial binary is retained: if verification fails, the temp file is
//     deleted before returning.
//   - Downgrade is re-checked here against the manifest minimum_accepted_version
//     as a second line of defence.
//   - Handoff to the installer is user-initiated (no silent restart).

#include <string>
#include <update/update_types.h>

namespace exosnap::update {

struct VerifyParams {
    std::string file_path;           // absolute path to the downloaded file
    std::string expected_sha256_hex; // 64 lowercase hex chars from manifest
};

// Verify the SHA-256 hash of a downloaded package file.
// If verification fails, the file at file_path is DELETED before returning.
// Returns VerifyResult::Ok on success, PackageHashMismatch or PackageNotFound otherwise.
[[nodiscard]] VerifyResult VerifyPackage(const VerifyParams& params) noexcept;

// Verify the SHA-256 hash of a package through an ALREADY-OPEN file handle
// (Win32 HANDLE, taken as void* so this header stays WinAPI-free). Hashing the
// bytes the caller already holds a handle to — rather than re-opening a path —
// is what closes the verify->consume TOCTOU window: when the caller keeps that
// handle open with a deny-write / deny-delete share mode, the exact bytes that
// hash here are the exact bytes later consumed, and no same-user process can
// swap them in between.
//
// Unlike VerifyPackage(), this NEVER deletes anything: the caller owns the
// handle (deletion while it is open would fail anyway) and decides what to do
// with the file on a mismatch. The handle's file pointer is rewound to the
// start before hashing. Returns Ok, PackageHashMismatch, or PackageNotFound
// (null / invalid handle).
[[nodiscard]] VerifyResult VerifyPackageHandle(void* file_handle, const std::string& expected_sha256_hex) noexcept;

// Launch the installer for the downloaded (and verified) package.
// The installer runs as a separate process; this function does NOT wait.
// Returns true if the process was successfully started.
[[nodiscard]] bool HandoffToInstaller(const std::string& installer_path) noexcept;

} // namespace exosnap::update
