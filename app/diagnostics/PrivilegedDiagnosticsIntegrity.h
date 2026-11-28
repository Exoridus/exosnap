#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace exosnap::diagnostics::privileged {

struct DiagnosticsImageDigests {
    std::string recorder;
    std::string helper;
};

[[nodiscard]] std::optional<DiagnosticsImageDigests>
VerifyDiagnosticsImageDigests(std::string_view manifest, std::span<const uint8_t> signature,
                              std::span<const uint8_t, 32> official_key);

[[nodiscard]] bool VerifyDiagnosticsImageFiles(std::wstring_view recorder_directory, void* recorder_file,
                                               void* helper_file, std::span<const uint8_t, 32> official_key);

} // namespace exosnap::diagnostics::privileged
