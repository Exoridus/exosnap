#pragma once

#include "diagnostics/PrivilegedDiagnosticsPath.h"

#include <string_view>

namespace exosnap::diagnostics::privileged {

enum class TraceSlot : size_t { Present, Dpc };

class TraceGuard {
  public:
    [[nodiscard]] bool Start(const ProtectedHelper& location, const std::wstring& nonce);
    [[nodiscard]] bool Alive() const;
    [[nodiscard]] const GUID& Id(TraceSlot slot) const;

  private:
    std::array<GUID, 2> ids_{};
    Handle child_;
};

[[nodiscard]] std::wstring TraceName(TraceSlot slot, const std::wstring& nonce);
void StopOwnedTrace(TraceSlot slot, const std::wstring& nonce, const GUID& id);
[[nodiscard]] bool RunTraceGuard(const ProtectedHelper& location, const std::wstring& nonce,
                                 std::wstring_view owner_arg, std::wstring_view section_arg,
                                 std::wstring_view ready_arg);

} // namespace exosnap::diagnostics::privileged
