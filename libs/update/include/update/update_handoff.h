#pragma once

// Private Win32 message used by the staged updater to transfer shutdown
// ownership to the running app. The message is intentionally defined without a
// windows.h dependency so both the Qt app and the UI-agnostic updater worker can
// share one exact protocol value.

#include <cstdint>

namespace exosnap::update {

// WM_APP is 0x8000. This offset is private to ExoSnap's main window.
inline constexpr std::uint32_t kUpdaterHandoffMessage = 0x8000u + 0x455u;
inline constexpr std::uintptr_t kUpdaterHandoffMagic = static_cast<std::uintptr_t>(0x45584F534E415055ull); // "EXOSNAPU"

} // namespace exosnap::update
