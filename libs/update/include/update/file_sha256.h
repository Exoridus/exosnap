#pragma once

#include <string>

namespace exosnap::update {

[[nodiscard]] std::string Sha256HexOfFile(const std::string& path) noexcept;

// Hash a borrowed Win32 file handle from the beginning without closing it.
[[nodiscard]] std::string Sha256HexOfHandle(void* file_handle) noexcept;

} // namespace exosnap::update
