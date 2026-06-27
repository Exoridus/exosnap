#pragma once

#include "ElevationProvider.h"
#include "PresentProvider.h"

namespace exosnap::diagnostics {

// PresentMon-backed present/tearing diagnostics provider (ADR 0033).
//
// SCOPE OF THIS SLICE: this is the verifiable *shell* only. The provider gates on
// the user opt-in AND the runtime elevation state, but does NOT yet open an ETW
// session or consume PresentMon events — Sample() always reports
// available == false (a clean Sentinel degrade, like DiskSpaceProvider returning
// 0 free bytes on query failure).
//
// TODO Vendor-Slice: wire an in-process PresentMon ETW consumer here; until then
// the provider reports Unavailable. The later slice extends IsAvailable() with the
// additional "ETW session open" condition and fills Sample() from live events.
class PresentMonProvider final : public IPresentProvider {
  public:
    // The elevation provider is borrowed (must outlive this object). `opt_in`
    // reflects PersistedAppSettings::present_diagnostics_optin.
    PresentMonProvider(const IElevationProvider& elevation, bool opt_in);

    // Always reports available == false in this slice (no ETW consumer yet).
    [[nodiscard]] PresentSample Sample() const override;

    // opt_in AND elevation.IsElevated(). The Vendor-Slice will additionally
    // require an open ETW session before reporting available.
    [[nodiscard]] bool IsAvailable() const override;

    // Lets the owner update the opt-in without rebuilding the provider (e.g. when
    // the user toggles the Settings switch).
    void SetOptIn(bool opt_in);

  private:
    const IElevationProvider& elevation_;
    bool opt_in_;
};

} // namespace exosnap::diagnostics
