#include "FilenameBuilder.h"

#include "OutputPathPolicy.h"
#include "RecordingProfile.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string_view>
#include <vector>

namespace exosnap {
namespace {

constexpr wchar_t kMissingTokenSentinel = 0xE000;

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
    if (c < 32) {
        return true;
    }

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

bool IsPatternPathSeparator(wchar_t c) {
    return c == L'/' || c == L'\\';
}

bool IsCleanupSeparator(wchar_t c) {
    return c == L'|' || c == L'.' || c == L',' || c == L':' || c == L';' || c == L'_' || c == L'-' || c == L'/' ||
           c == L'\\' || std::iswspace(c) != 0;
}

bool IsTrimSeparator(wchar_t c) {
    return c == L'|' || c == L'.' || c == L',' || c == L':' || c == L';' || c == L'_' || c == L'-' ||
           std::iswspace(c) != 0;
}

void TrimSegment(std::wstring& value) {
    std::size_t first = 0;
    while (first < value.size() && IsTrimSeparator(value[first])) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && IsTrimSeparator(value[last - 1])) {
        --last;
    }

    if (first == 0 && last == value.size()) {
        return;
    }

    value = value.substr(first, last - first);
}

bool IsWindowsReservedDeviceName(std::wstring_view segment) {
    const auto dot_pos = segment.find(L'.');
    const std::wstring_view stem = (dot_pos != std::wstring_view::npos) ? segment.substr(0, dot_pos) : segment;

    if (stem.size() == 3) {
        const wchar_t a = static_cast<wchar_t>(std::towlower(stem[0]));
        const wchar_t b = static_cast<wchar_t>(std::towlower(stem[1]));
        const wchar_t c = static_cast<wchar_t>(std::towlower(stem[2]));
        if (a == L'c' && b == L'o' && c == L'n')
            return true;
        if (a == L'p' && b == L'r' && c == L'n')
            return true;
        if (a == L'a' && b == L'u' && c == L'x')
            return true;
        if (a == L'n' && b == L'u' && c == L'l')
            return true;
    }
    if (stem.size() == 4) {
        const wchar_t a = static_cast<wchar_t>(std::towlower(stem[0]));
        const wchar_t b = static_cast<wchar_t>(std::towlower(stem[1]));
        const wchar_t c = static_cast<wchar_t>(std::towlower(stem[2]));
        const wchar_t d = stem[3];
        if (a == L'c' && b == L'o' && c == L'm' && d >= L'1' && d <= L'9')
            return true;
        if (a == L'l' && b == L'p' && c == L't' && d >= L'1' && d <= L'9')
            return true;
    }
    return false;
}

std::wstring SanitizeReservedDeviceName(std::wstring value) {
    if (IsWindowsReservedDeviceName(value)) {
        value += L"_1";
    }
    return value;
}

std::wstring SafeStrftime(const std::tm& tm_local, const wchar_t* format, const std::wstring& fallback) {
    std::array<wchar_t, 32> buffer{};
    if (wcsftime(buffer.data(), buffer.size(), format, &tm_local) != 0) {
        return buffer.data();
    }
    return fallback;
}

std::wstring BuildDateTimeToken(const std::tm& tm_local) {
    return SafeStrftime(tm_local, L"%Y-%m-%d_%H-%M-%S", L"1970-01-01_00-00-00");
}

std::wstring BuildFallbackBaseName(std::time_t timestamp) {
    std::tm tm_local{};
    localtime_s(&tm_local, &timestamp);
    return L"recording_" + BuildDateTimeToken(tm_local);
}

std::wstring SanitizeTokenValue(const std::wstring& value) {
    std::wstring sanitized = value;
    for (auto& c : sanitized) {
        if (IsInvalidWindowsFilenameChar(c)) {
            c = L'_';
        }
    }

    TrimSegment(sanitized);
    return sanitized;
}

std::wstring ResolveKnownToken(const std::wstring& token_name, const std::tm& tm_local, std::time_t timestamp,
                               capability::Container container, const FilenameTargetContext& context,
                               bool* recognized) {
    *recognized = true;

    if (token_name == L"date") {
        return SafeStrftime(tm_local, L"%Y-%m-%d", L"1970-01-01");
    }
    if (token_name == L"time") {
        return SafeStrftime(tm_local, L"%H-%M-%S", L"00-00-00");
    }
    if (token_name == L"datetime") {
        return BuildDateTimeToken(tm_local);
    }
    if (token_name == L"timestamp") {
        return std::to_wstring(static_cast<long long>(timestamp));
    }
    if (token_name == L"YYYY") {
        return SafeStrftime(tm_local, L"%Y", L"1970");
    }
    if (token_name == L"YY") {
        return SafeStrftime(tm_local, L"%y", L"70");
    }
    if (token_name == L"MM") {
        return SafeStrftime(tm_local, L"%m", L"01");
    }
    if (token_name == L"DD") {
        return SafeStrftime(tm_local, L"%d", L"01");
    }
    if (token_name == L"hh") {
        return SafeStrftime(tm_local, L"%H", L"00");
    }
    if (token_name == L"mm") {
        return SafeStrftime(tm_local, L"%M", L"00");
    }
    if (token_name == L"ss") {
        return SafeStrftime(tm_local, L"%S", L"00");
    }
    if (token_name == L"app") {
        return context.app_name;
    }
    if (token_name == L"title") {
        return context.window_title;
    }
    if (token_name == L"process") {
        return context.process_name;
    }
    if (token_name == L"target") {
        return context.target_name;
    }
    if (token_name == L"profile") {
        return context.profile_name;
    }
    if (token_name == L"container") {
        return ContainerToken(container);
    }
    if (token_name == L"video") {
        return CodecToken(context.video_codec);
    }
    if (token_name == L"audio") {
        return CodecToken(context.audio_codec);
    }

    *recognized = false;
    return {};
}

wchar_t PreferredSeparatorFromRun(const std::wstring& run) {
    for (auto it = run.rbegin(); it != run.rend(); ++it) {
        if (std::iswspace(*it) == 0) {
            return *it;
        }
    }
    return run.empty() ? L'\0' : run.front();
}

std::wstring CollapseMissingTokenSeparators(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());

