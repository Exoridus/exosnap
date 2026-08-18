// tools/envctl/win32/env_win32_system.h -- machine-wide appearance, READ ONLY.
//
// Windows exposes the Light/Dark choice only as a value in the documented
// Personalize location; there is no public setter. A registry WRITE is
// explicitly out of scope for this tool, so the appearance is ENV_HUMAN: read,
// reported, and changed by the operator when a scenario needs it.
//
// The read itself is a plain RegGetValueW against HKEY_CURRENT_USER. No key is
// opened for write anywhere in this target.

#pragma once

#include <string>

namespace exosnap::envctl::win32 {

// "light" | "dark" | "unavailable"
std::string ReadAppsTheme(std::string& error);
std::string ReadSystemTheme(std::string& error);

} // namespace exosnap::envctl::win32
