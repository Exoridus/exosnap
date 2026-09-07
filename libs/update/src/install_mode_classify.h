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

// What one registry hive says about an installation.
//
// Read as a unit, and that is the whole point of the type: the marker and the
// path always come from the SAME key, so a marker found in HKLM can never be
// compared against a path found in HKCU. Reading the two facts independently
// made that pairing possible, and nothing in the comparison could notice.
//
//   (no stamp)          neither hive carries the "installed" marker
//   install_dir empty   the marker is set but "InstallPath" is not
//   install_dir set     the directory the installer stamped
struct InstallStamp {
    std::wstring install_dir;
};

// The install-mode rule: this copy is Installed only when a hive stamped an
// install directory AND this executable is running from it.
//
// A stamp is a fact about the MACHINE, not about this copy. A portable build on
// a machine that also has an MSI install sees the same marker, so the directory
// comparison is what separates "an install exists here" from "I am it".
//
// Everything that is not provably the install is Portable, because that is the
// direction that fails safely: a portable answer costs a directory rename that
// an installation refuses honestly, while a wrong Installed answer runs msiexec
// on behalf of a copy that could not show it is the installed one.
[[nodiscard]] InstallMode ClassifyInstallMode(const std::optional<InstallStamp>& stamp,
                                              const std::wstring& running_exe_dir) noexcept;

} // namespace exosnap::update
