#pragma once

#include <QString>

#include <optional>

#include "viewmodels/RecordViewModel.h"

namespace exosnap::models {

// The engine's error phase for the disk-guard auto-stop. Two places react to it
// — the failure surface stays out of the way, and the low-storage notification
// takes over — so the spelling is written down once.
inline constexpr wchar_t kDiskSpaceErrorPhase[] = L"DiskSpace";

// True when this result is the disk-guard auto-stop rather than an ordinary
// failure.
[[nodiscard]] inline bool IsDiskSpaceAutoStop(const UiRecordingResult& result) {
    return !result.succeeded && result.error_phase == kDiskSpaceErrorPhase;
}

// What a failed recording attempt tells the user.
//
// The same struct the Widgets RecordingErrorPanel renders (it was declared there
// as RecordingErrorModel), lifted out of the UI so both frontends read one
// definition. `detail` may contain a local path: it is safe on screen, and the
// Sentry path scrubs it inside crash_capture::ReportNonFatalError before
// anything leaves the machine.
struct RecordingFailureReport {
    QString title;   // "Recording could not start" / "Recording stopped unexpectedly"
    QString summary; // one-line plain explanation
    QString phase;   // engine error phase, e.g. "Validate" / "Mux" / "Encode"
    QString code;    // HRESULT text, e.g. "0x80004001" (may be empty)
    QString detail;  // human-readable engine detail (may contain a local path)

    // Container/codec context — shown to the user and, when the user opts in,
    // attached as allow-listed Sentry tags. Empty strings are omitted on screen.
    QString container;
    QString video_codec;
    QString audio_codec;
};

// Decides whether a finished recording deserves the error surface, and what it
// says. Returns nullopt for a success and for the disk-space auto-stop, which
// has its own actionable "Storage running low" notification and must not also
// raise a modal — the user already knows, and they already have the action.
//
// Pure: no engine, no UI, no crash_capture. Whether the report can be SENT is a
// separate question the composition root answers from crash_capture::IsActive().
[[nodiscard]] std::optional<RecordingFailureReport> BuildRecordingFailureReport(const UiRecordingResult& result);

} // namespace exosnap::models
