#pragma once

#include "diagnostics/PrivilegedDiagnosticsPipe.h"

#include <optional>
#include <vector>

namespace exosnap::diagnostics::privileged {

inline constexpr wchar_t kExecutableName[] = L"exosnap-helper.exe";
inline constexpr wchar_t kRecorderName[] = L"exosnap.exe";

struct ProtectedHelper {
    std::wstring directory;
    std::wstring executable;
    std::vector<Handle> directory_pins;
    Handle recorder_pin;
    Handle executable_pin;
};

[[nodiscard]] std::optional<ProtectedHelper> OpenSiblingHelper(HANDLE process, const wchar_t* expected_name);
[[nodiscard]] bool ProcessImageMatchesFile(HANDLE process, HANDLE file);
[[nodiscard]] bool IsPairedRecorder(const ProtectedHelper& location, HANDLE process);

} // namespace exosnap::diagnostics::privileged
