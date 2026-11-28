#pragma once

#include <windows.h>

#include <string_view>

namespace exosnap::diagnostics::fixture {
ULONG QueryDpcSession(GUID* id = nullptr);
bool HoldOwnedCycle(std::string_view inherited_writer);
bool RunForcedExitCases();
} // namespace exosnap::diagnostics::fixture
