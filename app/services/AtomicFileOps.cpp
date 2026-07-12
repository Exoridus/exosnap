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

std::filesystem::path MakeSiblingTempPath(const std::filesystem::path& target) {
    const std::filesystem::path dir = target.parent_path();
    const std::wstring base = target.filename().wstring();
    for (int n = 0; n < 100000; ++n) {
        std::filesystem::path candidate = dir / (base + L".part" + (n == 0 ? std::wstring() : std::to_wstring(n)));
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return dir / (base + L".part");
}

unsigned long AtomicReplaceInPlace(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
        return 0;
    return ::GetLastError();
}

} // namespace exosnap
