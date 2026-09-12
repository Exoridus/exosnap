#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "AtomicFileOps.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace exosnap {

bool PathsEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto norm = [](std::filesystem::path p) {
        std::wstring s = p.lexically_normal().generic_wstring();
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
        return s;
    };
    return norm(a) == norm(b);
}

std::filesystem::path MakeDisposableSiblingStagingPath(const std::filesystem::path& target) {
    const std::filesystem::path dir = target.parent_path();
    const std::wstring base = target.filename().wstring();
    for (int n = 0; n < 100000; ++n) {
        const std::wstring suffix = n == 0 ? L".tmp" : L"." + std::to_wstring(n) + L".tmp";
        const std::filesystem::path candidate = dir / (base + suffix);
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec)
            return candidate;
    }
    return dir / (base + L".tmp");
}

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target) {
    return MakeDisposableSiblingStagingPath(target);
}

unsigned long AtomicReplaceInPlace(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return 0;
    return ::GetLastError();
}

std::string DescribeFailedStagingRemoval(const std::filesystem::path& staging) {
    std::error_code ec;
    // remove() returning false with no error means the file was not there, which
    // is the normal case on an error path that failed before the output was
    // opened. Only "it is there and I could not delete it" is worth reporting.
    std::filesystem::remove(staging, ec);
    if (!ec)
        return {};

    std::error_code exists_ec;
    if (!std::filesystem::exists(staging, exists_ec) && !exists_ec)
        return {}; // gone by some other means; the remove error is moot.

    return "could not remove the staging file \"" + staging.filename().string() + "\": " + ec.message() +
           "; it is still on disk next to the recording";
}

} // namespace exosnap
