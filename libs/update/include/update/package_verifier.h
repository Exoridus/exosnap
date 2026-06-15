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

// Launch the installer for the downloaded (and verified) package.
// The installer runs as a separate process; this function does NOT wait.
// Returns true if the process was successfully started.
[[nodiscard]] bool HandoffToInstaller(const std::string& installer_path) noexcept;

} // namespace exosnap::update
