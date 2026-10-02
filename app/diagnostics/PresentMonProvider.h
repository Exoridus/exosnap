#pragma once

#include "PresentMonEtwSession.h"
#include "PresentProvider.h"
#include "PresentTraceBackend.h"

#include <functional>
#include <memory>

namespace exosnap::diagnostics {

// PresentMon-backed present/tearing diagnostics provider.
//
// The opt-in is the request; the actual controlled ETW open attempt decides
// availability. Elevation is deliberately NOT part of the gate: a standard token
// that holds the trace right (for example Performance Log Users) can measure,
// and an elevated process can still meet a session conflict. `state()` reports
// the real answer, including access denied and conflicts, instead of a
// privilege prediction.
class PresentMonProvider final : public IPresentProvider {
  public:
    // `opt_in` is the session-scoped in-depth diagnostics switch, never a
    // persisted setting.
    explicit PresentMonProvider(bool opt_in);

    // Test seam. Passed straight to the session, so a test can drive the whole
    // availability truth table without a real trace.
    //
    // Tests MUST use this. An opt-in provider built with the default factory
    // opens a real system-wide ETW session named ExoSnapPresentMon. Start() no
    // longer stops a same-named session -- a conflict is reported instead -- but
    // a test must not create one on a developer's desktop either.
    PresentMonProvider(bool opt_in, std::function<std::shared_ptr<IPresentTraceBackend>()> backend_factory);

    [[nodiscard]] PresentSample Sample() const override;

    // The trace is open and its consumer alive. Availability says nothing about
    // whether fresh data has arrived; that is what state() and the sample's own
    // `available` flag answer.
    [[nodiscard]] bool IsAvailable() const override;

    // opt-in requested, from the user's switch.
    [[nodiscard]] bool GateOpen() const;

    // The honest three-fact projection: request, OS answer, fresh measurement.
    [[nodiscard]] PresentProviderState state() const;
    // Why the last open ended as it did, regardless of the current opt-in.
    [[nodiscard]] PresentTraceOpenResult openResult() const;

    // Updates the opt-in and starts/stops the ETW session to match the gate.
    void SetOptIn(bool opt_in);

    // Scope present attribution to the recorded target's process (0 == global / any).
    // Set to the captured window's PID on record-start (Window targets only) so present-
    // mode/discard/flip stats reflect the recorded source, not whatever last presented.
    void SetTargetProcessId(unsigned long pid) {
        session_.SetTargetProcessId(pid);
    }

  private:
    bool opt_in_;
    mutable PresentMonEtwSession session_;
};

} // namespace exosnap::diagnostics
