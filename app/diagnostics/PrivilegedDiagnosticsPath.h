#pragma once

#include "diagnostics/PrivilegedDiagnosticsPipe.h"

#include <optional>

namespace exosnap::diagnostics::privileged {

inline constexpr wchar_t kDirectoryName[] = L"ExoSnap Diagnostics";
inline constexpr wchar_t kExecutableName[] = L"exosnap-diagnostics.exe";

struct ProtectedHelper {
    std::wstring directory;
    std::wstring executable;
    Handle program_files_pin;
    Handle directory_pin;
    Handle executable_pin;
};

[[nodiscard]] std::optional<ProtectedHelper> OpenProtectedHelper();

} // namespace exosnap::diagnostics::privileged
