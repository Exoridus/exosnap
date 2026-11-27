#pragma once

#include "diagnostics/DpcLatencyProvider.h"
#include "diagnostics/PresentTraceBackend.h"

namespace exosnap::diagnostics {

enum class PrivilegedDiagnosticsState { Idle, Starting, Running, Missing, Declined, Failed, Stopping };

class PrivilegedDiagnosticsClient final : public IDpcLatencyProvider {
  public:
    PrivilegedDiagnosticsClient() = default;
    ~PrivilegedDiagnosticsClient() override;
    PrivilegedDiagnosticsClient(const PrivilegedDiagnosticsClient&) = delete;
    PrivilegedDiagnosticsClient& operator=(const PrivilegedDiagnosticsClient&) = delete;

    void Start(bool capture_present);
    void Stop();
    [[nodiscard]] PrivilegedDiagnosticsState Status() const;
    [[nodiscard]] bool KernelTraceOpen() const;
    [[nodiscard]] DpcLatencyReading Read() const override;
    [[nodiscard]] std::shared_ptr<IPresentTraceBackend> MakePresentBackend() const;

    struct Connection;

  private:
    std::shared_ptr<Connection> connection_;
};

} // namespace exosnap::diagnostics
