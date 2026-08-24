#pragma once

// Recording admission: the independent enforcement of the diagnostic blockers
// that gate a recording start.
//
// The product canon (docs/product-spec.md, "Pre-flight readiness gate") says
// recording start is blocked while any diagnostic blocker exists. Most Tier-1
// blockers already have an independent enforcement path on the way into the
// engine and are deliberately NOT repeated here:
//
//   rec.output.writable  -> ValidateOutputFolder() in the prepare worker
//   rec.007 (disk)       -> the pre-start hard-stop guard in the prepare worker
//   rec.003 / rec.004    -> capability::Resolve() sanitizes an unavailable codec
//   rec.006              -> same resolver; an unsupported profile never resolves
//   rec.009 / rec.010    -> ContainerCompatRegistry::ReconcileCodecs in the resolver
//
// Two blockers had no such path, which is what this resolver exists for:
//
//   rec.hdr.h264                 HDR10 selected + the codec cannot carry HDR10 +
//                                the capture target's display is HDR-active.
//   rec.capture.exclusive_window The selected window is in legacy exclusive
//                                fullscreen and capture has demonstrably produced
//                                no frames (ProvenBlack — a Suspected window is a
//                                Notice and must NOT block: an ordinary borderless
//                                fullscreen game looks exactly the same and
//                                captures fine).
//
// Pure: no Win32, no Qt, no clock. The coordinator gathers the facts (refreshed
// display facts + resolved config on the worker, window facts + measured hub
// evidence on the UI thread) and this decides.

#include <exosnap/engine/codec_types.h>

#include "../diagnostics/WindowTargetFacts.h"

namespace exosnap {

enum class AdmissionBlocker {
    None,
    // rec.hdr.h264 — HDR10 requested on a codec with no 10-bit/HDR10 path.
    Hdr10CodecConflict,
    // rec.capture.exclusive_window (ProvenBlack) — window capture records black.
    ExclusiveFullscreenWindow,
};

// Everything the admission decision reads. Defaults describe an unremarkable SDR
// monitor recording, so a caller that supplies nothing is never blocked.
struct AdmissionFacts {
    exosnap::engine::HdrMode hdr_mode = exosnap::engine::HdrMode::Off;
    // Whether the selected video codec can carry a native HDR10 signal. Comes from
    // the capability annotation (caps.QueryHdr10Native), never a codec-name
    // compare — the same source RecommendationEngine::checkHdrH264Blocker uses, so
    // the gate and the card can never disagree.
    bool codec_can_carry_hdr10 = false;
    // Whether the capture target's display currently has Windows HDR on. Read from
    // the REFRESHED display facts, not the startup snapshot: HDR can be toggled
    // after launch and the blocker only fires when the native path would engage.
    bool target_display_hdr_active = false;
    // Measured exclusive-fullscreen verdict for a window capture target. None for a
    // monitor target, or when nothing measured the window.
    diagnostics::ExclusiveEvidence window_exclusive_evidence = diagnostics::ExclusiveEvidence::None;
};

// PURE. Returns the first blocker that applies, or None when the start is admitted.
[[nodiscard]] AdmissionBlocker EvaluateRecordingAdmission(const AdmissionFacts& facts) noexcept;

// User-facing detail for the Failed result. Names the conflict and the way out,
// matching the wording of the corresponding diagnostics card.
[[nodiscard]] const wchar_t* AdmissionBlockerDetail(AdmissionBlocker blocker) noexcept;

// The diagnostics check id this blocker enforces, for the structured log line.
[[nodiscard]] const char* AdmissionBlockerDiagnosticId(AdmissionBlocker blocker) noexcept;

} // namespace exosnap
