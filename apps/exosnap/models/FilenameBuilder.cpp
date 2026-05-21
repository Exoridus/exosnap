#include "FilenameBuilder.h"

#include <array>

namespace exosnap {
namespace {

std::wstring ContainerExtension(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return L".mkv";
    case capability::Container::Mp4:
        return L".mp4";
    case capability::Container::WebM:
        return L".webm";
    default:
        return L".mkv";
    }
}

bool IsInvalidWindowsFilenameChar(wchar_t c) {
    switch (c) {
    case L'<':
    case L'>':
    case L':':
    case L'"':
    case L'/':
    case L'\\':
    case L'|':
    case L'?':
    case L'*':
        return true;
    default:
        return false;
    }
}

void ReplaceAll(std::wstring& text, const std::wstring& needle, const std::wstring& replacement) {
    if (needle.empty()) {
        return;
    }

    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::wstring::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

} // namespace

std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp) {
    std::tm tm_local{};
    localtime_s(&tm_local, &timestamp);

    std::array<wchar_t, 16> date_buf{};
    std::array<wchar_t, 16> time_buf{};
    std::wstring date_token = L"00000000";
    std::wstring time_token = L"000000";

    if (wcsftime(date_buf.data(), date_buf.size(), L"%Y%m%d", &tm_local) != 0) {
        date_token = date_buf.data();
    }
    if (wcsftime(time_buf.data(), time_buf.size(), L"%H%M%S", &tm_local) != 0) {
        time_token = time_buf.data();
    }

    std::wstring filename = pattern;
    ReplaceAll(filename, L"{date}", date_token);
    ReplaceAll(filename, L"{time}", time_token);

    for (auto& c : filename) {
        if (IsInvalidWindowsFilenameChar(c)) {
            c = L'_';
        }
    }

    filename += ContainerExtension(container);
    return filename;
}

std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp) {
    return folder / BuildFilename(pattern, container, timestamp);
}

} // namespace exosnap
