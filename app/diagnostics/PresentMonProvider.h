#pragma once

#include "ElevationProvider.h"
#include "PresentMonEtwSession.h"
#include "PresentProvider.h"

namespace exosnap::diagnostics {

// PresentMon-backed present/tearing diagnostics provider (ADR 0033).
//
// GateOpen() is the pre-session condition: opt_in && elevation.IsElevated().
// IsAvailable() additionally requires the ETW session to be open.
// SetOptIn() starts or stops the session to match the gate.
class PresentMonProvider final : public IPresentProvider {
  public:
    // The elevation provider is borrowed (must outlive this object). `opt_in`
    // reflects PersistedAppSettings::present_diagnostics_optin.
    PresentMonProvider(const IElevationProvider& elevation, bool opt_in);

    [[nodiscard]] PresentSample Sample() const override;

    // opt_in AND elevation AND session open.
    [[nodiscard]] bool IsAvailable() const override;

    // Pre-session gate: opt_in AND elevation.IsElevated().
    // Drives whether SetOptIn starts the ETW session.
    [[nodiscard]] bool GateOpen() const;

    // Updates the opt-in and starts/stops the ETW session to match the gate.
    void SetOptIn(bool opt_in);

  private:
    const IElevationProvider& elevation_;
    bool opt_in_;
    mutable PresentMonEtwSession session_;
};

} // namespace exosnap::diagnostics
