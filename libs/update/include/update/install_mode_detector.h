#pragma once

#include <optional>
#include <string>

#include <update/update_types.h>

namespace exosnap::update {

// A matching MSI registry record determines physical installation and ownership.
// Portable ownership uses adjacent Scoop metadata or a recognized Scoop layout.
// Empty application_directory uses the running executable directory.
[[nodiscard]] DistributionContext DetectDistributionContext(const std::wstring& application_directory = {}) noexcept;

enum class MsiInstallScope { User, Machine };

struct RegisteredInstallation {
    MsiInstallScope scope;
    bool setup_managed;
};

// Resolves scope only for the registered tree matching this exact directory.
[[nodiscard]] std::optional<RegisteredInstallation>
ReadRegisteredInstallation(const std::wstring& application_directory);

// Reports only the physical installation axis of DetectDistributionContext().
[[nodiscard]] InstallMode DetectInstallMode() noexcept;

// Reads InstallPath (REG_SZ) from Software\ExoSnap, then the legacy
// Software\Codexo\ExoSnap key. HKLM precedes HKCU. Missing values return nullopt.
[[nodiscard]] std::optional<std::wstring> ReadInstallPath(const std::wstring& matching_directory = {});

} // namespace exosnap::update
