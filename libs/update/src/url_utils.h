// url_utils.h -- shared UTF-8 -> UTF-16 conversion for Windows path/URL APIs.
//
// WinHTTP and the Win32 filesystem APIs consume std::wstring; every payload
// that reaches this library (URLs, zip entry names) arrives as UTF-8
// std::string. Centralised here so a byte-wise widening (correct only for
// ASCII) is never re-introduced at a new call site.

#pragma once

#include <string>
#include <string_view>

namespace exosnap::update {

// Converts a UTF-8 byte sequence to UTF-16 via MultiByteToWideChar(CP_UTF8).
// Returns an empty string for empty input or on conversion failure.
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);

} // namespace exosnap::update
