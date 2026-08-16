#include "ShellAdapter.h"

namespace exosnap::quick {

ShellAdapter::ShellAdapter(QObject* parent) : QObject(parent) {
}

void ShellAdapter::setStateProvider(std::function<CloseGuardState()> provider) {
    state_provider_ = std::move(provider);
}

void ShellAdapter::setHideToTrayProvider(std::function<bool()> provider) {
    hide_to_tray_provider_ = std::move(provider);
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
    // Ahead of the guards, and deliberately so. Close-to-tray is not a weaker
    // form of closing: nothing is torn down, so there is nothing to warn about.
    // Asking "you are still recording, really close?" before a hide would
    // contradict the whole reason the preference exists. The provider owns the
    // force-quit latch, so a tray "Quit" falls through to the guards below.
    if (hide_to_tray_provider_ && hide_to_tray_provider_()) {
        // Any prompt still standing belongs to a previous, abandoned attempt.
        cancelCloseGuard();
        emit hideToTrayRequested();
        return false;
    }

    const CloseGuardPrompt prompt = EvaluateCloseGuard(currentState());
    switch (prompt.kind) {
    case CloseGuardKind::Allow:
        clearPrompt();
        return true;
    case CloseGuardKind::BlockSilently:
        // The finalizing overlay is already on screen; no prompt, no close.
        clearPrompt();
        return false;
    case CloseGuardKind::ConfirmRemux:
    case CloseGuardKind::ConfirmExport:
    case CloseGuardKind::ConfirmRecording:
        publish(prompt);
        return false;
    }
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
    if (next.kind == CloseGuardKind::Allow)
        emit closeApproved();
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
