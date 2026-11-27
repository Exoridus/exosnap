#pragma once

#include "diagnostics/DpcLatencyProvider.h"
#include "diagnostics/PresentTraceBackend.h"

namespace exosnap::diagnostics {
class IPrivilegedDiagnosticsBackend : public IPresentTraceBackend, public IDpcLatencyProvider {};
[[nodiscard]] std::shared_ptr<IPrivilegedDiagnosticsBackend> MakePrivilegedDiagnosticsBackend(bool collect_presents);
} // namespace exosnap::diagnostics
