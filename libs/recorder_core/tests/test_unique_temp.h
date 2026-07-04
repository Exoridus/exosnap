// test_unique_temp.h — collision-free temporary paths for the recorder_core
// test binaries.
//
// Every one of these tests writes a real file (an .mkv / .mp4 / segment dir)
// under the system temp directory. A FIXED name such as
// `exosnap_stream_writer_test.mkv` races the moment two writers run at once:
//
//   * different worktrees run their own copy of the same binary in parallel and
//     share the one system temp dir, so both open the identical path; and
//   * historically, per-gtest-case CTest entries meant `ctest -j` launched
//     several cases of the same binary as separate processes concurrently.
//
// Both corrupt each other's reads and surface as re-run-green flakes. Fold a
// per-process random token, a monotonic counter, and the running gtest case
// name into the path so no two writers ever collide, regardless of process,
// worktree, or scheduling order. No platform headers required (the random token
// gives cross-process uniqueness without pulling in <windows.h>).
#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>

#include <gtest/gtest.h>

namespace exosnap_test {

// Returns an absolute path under the system temp dir, unique across processes,
// worktrees, and calls. `suffix` should carry the extension and any role hint,
// e.g. "src.mkv" or "out.mp4".
inline std::filesystem::path UniqueTempPath(const std::string& suffix) {
    // One random token per process (32 bits of entropy) disambiguates
    // concurrent processes/worktrees; the counter disambiguates repeated calls
    // within a process.
    static const unsigned s_token = [] {
        std::random_device rd;
        return rd();
    }();
    static std::atomic<unsigned> s_counter{0};

    std::string case_name = "anon";
    if (const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info()) {
        case_name = info->name();
    }

    const std::string name = "exosnap_" + case_name + "_" + std::to_string(s_token) + "_" +
                             std::to_string(s_counter.fetch_add(1)) + "_" + suffix;
    return std::filesystem::temp_directory_path() / name;
}

// Convenience overload returning std::string for the many call sites that hold
// paths as std::string.
inline std::string UniqueTempPathStr(const std::string& suffix) {
    return UniqueTempPath(suffix).string();
}

} // namespace exosnap_test
