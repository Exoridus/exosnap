#include "models/CaptureTargetPresentation.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

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

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

bool StartsWithAsciiInsensitive(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin(), [](const char a, const char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

bool IsInternalWindowToken(const std::string& value) {
    if (value.empty() || EqualsAsciiInsensitive(value, "unnamed") || EqualsAsciiInsensitive(value, "(unnamed)") ||
        StartsWithAsciiInsensitive(value, "hwnd:") || StartsWithAsciiInsensitive(value, "window:")) {
        return true;
    }
    std::size_t cursor = StartsWithAsciiInsensitive(value, "0x") ? 2 : 0;
    return value.size() - cursor >= 5 &&
           std::all_of(value.begin() + static_cast<std::ptrdiff_t>(cursor), value.end(),
                       [](const char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; });
}

std::wstring ToWide(const std::string& value) {
    if (value.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string ProcessName(const std::string& app_name) {
    std::string result;
    for (const unsigned char ch : app_name) {
        if (std::isalnum(ch) != 0)
            result.push_back(static_cast<char>(std::tolower(ch)));
    }
    return result.empty() ? std::string("window") : result;
}

struct WindowParts {
    std::string app;
    std::string title;
};

WindowParts ParseWindow(const std::string& description) {
    const std::string value = TrimAscii(description);
    if (IsInternalWindowToken(value))
        return {"Window", {}};

    const std::string separators[] = {" \xE2\x80\x94 ", " - "};
    std::size_t position = std::string::npos;
    std::size_t separator_size = 0;
    for (const std::string& separator : separators) {
        const std::size_t candidate = value.rfind(separator);
        if (candidate != std::string::npos && (position == std::string::npos || candidate > position)) {
            position = candidate;
            separator_size = separator.size();
        }
    }
    if (position == std::string::npos)
        return {value, {}};

    const std::string title = TrimAscii(value.substr(0, position));
    const std::string app = TrimAscii(value.substr(position + separator_size));
    if (IsInternalWindowToken(app))
        return {value, {}};
    if (IsInternalWindowToken(title) || EqualsAsciiInsensitive(app, title))
        return {app, {}};
    return {app, title};
}

CaptureTargetPresentation ResolveWindow(const exosnap::engine::CaptureTarget& target) {
    const WindowParts parts = ParseWindow(target.description);
    CaptureTargetPresentation result;
    result.kind = CaptureTargetPresentationKind::Window;
    result.app_name = parts.app;
    result.title = parts.title;
    result.label = parts.title.empty() ? parts.app : parts.app + " - " + parts.title;
    result.filename.app_name = ToWide(parts.app);
    result.filename.window_title = ToWide(parts.title.empty() ? parts.app : parts.title);
    result.filename.process_name = ToWide(ProcessName(parts.app));
    result.filename.target_name = ToWide(result.label);
    return result;
}

} // namespace

CaptureTargetPresentation
ResolveCaptureTargetPresentation(const exosnap::engine::CaptureTarget& target, CaptureTargetPresentationKind kind,
                                 const std::unordered_map<std::wstring, int>& display_sequence) {
    if (kind == CaptureTargetPresentationKind::Window)
        return ResolveWindow(target);

    const std::string display = SequentialDisplayLabel(target.description, display_sequence);
    CaptureTargetPresentation result;
    result.kind = kind;
    if (kind == CaptureTargetPresentationKind::Region) {
        result.label = "Region on " + display;
        result.app_name = "Region";
        result.title = display;
        result.filename.app_name = L"Region";
        result.filename.process_name = L"region";
    } else {
        result.label = "Desktop - " + display;
        result.app_name = "Desktop";
        result.title = display;
        result.filename.app_name = L"Desktop";
        result.filename.process_name = L"desktop";
    }
    result.filename.window_title = ToWide(display);
    result.filename.target_name = ToWide(result.label);
    return result;
}

CaptureTargetPresentation ResolveCaptureTargetPresentation(const exosnap::engine::CaptureTarget& target,
                                                           CaptureTargetPresentationKind kind) {
    return ResolveCaptureTargetPresentation(target, kind,
                                            kind == CaptureTargetPresentationKind::Window
                                                ? std::unordered_map<std::wstring, int>{}
                                                : BuildDisplaySequenceMap());
}

} // namespace exosnap
