#pragma once

#include <string_view>

#include <winrt/base.h>

namespace exosnap::startup_log {

void Write(std::wstring_view message) noexcept;
void WriteNarrow(char const* message) noexcept;
void WriteHResult(char const* context, winrt::hresult_error const& ex) noexcept;

} // namespace exosnap::startup_log