    std::size_t i = 0;
    while (i < value.size()) {
        if (value[i] != kMissingTokenSentinel) {
            result.push_back(value[i]);
            ++i;
            continue;
        }

        std::wstring left_run;
        while (!result.empty() && IsCleanupSeparator(result.back())) {
            left_run.insert(left_run.begin(), result.back());
            result.pop_back();
        }

        while (i < value.size() && value[i] == kMissingTokenSentinel) {
            ++i;
        }

        std::wstring right_run;
        while (i < value.size() && IsCleanupSeparator(value[i])) {
            right_run.push_back(value[i]);
            ++i;
        }

        const bool has_left_content = !result.empty();
        const bool has_right_content = i < value.size();
        if (!has_left_content || !has_right_content) {
            continue;
        }

        wchar_t join_char = L'\0';
        if (!left_run.empty()) {
            join_char = PreferredSeparatorFromRun(left_run);
        } else if (!right_run.empty()) {
            join_char = PreferredSeparatorFromRun(right_run);
        }

        if (join_char != L'\0') {
            result.push_back(join_char);
        }
    }

    return result;
}

std::wstring RenderPatternSegment(const std::wstring& segment, const std::tm& tm_local, std::time_t timestamp,
                                  capability::Container container, const FilenameTargetContext& context) {
    std::wstring rendered;
    rendered.reserve(segment.size() + 32);

    std::size_t i = 0;
    while (i < segment.size()) {
        if (segment[i] != L'{') {
            rendered.push_back(segment[i]);
            ++i;
            continue;
        }

        const std::size_t closing = segment.find(L'}', i + 1);
        if (closing == std::wstring::npos) {
            rendered.push_back(segment[i]);
            ++i;
            continue;
        }

        const std::wstring token_name = segment.substr(i + 1, closing - i - 1);
        bool recognized = false;
        const std::wstring token_value =
            ResolveKnownToken(token_name, tm_local, timestamp, container, context, &recognized);
        if (!recognized) {
            rendered.append(segment, i, closing - i + 1);
            i = closing + 1;
            continue;
        }

        const std::wstring sanitized_token = SanitizeTokenValue(token_value);
        if (sanitized_token.empty()) {
            rendered.push_back(kMissingTokenSentinel);
        } else {
            rendered.append(sanitized_token);
        }

        i = closing + 1;
    }

    rendered = CollapseMissingTokenSeparators(rendered);
    rendered.erase(std::remove(rendered.begin(), rendered.end(), kMissingTokenSentinel), rendered.end());

    for (auto& c : rendered) {
        if (IsInvalidWindowsFilenameChar(c)) {
            c = L'_';
        }
    }

    TrimSegment(rendered);
    rendered = SanitizeReservedDeviceName(std::move(rendered));
    return rendered;
}

