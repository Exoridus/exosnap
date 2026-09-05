#pragma once
// install_mode_classify.h -- the portable-vs-installed decision, as a pure
// function over facts the caller has already read.
//
// Separated from install_mode_detector.cpp because the decision is the part
// worth testing and the registry read is the part that cannot be.

#include <optional>
#include <string>

#include <update/update_types.h>

namespace exosnap::update {

// Lower-cases and strips trailing separators so two spellings of the same
// directory compare equal. Windows paths are case-insensitive and the registry
// value may or may not carry a trailing backslash.
[[nodiscard]] std::wstring NormalizeDirForCompare(std::wstring path);

// The install-mode rule.
//
//   marker_present          HKLM/HKCU "installed" == 1
//   registry_install_dir    HKLM/HKCU "InstallPath", if set
//   running_exe_dir         directory of the running executable
//
// Installed requires BOTH the marker AND that this executable actually lives in
// the stamped install directory. A portable copy on a machine that also has an
// MSI install would otherwise inherit the marker and claim to be the installed
// copy -- which the updater then rejects as a registry mismatch, leaving the
// portable copy permanently unable to update itself.
//
// With the marker present but no InstallPath to compare against, the answer
// stays Installed: an install stamped by an older MSI that wrote no path is
// still an install, and guessing Portable there would offer a ZIP swap over a
// real installation.
[[nodiscard]] InstallMode ClassifyInstallMode(bool marker_present,
                                              const std::optional<std::wstring>& registry_install_dir,
                                              const std::wstring& running_exe_dir) noexcept;

} // namespace exosnap::update
