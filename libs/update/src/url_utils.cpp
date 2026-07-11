#include "url_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap::update {

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty())
        return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

} // namespace exosnap::update
