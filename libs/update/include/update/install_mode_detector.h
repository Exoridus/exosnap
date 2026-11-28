#pragma once

#include <optional>
#include <string>

#include <update/update_types.h>

namespace exosnap::update {

// A matching MSI registry record determines physical installation and ownership.
// Portable ownership uses adjacent Scoop metadata or a recognized Scoop layout.
// Empty application_directory uses the running executable directory.
[[nodiscard]] DistributionContext DetectDistributionContext(const std::wstring& application_directory = {}) noexcept;

// Reports only the physical installation axis of DetectDistributionContext().
[[nodiscard]] InstallMode DetectInstallMode() noexcept;

// Returns the stamped path for the named application directory, or for the running
// executable when omitted. An unrelated registration never supplies a path.
[[nodiscard]] std::optional<std::wstring> ReadInstallPath(const std::wstring& application_directory = {});

} // namespace exosnap::update
