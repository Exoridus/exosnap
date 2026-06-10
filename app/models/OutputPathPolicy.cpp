#include "OutputPathPolicy.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <unordered_set>
#include <vector>

namespace exosnap {
namespace {

constexpr std::array<const wchar_t*, 6> kAllowedEnvNames = {
    L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA", L"PUBLIC", L"TEMP", L"TMP",
};

std::wstring Trim(const std::wstring& value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]) != 0) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]) != 0) {
        --last;
    }

    return value.substr(first, last - first);
}

std::wstring ToUpperAscii(const std::wstring& value) {
    std::wstring upper;
    upper.reserve(value.size());
    for (const wchar_t c : value) {
        upper.push_back(static_cast<wchar_t>(std::towupper(c)));
    }
    return upper;
}

bool StartsWith(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool IsAllowedEnvName(const std::wstring& name) {
    const std::wstring upper = ToUpperAscii(name);
    for (const wchar_t* allowed : kAllowedEnvNames) {
        if (upper == allowed) {
            return true;
        }
    }
    return false;
}

std::optional<std::wstring> ReadEnvironmentValue(const std::wstring& variable_name) {
    const DWORD size = GetEnvironmentVariableW(variable_name.c_str(), nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }

    std::wstring value(static_cast<std::size_t>(size - 1), L'\0');
    const DWORD written = GetEnvironmentVariableW(variable_name.c_str(), value.data(), size);
    if (written == 0) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::wstring> ExpandAllowedEnvironmentVariables(const std::wstring& input,
                                                              OutputFolderPolicyResult* out_error) {
    std::wstring expanded;
    expanded.reserve(input.size());

    std::size_t cursor = 0;
    while (cursor < input.size()) {
        const wchar_t ch = input[cursor];
        if (ch != L'%') {
            expanded.push_back(ch);
            ++cursor;
            continue;
        }

        const std::size_t end = input.find(L'%', cursor + 1);
        if (end == std::wstring::npos) {
            *out_error = OutputFolderPolicyResult::InvalidPath;
            return std::nullopt;
        }

        const std::wstring name = input.substr(cursor + 1, end - cursor - 1);
        if (!IsAllowedEnvName(name)) {
            *out_error = OutputFolderPolicyResult::UnsupportedEnvironmentVariable;
            return std::nullopt;
        }

        const auto env_value = ReadEnvironmentValue(name);
        if (!env_value.has_value()) {
            *out_error = OutputFolderPolicyResult::InvalidPath;
            return std::nullopt;
        }

        expanded.append(*env_value);
        cursor = end + 1;
    }

    return expanded;
}

std::optional<std::wstring> ExpandHomeAlias(const std::wstring& input, OutputFolderPolicyResult* out_error) {
    if (!StartsWith(input, L"~/") && !StartsWith(input, L"~\\")) {
        return input;
    }

    const auto user_profile = ReadEnvironmentValue(L"USERPROFILE");
    if (!user_profile.has_value()) {
        *out_error = OutputFolderPolicyResult::InvalidPath;
        return std::nullopt;
    }

    std::wstring expanded = *user_profile;
    const std::wstring suffix = input.substr(2);
    if (!suffix.empty()) {
        if (!expanded.empty() && expanded.back() != L'\\' && expanded.back() != L'/') {
            expanded.push_back(L'\\');
        }
        expanded.append(suffix);
    }
    return expanded;
}

bool IsDriveAbsolutePath(const std::wstring& input) {
    return input.size() >= 3 && std::iswalpha(input[0]) != 0 && input[1] == L':' &&
           (input[2] == L'\\' || input[2] == L'/');
}

bool IsUncPath(const std::wstring& input) {
    return StartsWith(input, L"\\\\") || StartsWith(input, L"//");
}

bool IsAbsoluteWindowsPath(const std::wstring& input) {
    if (IsDriveAbsolutePath(input) || IsUncPath(input)) {
        return true;
    }

    const std::filesystem::path path(input);
    return path.is_absolute();
}

std::wstring StripTrailingSeparatorsPreservingRoot(const std::wstring& input) {
    std::wstring normalized = input;
    for (wchar_t& c : normalized) {
        if (c == L'/') {
            c = L'\\';
        }
    }

    const std::filesystem::path path(normalized);
    const std::wstring root = path.root_path().native();
    while (normalized.size() > root.size() && !normalized.empty() &&
           (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    return normalized;
}

std::vector<std::wstring> SplitBySlash(const std::wstring& value) {
    std::vector<std::wstring> segments;
    std::wstring current;

    for (const wchar_t ch : value) {
        if (ch == L'/') {
            segments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }

    segments.push_back(current);
    return segments;
}

bool LooksLikeFullFilePath(const std::wstring& input) {
    const std::wstring normalized = input;
    const std::filesystem::path path(normalized);
    return path.has_filename() && path.has_extension();
}

bool SegmentHasToken(const std::wstring& segment) {
    return segment.find(L"{") != std::wstring::npos && segment.find(L"}") != std::wstring::npos;
}

std::wstring NormalizePatternPrefix(const std::wstring& raw_pattern) {
    std::wstring pattern = raw_pattern;
    while (true) {
        if (StartsWith(pattern, L"./") || StartsWith(pattern, L".\\")) {
            pattern.erase(0, 2);
            continue;
        }
        if (!pattern.empty() && (pattern.front() == L'/' || pattern.front() == L'\\')) {
            pattern.erase(pattern.begin());
            continue;
        }
        break;
    }
    return pattern;
}

} // namespace

NormalizedOutputFolder NormalizeOutputFolderInput(const std::wstring& raw_input) {
    NormalizedOutputFolder normalized;
    const std::wstring trimmed = Trim(raw_input);
    if (trimmed.empty()) {
        normalized.result = OutputFolderPolicyResult::EmptyInput;
        return normalized;
    }

    if (trimmed.find(L"$env:") != std::wstring::npos || trimmed.find(L"$") != std::wstring::npos) {
        normalized.result = OutputFolderPolicyResult::UnsupportedExpression;
        return normalized;
    }

    OutputFolderPolicyResult error = OutputFolderPolicyResult::Ok;
    const auto home_expanded = ExpandHomeAlias(trimmed, &error);
    if (!home_expanded.has_value()) {
        normalized.result = error;
        return normalized;
    }

    const auto env_expanded = ExpandAllowedEnvironmentVariables(*home_expanded, &error);
    if (!env_expanded.has_value()) {
        normalized.result = error;
        return normalized;
    }

    if (!IsAbsoluteWindowsPath(*env_expanded)) {
        normalized.result = OutputFolderPolicyResult::NotAbsolutePath;
        return normalized;
    }

    const std::wstring trimmed_separators = StripTrailingSeparatorsPreservingRoot(*env_expanded);
    std::filesystem::path path(trimmed_separators);
    path = path.lexically_normal();
    if (path.empty()) {
        normalized.result = OutputFolderPolicyResult::InvalidPath;
        return normalized;
    }

    normalized.result = OutputFolderPolicyResult::Ok;
    normalized.resolved_path = path;
    normalized.normalized_input = path.wstring();
    return normalized;
}

NormalizedFilenamePattern NormalizeFilenamePatternInput(const std::wstring& raw_pattern) {
    NormalizedFilenamePattern normalized;
    const std::wstring trimmed = Trim(raw_pattern);
    if (trimmed.empty()) {
        normalized.result = FilenamePatternPolicyResult::EmptyInput;
        return normalized;
    }

    if (trimmed.find(L"%") != std::wstring::npos) {
        normalized.result = FilenamePatternPolicyResult::UnsupportedEnvironmentVariable;
        return normalized;
    }
    if (trimmed.find(L"$env:") != std::wstring::npos || trimmed.find(L"$") != std::wstring::npos) {
        normalized.result = FilenamePatternPolicyResult::UnsupportedExpression;
        return normalized;
    }
    if (StartsWith(trimmed, L"~/") || StartsWith(trimmed, L"~\\")) {
        normalized.result = FilenamePatternPolicyResult::UnsupportedHomeAlias;
        return normalized;
    }

    std::wstring slash_normalized = trimmed;
    for (wchar_t& c : slash_normalized) {
        if (c == L'\\') {
            c = L'/';
        }
    }

    if (IsDriveAbsolutePath(slash_normalized) || IsUncPath(slash_normalized)) {
        normalized.result = FilenamePatternPolicyResult::AbsolutePath;
        return normalized;
    }

    slash_normalized = NormalizePatternPrefix(slash_normalized);
    const std::vector<std::wstring> raw_segments = SplitBySlash(slash_normalized);

    std::vector<std::wstring> normalized_segments;
    normalized_segments.reserve(raw_segments.size());
    for (const std::wstring& segment : raw_segments) {
        if (segment.empty() || segment == L".") {
            continue;
        }
        if (segment == L"..") {
            normalized.result = FilenamePatternPolicyResult::ParentTraversalSegment;
            return normalized;
        }
        normalized_segments.push_back(segment);
    }

    if (normalized_segments.empty()) {
        normalized.result = FilenamePatternPolicyResult::EmptyInput;
        return normalized;
    }

    std::wstring joined;
    for (std::size_t i = 0; i < normalized_segments.size(); ++i) {
        if (i > 0) {
            joined.push_back(L'/');
        }
        joined.append(normalized_segments[i]);
    }

    normalized.result = FilenamePatternPolicyResult::Ok;
    normalized.normalized_pattern = joined;
    return normalized;
}

bool ContainsSupportedFilenameToken(const std::wstring& value) {
    static const std::unordered_set<std::wstring> kSupportedTokens = {
        L"{datetime}", L"{date}",    L"{time}",      L"{timestamp}", L"{YYYY}",  L"{YY}",    L"{MM}",
        L"{DD}",       L"{hh}",      L"{mm}",        L"{ss}",        L"{app}",   L"{title}", L"{process}",
        L"{target}",   L"{profile}", L"{container}", L"{video}",     L"{audio}",
    };

    for (const auto& token : kSupportedTokens) {
        if (value.find(token) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

OutputPasteSplitDecision AnalyzeOutputPasteInput(const std::wstring& raw_input) {
    OutputPasteSplitDecision decision;
    const std::wstring trimmed = Trim(raw_input);
    if (trimmed.empty()) {
        return decision;
    }

    const bool absolute_like = IsAbsoluteWindowsPath(trimmed) || StartsWith(trimmed, L"~/") ||
                               StartsWith(trimmed, L"~\\") || trimmed.find(L"%") != std::wstring::npos;
    if (!absolute_like) {
        return decision;
    }

    std::wstring split_candidate = trimmed;
    for (wchar_t& c : split_candidate) {
        if (c == L'\\') {
            c = L'/';
        }
    }

    const auto segments = SplitBySlash(split_candidate);
    int token_segment_index = -1;
    for (int i = 0; i < static_cast<int>(segments.size()); ++i) {
        if (SegmentHasToken(segments[static_cast<std::size_t>(i)]) &&
            ContainsSupportedFilenameToken(segments[static_cast<std::size_t>(i)])) {
            token_segment_index = i;
            break;
        }
    }

    if (token_segment_index > 0) {
        std::wstring folder;
        std::wstring pattern;
        for (int i = 0; i < token_segment_index; ++i) {
            if (!folder.empty()) {
                folder.push_back(L'/');
            }
            folder.append(segments[static_cast<std::size_t>(i)]);
        }
        for (std::size_t i = static_cast<std::size_t>(token_segment_index); i < segments.size(); ++i) {
            if (!pattern.empty()) {
                pattern.push_back(L'/');
            }
            pattern.append(segments[i]);
        }

        decision.kind = OutputPasteSplitKind::AutoSplitTokenPath;
        decision.folder_input = folder;
        decision.pattern_input = pattern;
        return decision;
    }

    if (LooksLikeFullFilePath(trimmed)) {
        decision.kind = OutputPasteSplitKind::OfferSplitFullFilePath;
        const std::filesystem::path path(trimmed);
        decision.folder_input = path.parent_path().wstring();
        decision.pattern_input = path.stem().wstring();
        return decision;
    }

    decision.kind = OutputPasteSplitKind::TreatAsFolder;
    decision.folder_input = trimmed;
    return decision;
}

std::wstring OutputFolderPolicyMessage(OutputFolderPolicyResult result) {
    switch (result) {
    case OutputFolderPolicyResult::Ok:
        return L"";
    case OutputFolderPolicyResult::EmptyInput:
        return L"Output folder is required.";
    case OutputFolderPolicyResult::InvalidPath:
        return L"Output folder path is invalid.";
    case OutputFolderPolicyResult::NotAbsolutePath:
        return L"Output folder must resolve to an absolute Windows path.";
    case OutputFolderPolicyResult::UnsupportedEnvironmentVariable:
        return L"Only allowlisted environment variables are supported in output folder.";
    case OutputFolderPolicyResult::UnsupportedExpression:
        return L"Shell expressions are not supported in output folder.";
    default:
        return L"Output folder is invalid.";
    }
}

std::wstring FilenamePatternPolicyMessage(FilenamePatternPolicyResult result) {
    switch (result) {
    case FilenamePatternPolicyResult::Ok:
        return L"";
    case FilenamePatternPolicyResult::EmptyInput:
        return L"Filename pattern is required.";
    case FilenamePatternPolicyResult::ParentTraversalSegment:
        return L"Filename pattern must not contain '..' segments.";
    case FilenamePatternPolicyResult::AbsolutePath:
        return L"Filename pattern must stay relative to output folder.";
    case FilenamePatternPolicyResult::UnsupportedEnvironmentVariable:
        return L"Environment variables are not allowed in filename pattern.";
    case FilenamePatternPolicyResult::UnsupportedHomeAlias:
        return L"Home alias is not allowed in filename pattern.";
    case FilenamePatternPolicyResult::UnsupportedExpression:
        return L"Shell expressions are not allowed in filename pattern.";
    default:
        return L"Filename pattern is invalid.";
    }
}

} // namespace exosnap
