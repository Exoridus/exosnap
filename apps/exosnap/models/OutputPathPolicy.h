#pragma once

#include <filesystem>
#include <string>

namespace exosnap {

enum class OutputFolderPolicyResult {
    Ok,
    EmptyInput,
    InvalidPath,
    NotAbsolutePath,
    UnsupportedEnvironmentVariable,
    UnsupportedExpression,
};

struct NormalizedOutputFolder {
    OutputFolderPolicyResult result = OutputFolderPolicyResult::InvalidPath;
    std::filesystem::path resolved_path;
    std::wstring normalized_input;
};

enum class FilenamePatternPolicyResult {
    Ok,
    EmptyInput,
    ParentTraversalSegment,
    AbsolutePath,
    UnsupportedEnvironmentVariable,
    UnsupportedHomeAlias,
    UnsupportedExpression,
};

struct NormalizedFilenamePattern {
    FilenamePatternPolicyResult result = FilenamePatternPolicyResult::EmptyInput;
    std::wstring normalized_pattern;
};

enum class OutputPasteSplitKind {
    TreatAsFolder,
    AutoSplitTokenPath,
    OfferSplitFullFilePath,
};

struct OutputPasteSplitDecision {
    OutputPasteSplitKind kind = OutputPasteSplitKind::TreatAsFolder;
    std::wstring folder_input;
    std::wstring pattern_input;
};

[[nodiscard]] NormalizedOutputFolder NormalizeOutputFolderInput(const std::wstring& raw_input);
[[nodiscard]] NormalizedFilenamePattern NormalizeFilenamePatternInput(const std::wstring& raw_pattern);
[[nodiscard]] bool ContainsSupportedFilenameToken(const std::wstring& value);
[[nodiscard]] OutputPasteSplitDecision AnalyzeOutputPasteInput(const std::wstring& raw_input);

[[nodiscard]] std::wstring OutputFolderPolicyMessage(OutputFolderPolicyResult result);
[[nodiscard]] std::wstring FilenamePatternPolicyMessage(FilenamePatternPolicyResult result);

} // namespace exosnap
