#pragma once

#include <cstdint>
#include <cstdio>

namespace recorder_core {

// std::ftell()/std::fseek() use `long`, which is 32-bit even in a 64-bit
// Windows build (LLP64): querying or seeking to a file position >= 2 GiB
// either fails (ftell returns -1) or silently truncates the requested
// offset (fseek). Any file this process writes that can grow past 2 GiB —
// a long screen recording is a routine case — needs the MSVC 64-bit-safe
// equivalents instead.

// Returns the current file position, or -1 on error (matches ftell's contract).
[[nodiscard]] int64_t Ftell64(FILE* file) noexcept;

// Seeks to `offset` from `origin` (SEEK_SET/SEEK_CUR/SEEK_END). Returns 0 on
// success, nonzero on error (matches fseek's contract).
int Fseek64(FILE* file, int64_t offset, int origin) noexcept;

} // namespace recorder_core
