#pragma once
// install_mode_detector.h -- Detect whether ExoSnap is running as an
// installed copy or a portable (ZIP-extracted) copy.
//
// Portable detection rule (ADR-0012):
//   An installed copy writes a registry key during MSI/NSIS setup:
//     HKLM\Software\ExoSnap\InstallPath  (or HKCU on per-user installs)
//   If the key is absent, we are running in portable mode.
//   Portable mode = update check may still show notifications but
//   download/install actions are disabled (notify-only).

#include <update/update_types.h>

namespace exosnap::update {

// Returns InstallMode::Installed if the registry key is present and the
// install path matches the executable's directory; otherwise Portable.
[[nodiscard]] InstallMode DetectInstallMode() noexcept;

} // namespace exosnap::update
