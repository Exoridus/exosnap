#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace exosnap::diagnostics {

// Injectable interface for querying free bytes on the drive that hosts a given
// path.  Production code uses Win32DiskSpaceProvider; tests inject a stub.
class IDiskSpaceProvider {
  public:
    virtual ~IDiskSpaceProvider() = default;

    // Returns the number of free bytes available on the volume that hosts
    // `path`, or nullopt when the volume could not be queried (path does not
    // exist, access denied, an unreachable UNC share).
    //
    // "Could not query" and "zero bytes free" are different answers and must not
    // share an encoding: a full disk reports 0 and has to stop the recording,
    // while an unqueryable share reports nullopt and must not.
    [[nodiscard]] virtual std::optional<uint64_t> FreeBytesForPath(const std::filesystem::path& path) const = 0;
};

// Win32-backed implementation using GetDiskFreeSpaceExW.
class Win32DiskSpaceProvider final : public IDiskSpaceProvider {
  public:
    [[nodiscard]] std::optional<uint64_t> FreeBytesForPath(const std::filesystem::path& path) const override;
};

} // namespace exosnap::diagnostics
