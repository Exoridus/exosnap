#pragma once

#include <cstdint>
#include <filesystem>

namespace exosnap::diagnostics {

// Injectable interface for querying free bytes on the drive that hosts a given
// path.  Production code uses Win32DiskSpaceProvider; tests inject a stub.
class IDiskSpaceProvider {
  public:
    virtual ~IDiskSpaceProvider() = default;

    // Returns the number of free bytes available on the volume that hosts
    // `path`.  On failure (path does not exist, access denied, etc.) returns 0.
    [[nodiscard]] virtual uint64_t FreeBytesForPath(const std::filesystem::path& path) const = 0;
};

// Win32-backed implementation using GetDiskFreeSpaceExW.
class Win32DiskSpaceProvider final : public IDiskSpaceProvider {
  public:
    [[nodiscard]] uint64_t FreeBytesForPath(const std::filesystem::path& path) const override;
};

} // namespace exosnap::diagnostics