std::vector<std::wstring> BuildRelativeSegments(const std::wstring& pattern, const std::tm& tm_local,
                                                std::time_t timestamp, capability::Container container,
                                                const FilenameTargetContext& context) {
    const auto normalized_pattern = NormalizeFilenamePatternInput(pattern);
    const std::wstring effective_pattern = (normalized_pattern.result == FilenamePatternPolicyResult::Ok)
                                               ? normalized_pattern.normalized_pattern
                                               : L"recording_{datetime}";

    std::vector<std::wstring> segments;
    std::wstring raw_segment;

    const auto flush_segment = [&]() {
        std::wstring rendered = RenderPatternSegment(raw_segment, tm_local, timestamp, container, context);
        raw_segment.clear();
        if (rendered.empty()) {
            return;
        }
        if (rendered == L"." || rendered == L"..") {
            return;
        }
        segments.push_back(std::move(rendered));
    };

    for (const wchar_t c : effective_pattern) {
        if (IsPatternPathSeparator(c)) {
            flush_segment();
            continue;
        }
        raw_segment.push_back(c);
    }

    flush_segment();

    if (segments.empty()) {
        segments.push_back(BuildFallbackBaseName(timestamp));
    }

    return segments;
}

std::filesystem::path BuildRelativePath(const std::wstring& pattern, capability::Container container,
                                        std::time_t timestamp, const FilenameTargetContext& context) {
    std::tm tm_local{};
    localtime_s(&tm_local, &timestamp);
    std::vector<std::wstring> segments = BuildRelativeSegments(pattern, tm_local, timestamp, container, context);

    if (segments.empty()) {
        segments.push_back(BuildFallbackBaseName(timestamp));
    }

    const std::wstring extension = ContainerExtension(container);
    segments.back().append(extension);

    std::filesystem::path relative_path;
    for (const auto& segment : segments) {
        relative_path /= std::filesystem::path(segment);
    }
    return relative_path;
}

bool PathElementEqualCaseInsensitive(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    const std::wstring lhs_value = lhs.native();
    const std::wstring rhs_value = rhs.native();
    if (lhs_value.size() != rhs_value.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs_value.size(); ++i) {
        const wchar_t a = static_cast<wchar_t>(std::towlower(lhs_value[i]));
        const wchar_t b = static_cast<wchar_t>(std::towlower(rhs_value[i]));
        if (a != b) {
            return false;
        }
    }

    return true;
}

bool IsPathUnderFolder(const std::filesystem::path& candidate, const std::filesystem::path& folder) {
    if (folder.empty()) {
        return false;
    }

    auto folder_it = folder.begin();
    auto candidate_it = candidate.begin();

    for (; folder_it != folder.end(); ++folder_it, ++candidate_it) {
        if (candidate_it == candidate.end()) {
            return false;
        }
        if (!PathElementEqualCaseInsensitive(*candidate_it, *folder_it)) {
            return false;
        }
    }

    return true;
}

} // namespace

std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp) {
    return BuildFilename(pattern, container, timestamp, FilenameTargetContext{});
}

std::wstring BuildFilename(const std::wstring& pattern, capability::Container container, std::time_t timestamp,
                           const FilenameTargetContext& context) {
    return BuildRelativePath(pattern, container, timestamp, context).wstring();
}

std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp) {
    return BuildOutputPath(folder, pattern, container, timestamp, FilenameTargetContext{});
}

std::filesystem::path BuildOutputPath(const std::filesystem::path& folder, const std::wstring& pattern,
                                      capability::Container container, std::time_t timestamp,
                                      const FilenameTargetContext& context) {
    const std::filesystem::path normalized_folder = folder.lexically_normal();
    const std::filesystem::path relative_path = BuildRelativePath(pattern, container, timestamp, context);
    const std::filesystem::path candidate = (normalized_folder / relative_path).lexically_normal();

    if (IsPathUnderFolder(candidate, normalized_folder)) {
        return candidate;
    }

    const std::filesystem::path fallback =
        (normalized_folder / BuildRelativePath(L"recording_{datetime}", container, timestamp, FilenameTargetContext{}))
            .lexically_normal();
    return fallback;
}

} // namespace exosnap
