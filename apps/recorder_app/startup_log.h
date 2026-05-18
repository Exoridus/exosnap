#pragma once
#include <string_view>

namespace exosnap::startup_log {

void Write(std::wstring_view message) noexcept;
void WriteNarrow(char const* message) noexcept;
void WriteHResult(char const* context, long hresult) noexcept;

} // namespace exosnap::startup_log
