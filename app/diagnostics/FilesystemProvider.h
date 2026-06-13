#pragma once

#include <filesystem>
#include <string>

namespace exosnap::diagnostics {

// ──────────────────────────────────────────────────────────────────────────────
// FilesystemProvider — injectable interface for querying the filesystem type of
// the volume that hosts a given path.
//
// Production code uses Win32FilesystemProvider (backed by GetVolumeInformationW).
// Tests inject a stub that returns a pre-configured filesystem name.
//
// The returned string is the raw filesystem name as reported by Windows
// (e.g. "FAT32", "NTFS", "exFAT").  An empty string indicates that the query
// failed (path does not exist, access denied, etc.).
// ──────────────────────────────────────────────────────────────────────────────

class IFilesystemProvider {
  public:
    virtual ~IFilesystemProvider() = default;

    // Returns the filesystem name for the volume that hosts `path` (e.g.
    // "FAT32", "NTFS", "exFAT").  Returns an empty string on failure.
    [[nodiscard]] virtual std::string FilesystemNameForPath(const std::filesystem::path& path) const = 0;
};

// Win32-backed implementation using GetVolumeInformationW.
class Win32FilesystemProvider final : public IFilesystemProvider {
  public:
    [[nodiscard]] std::string FilesystemNameForPath(const std::filesystem::path& path) const override;
};

} // namespace exosnap::diagnostics
