#pragma once

#include <cstdint>
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

// What kind of volume a path lives on (GetDriveType). Network and removable
// volumes stall writes in ways a local disk does not; a recorder should say so
// before the first dropped frame, not after.
enum class DriveKind : uint8_t { Unknown, Fixed, Removable, Remote, CdRom, RamDisk };

class IFilesystemProvider {
  public:
    virtual ~IFilesystemProvider() = default;

    // Returns the filesystem name for the volume that hosts `path` (e.g.
    // "FAT32", "NTFS", "exFAT").  Returns an empty string on failure.
    [[nodiscard]] virtual std::string FilesystemNameForPath(const std::filesystem::path& path) const = 0;

    // Kind of the volume that hosts `path`; Unknown when it cannot be told.
    [[nodiscard]] virtual DriveKind DriveKindForPath(const std::filesystem::path& /*path*/) const {
        return DriveKind::Unknown;
    }
};

// Win32-backed implementation using GetVolumeInformationW.
class Win32FilesystemProvider final : public IFilesystemProvider {
  public:
    [[nodiscard]] std::string FilesystemNameForPath(const std::filesystem::path& path) const override;
    [[nodiscard]] DriveKind DriveKindForPath(const std::filesystem::path& path) const override;
};

} // namespace exosnap::diagnostics
