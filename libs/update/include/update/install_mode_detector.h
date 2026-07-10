#pragma once
// install_mode_detector.h -- Detect whether ExoSnap is running as an
// installed copy or a portable (ZIP-extracted) copy.
//
// Portable detection rule (ADR-0012):
//   An installed copy is stamped by the MSI under
//     HKLM\Software\Codexo\ExoSnap  (or HKCU on per-user installs):
//       "installed"   (REG_DWORD == 1)  -- presence marker
//       "InstallPath" (REG_SZ)          -- install root ([INSTALLFOLDER])
//   If the marker is absent, we are running in portable mode.
//   Portable mode = update check may still show notifications; the ZIP
//   package is applied via a staged swap.

#include <optional>
#include <string>

#include <update/update_types.h>

namespace exosnap::update {

// Returns InstallMode::Installed when the "installed" marker (REG_DWORD == 1)
// is present under HKLM (then HKCU); otherwise Portable.
[[nodiscard]] InstallMode DetectInstallMode() noexcept;

// Registry Software\Codexo\ExoSnap value "InstallPath" (REG_SZ), HKLM then
// HKCU; nullopt if unset.
[[nodiscard]] std::optional<std::wstring> ReadInstallPath();

} // namespace exosnap::update
