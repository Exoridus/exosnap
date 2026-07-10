#include "DisplayNumbering.h"

#include <windows.h>

#include <algorithm>
#include <cctype>

namespace exosnap {
namespace {

std::string TrimAscii(const std::string& value) {
    const auto is_space = [](const char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; };
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && is_space(*begin))
        ++begin;
    while (end != begin && is_space(*(end - 1)))
        --end;
    return std::string(begin, end);
}

bool StartsWithAsciiInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin(), [](const char a, const char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), len);
    return result;
}

} // namespace

std::unordered_map<std::wstring, int> BuildDisplaySequenceMap() {
    std::unordered_map<std::wstring, int> map;
    int seq = 1;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            map[dd.DeviceName] = seq++;
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    return map;
}

std::string SequentialDisplayLabel(const std::string& raw_description,
                                   const std::unordered_map<std::wstring, int>& sequence_map) {
    std::string value = TrimAscii(raw_description);
    if (value.empty()) {
        return "Display";
    }

    // The map is keyed by the full GDI device name exactly as GetMonitorInfo
    // reports it — look the raw description up before any trimming.
    const std::wstring wide = Utf8ToWide(value);
    if (!wide.empty()) {
        const auto it = sequence_map.find(wide);
        if (it != sequence_map.end()) {
            return "Display " + std::to_string(it->second);
        }
    }

    // Not in the current topology (stale target, display just left): fall back
    // to the raw trailing number rather than inventing one.
    if (StartsWithAsciiInsensitive(value, R"(\\.\)")) {
        value.erase(0, 4);
    } else if (StartsWithAsciiInsensitive(value, "//./")) {
        value.erase(0, 4);
    }

    if (value.size() > 7 && StartsWithAsciiInsensitive(value, "DISPLAY")) {
        const std::string suffix = value.substr(7);
        const bool digits_only = !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](const char ch) {
            return std::isdigit(static_cast<unsigned char>(ch)) != 0;
        });
        if (digits_only) {
            return "Display " + suffix;
        }
    }

    return value;
}

} // namespace exosnap
