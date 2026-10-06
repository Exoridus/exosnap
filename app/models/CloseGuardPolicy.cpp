#include "CloseGuardPolicy.h"
#include <QCoreApplication>

namespace exosnap {

CloseGuardPrompt EvaluateCloseGuard(const CloseGuardState& state) {
    CloseGuardPrompt prompt;

    if (state.finalizing) {
        prompt.kind = CloseGuardKind::BlockSilently;
        return prompt;
    }

    if (state.remuxing) {
        prompt.kind = CloseGuardKind::ConfirmRemux;
        prompt.title = QCoreApplication::translate("CloseGuardPolicy", "Saving in progress");
        prompt.body = QCoreApplication::translate(
            "CloseGuardPolicy", "ExoSnap is saving your MP4 recording. Closing now will cancel the save and "
                                "leave only the temporary MKV file on disk.");
        prompt.proceed_label = QCoreApplication::translate("CloseGuardPolicy", "Cancel save and close");
        prompt.cancel_label = QCoreApplication::translate("CloseGuardPolicy", "Wait for save to finish");
        prompt.default_is_cancel = true;
        return prompt;
    }

    if (state.exporting) {
        prompt.kind = CloseGuardKind::ConfirmExport;
        prompt.title = QCoreApplication::translate("CloseGuardPolicy", "Export in progress");
        prompt.body = QCoreApplication::translate(
            "CloseGuardPolicy", "ExoSnap is exporting your edited recording. Closing now will cancel the "
                                "export. The original recording is untouched.");
        prompt.proceed_label = QCoreApplication::translate("CloseGuardPolicy", "Cancel export and close");
        prompt.cancel_label = QCoreApplication::translate("CloseGuardPolicy", "Wait for export to finish");
        prompt.default_is_cancel = true;
        return prompt;
    }

    if (state.recording) {
        prompt.kind = CloseGuardKind::ConfirmRecording;
        prompt.title = QCoreApplication::translate("CloseGuardPolicy", "Recording in progress");
        prompt.body = QCoreApplication::translate(
            "CloseGuardPolicy", "ExoSnap is still recording. Closing now will stop the current recording.");
        prompt.proceed_label = QCoreApplication::translate("CloseGuardPolicy", "Stop recording and close");
        prompt.cancel_label = QCoreApplication::translate("CloseGuardPolicy", "Cancel");
        // Unlike the two save guards, the safe default here is to keep
        // recording: an accidental stop loses capture that cannot be redone.
        prompt.default_is_cancel = true;
        return prompt;
    }

    prompt.kind = CloseGuardKind::Allow;
    return prompt;
}

} // namespace exosnap
