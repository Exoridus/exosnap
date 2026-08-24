#include "ShellAdapter.h"

namespace exosnap::quick {

namespace {

// Stable keys for the close-decision signal. The whole point of reporting this
// path is that its "nothing happened" outcomes are indistinguishable to the
// user -- and, until now, to a support bundle as well.
[[nodiscard]] const char* CloseGuardKindKey(CloseGuardKind kind) noexcept {
    switch (kind) {
    case CloseGuardKind::Allow:
        return "allow";
    case CloseGuardKind::BlockSilently:
        return "blockSilently";
    case CloseGuardKind::ConfirmRemux:
        return "confirmRemux";
    case CloseGuardKind::ConfirmExport:
        return "confirmExport";
    case CloseGuardKind::ConfirmRecording:
        return "confirmRecording";
    }
    return "unknown";
}

} // namespace

ShellAdapter::ShellAdapter(QObject* parent) : QObject(parent) {
}

void ShellAdapter::setStateProvider(std::function<CloseGuardState()> provider) {
    state_provider_ = std::move(provider);
}

int ShellAdapter::currentPage() const noexcept {
    return current_page_;
}

void ShellAdapter::setCurrentPage(int page) {
    if (current_page_ == page)
        return;
    current_page_ = page;
    emit currentPageChanged();
}

bool ShellAdapter::editSurfaceVisible() const noexcept {
    return edit_surface_visible_;
}

void ShellAdapter::setEditSurfaceVisible(bool visible) {
    if (edit_surface_visible_ == visible)
        return;
    edit_surface_visible_ = visible;
    emit editSurfaceVisibleChanged();
}

bool ShellAdapter::sourcePickerOpen() const noexcept {
    return source_picker_open_;
}

void ShellAdapter::setSourcePickerOpen(bool open) {
    if (source_picker_open_ == open)
        return;
    source_picker_open_ = open;
    emit sourcePickerOpenChanged();
}

bool ShellAdapter::closeGuardActive() const noexcept {
    return active_;
}

const QString& ShellAdapter::closeGuardTitle() const noexcept {
    return prompt_.title;
}

const QString& ShellAdapter::closeGuardBody() const noexcept {
    return prompt_.body;
}

const QString& ShellAdapter::closeGuardProceedLabel() const noexcept {
    return prompt_.proceed_label;
}

const QString& ShellAdapter::closeGuardCancelLabel() const noexcept {
    return prompt_.cancel_label;
}

bool ShellAdapter::closeGuardDefaultIsCancel() const noexcept {
    return prompt_.default_is_cancel;
}

CloseGuardState ShellAdapter::currentState() const {
    // No provider means nothing can be in flight as far as this adapter knows.
    // Failing open is correct here: refusing every close because the wiring is
    // missing would strand the user in an unclosable window.
    CloseGuardState state = state_provider_ ? state_provider_() : CloseGuardState{};
    if (waived_remux_)
        state.remuxing = false;
    if (waived_export_)
        state.exporting = false;
    if (waived_recording_)
        state.recording = false;
    return state;
}

bool ShellAdapter::requestClose() {
    const CloseGuardState state = currentState();
    const CloseGuardPrompt prompt = EvaluateCloseGuard(state);

    // Reported for every outcome, because all but one look identical from the
    // outside: the window simply stays. A user saying "Quit did nothing" has no way
    // to tell a silent block from an unseen prompt from a teardown that hung, and
    // until this signal existed neither did a support bundle.
    //
    // A SIGNAL rather than a log call, so this adapter keeps its two-library
    // dependency surface. The application logs it, which is also where the other
    // half of the story lives -- the tray Quit that asked for the close.
    const auto report = [this, &state](const char* kind) {
        emit closeDecided(QString::fromLatin1(kind), state.recording, state.exporting, state.remuxing);
    };

    switch (prompt.kind) {
    case CloseGuardKind::ConfirmRemux:
    case CloseGuardKind::ConfirmExport:
    case CloseGuardKind::ConfirmRecording:
        report(CloseGuardKindKey(prompt.kind));
        publish(prompt);
        return false;
    case CloseGuardKind::BlockSilently:
        // The finalizing overlay is already on screen; no prompt, no close, and
        // no alternative route that would let the half-written container the
        // guard exists to prevent arise anyway.
        report(CloseGuardKindKey(prompt.kind));
        clearPrompt();
        return false;
    case CloseGuardKind::Allow:
        break;
    }

    report(CloseGuardKindKey(CloseGuardKind::Allow));
    clearPrompt();
    return true;
}

void ShellAdapter::confirmCloseGuard() {
    if (!active_)
        return;
    switch (prompt_.kind) {
    case CloseGuardKind::ConfirmRemux:
        waived_remux_ = true;
        emit cancelRemuxRequested();
        break;
    case CloseGuardKind::ConfirmExport:
        waived_export_ = true;
        emit cancelExportRequested();
        break;
    case CloseGuardKind::ConfirmRecording:
        waived_recording_ = true;
        emit stopRecordingRequested();
        break;
    case CloseGuardKind::Allow:
    case CloseGuardKind::BlockSilently:
        break;
    }

    // Re-evaluate rather than closing straight away: the Widgets guard chain
    // fell through from the export question to the recording question, and a
    // user who cancels an export while still recording must be asked both.
    const CloseGuardPrompt next = EvaluateCloseGuard(currentState());
    if (next.kind == CloseGuardKind::ConfirmRemux || next.kind == CloseGuardKind::ConfirmExport ||
        next.kind == CloseGuardKind::ConfirmRecording) {
        publish(next);
        return;
    }
    clearPrompt();
    if (next.kind == CloseGuardKind::Allow) {
        // BOTH signals, and closeDecided FIRST. They answer different questions, and
        // only one of them ends the process: `closeApproved` tells the window it may
        // go and flushes pending persists, while `closeDecided("allow")` is what the
        // application hangs the explicit QCoreApplication::quit() off.
        //
        // Emitting only the former is how a confirmed close came to destroy the
        // window without ending the process. Qt's own quit does not step in there:
        // it fires on the last visible PRIMARY window, and all five capture overlays
        // are exactly that -- top level, `transientParent: null` -- and they are
        // hidden rather than closed, so the signal gets no later chance either. What
        // is left is no window, a live tray icon, and Task Manager. That is the
        // failure the tray-Quit path used to have; the guarded path kept it longer
        // because it approves the close HERE and never returns through
        // requestClose(), which is where every other outcome is reported.
        const CloseGuardState allowed = currentState();
        emit closeDecided(QString::fromLatin1(CloseGuardKindKey(CloseGuardKind::Allow)), allowed.recording,
                          allowed.exporting, allowed.remuxing);
        emit closeApproved();
    }
    // BlockSilently: a finalize started while the dialog was up. Keeping the
    // window open is the only safe answer, and the overlay explains it.
}

void ShellAdapter::cancelCloseGuard() {
    waived_remux_ = false;
    waived_export_ = false;
    waived_recording_ = false;
    clearPrompt();
}

void ShellAdapter::publish(const CloseGuardPrompt& prompt) {
    prompt_ = prompt;
    active_ = true;
    emit closeGuardChanged();
}

void ShellAdapter::clearPrompt() {
    if (!active_ && prompt_.kind == CloseGuardKind::Allow)
        return;
    prompt_ = CloseGuardPrompt{};
    active_ = false;
    emit closeGuardChanged();
}

} // namespace exosnap::quick
