#pragma once

// The stable, thread-safe hand-off between the exclusive-fullscreen probe and
// everything that reads it: the Diagnostics card and the recording-admission
// gate.
//
// The probe (services/WindowEvidenceProbe) owns a WGC subscription on a COM
// apartment of its own and publishes one of these under a mutex. Consumers copy
// it and reason over the copy, so nothing outside the probe ever touches COM,
// D3D or the accumulator — which is what lets the admission gate resolve a
// verdict on the UI thread without waiting for a native probe.
//
// `hwnd` travels WITH the evidence on purpose. A consumer that has already
// retargeted must not judge the new window by the previous one's measurements,
// and a snapshot that names its own target makes that a comparison rather than
// an ordering assumption about when the worker catches up.
//
// Header-only and free of Win32/COM/Qt so the resolver below is unit-pinned
// without a GPU.

#include <cstdint>

#include "WindowTargetFacts.h"

namespace exosnap::diagnostics {

struct WindowEvidenceSnapshot {
    bool active = false; // a window target is subscribed
    uintptr_t hwnd = 0;  // the target this evidence describes
    WindowTargetFacts facts;
    WindowHubEvidence evidence;
};

// PURE. The exclusive-fullscreen verdict a consumer may draw from `snapshot`
// about `target_hwnd`.
//
// None — never a guess — whenever the snapshot cannot speak for that target:
// no subscription, no target, or a snapshot describing a different window. Only
// a snapshot that names exactly this window is resolved, and then through the
// same ResolveExclusiveEvidence the Diagnostics card uses.
[[nodiscard]] inline ExclusiveEvidence ResolveSnapshotEvidence(const WindowEvidenceSnapshot& snapshot,
                                                               uintptr_t target_hwnd,
                                                               bool present_exclusive_fullscreen) noexcept {
    if (!snapshot.active || snapshot.hwnd == 0 || target_hwnd == 0 || snapshot.hwnd != target_hwnd) {
        return ExclusiveEvidence::None;
    }
    return ResolveExclusiveEvidence(snapshot.facts, snapshot.evidence, present_exclusive_fullscreen);
}

} // namespace exosnap::diagnostics
