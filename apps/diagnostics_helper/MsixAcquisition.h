#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <span>

namespace exosnap::diagnostics::ipc {

// The caller retains the verified package handle and path locks throughout deployment.
using VerifyRegisteredImage = std::function<bool(const std::filesystem::path&, std::span<const unsigned char>,
                                                 std::span<const unsigned char>)>;
bool AcquireRegisteredMsix(HANDLE source, const std::filesystem::path& source_path, HANDLE stop,
                           const VerifyRegisteredImage& verify);

} // namespace exosnap::diagnostics::ipc
