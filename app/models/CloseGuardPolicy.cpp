#include "CloseGuardPolicy.h"

namespace exosnap {

CloseGuardPrompt EvaluateCloseGuard(const CloseGuardState& state) {
    CloseGuardPrompt prompt;

    if (state.finalizing) {
        prompt.kind = CloseGuardKind::BlockSilently;
        return prompt;
    }

    if (state.remuxing) {
        prompt.kind = CloseGuardKind::ConfirmRemux;
        prompt.title = QStringLiteral("Saving in progress");
        prompt.body = QStringLiteral("ExoSnap is saving your MP4 recording. Closing now will cancel the save and "
                                     "leave only the temporary MKV file on disk.");
        prompt.proceed_label = QStringLiteral("Cancel save and close");
        prompt.cancel_label = QStringLiteral("Wait for save to finish");
        prompt.default_is_cancel = true;
        return prompt;
    }

    if (state.exporting) {
        prompt.kind = CloseGuardKind::ConfirmExport;
        prompt.title = QStringLiteral("Export in progress");
        prompt.body = QStringLiteral("ExoSnap is exporting your edited recording. Closing now will cancel the "
                                     "export. The original recording is untouched.");
        prompt.proceed_label = QStringLiteral("Cancel export and close");
        prompt.cancel_label = QStringLiteral("Wait for export to finish");
        prompt.default_is_cancel = true;
        return prompt;
    }

    if (state.recording) {
        prompt.kind = CloseGuardKind::ConfirmRecording;
        prompt.title = QStringLiteral("Recording in progress");
        prompt.body = QStringLiteral("ExoSnap is still recording. Closing now will stop the current recording.");
        prompt.proceed_label = QStringLiteral("Stop recording and close");
        prompt.cancel_label = QStringLiteral("Cancel");
        // Unlike the two save guards, the safe default here is to keep
        // recording: an accidental stop loses capture that cannot be redone.
        prompt.default_is_cancel = true;
        return prompt;
    }

    prompt.kind = CloseGuardKind::Allow;
    return prompt;
}

} // namespace exosnap
