#include "RecordingAdmission.h"

namespace exosnap {

AdmissionBlocker EvaluateRecordingAdmission(const AdmissionFacts& facts) noexcept {
    // rec.hdr.h264. All three gates required, in the same order and with the same
    // meaning as the diagnostics check:
    //   1. HDR10 is the selected handling. Tone-map-to-SDR outputs SDR 8-bit and
    //      is explicitly not a conflict, on any codec.
    //   2. The codec cannot carry HDR10.
    //   3. The target display is HDR-active. On an SDR desktop the native HDR10
    //      path never engages, so there is nothing to break.
    if (facts.hdr_mode == recorder_core::HdrMode::Hdr10 && !facts.codec_can_carry_hdr10 &&
        facts.target_display_hdr_active) {
        return AdmissionBlocker::Hdr10CodecConflict;
    }

    // rec.capture.exclusive_window. Only ProvenBlack blocks. Suspected is a Notice
    // by design — the shape heuristic cannot tell borderless from exclusive
    // fullscreen, and blocking on it would reject the common, working case.
    if (facts.window_exclusive_evidence == diagnostics::ExclusiveEvidence::ProvenBlack) {
        return AdmissionBlocker::ExclusiveFullscreenWindow;
    }

    return AdmissionBlocker::None;
}

const wchar_t* AdmissionBlockerDetail(AdmissionBlocker blocker) noexcept {
    switch (blocker) {
    case AdmissionBlocker::Hdr10CodecConflict:
        return L"The selected video codec cannot record HDR10. Switch the codec to AV1 or HEVC, or set HDR "
               L"handling to Tone-map to SDR.";
    case AdmissionBlocker::ExclusiveFullscreenWindow:
        return L"The selected window is in exclusive fullscreen and window capture produces no frames. "
               L"Record the monitor instead, or switch the application to borderless fullscreen.";
    case AdmissionBlocker::None:
        break;
    }
    return L"";
}

const char* AdmissionBlockerDiagnosticId(AdmissionBlocker blocker) noexcept {
    switch (blocker) {
    case AdmissionBlocker::Hdr10CodecConflict:
        return "rec.hdr.h264";
    case AdmissionBlocker::ExclusiveFullscreenWindow:
        return "rec.capture.exclusive_window";
    case AdmissionBlocker::None:
        break;
    }
    return "";
}

} // namespace exosnap
