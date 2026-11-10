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
    case CloseGuardKind::ConfirmEditor:
        return "confirmEditor";
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

int ShellAdapter::diagnosticsSection() const noexcept {
    return diagnostics_section_;
}

void ShellAdapter::setDiagnosticsSection(int section) {
    if (diagnostics_section_ == section)
        return;
    diagnostics_section_ = section;
    emit diagnosticsSectionChanged();
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

const QVariantMap& ShellAdapter::editPanels() const noexcept {
    return edit_panels_;
}

void ShellAdapter::setEditPanels(const QVariantMap& panels) {
    if (edit_panels_ == panels)
        return;
    edit_panels_ = panels;
    emit editPanelsChanged();
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
    if (waived_remux_ == state.remux_generation)
        state.remuxing = false;
    if (waived_export_ == state.export_generation)
        state.exporting = false;
    if (waived_recording_ == state.recording_generation)
        state.recording = false;
    if (waived_editor_ == state.editor_revision)
        state.editor_dirty = false;
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
        emit closeDecided(QString::fromLatin1(kind), state.recording, state.exporting, state.remuxing, state.finalizing,
                          state.editor_dirty);
    };

    switch (prompt.kind) {
    case CloseGuardKind::ConfirmRemux:
    case CloseGuardKind::ConfirmExport:
    case CloseGuardKind::ConfirmRecording:
    case CloseGuardKind::ConfirmEditor:
        report(CloseGuardKindKey(prompt.kind));
        publish(prompt, state);
        return false;
    case CloseGuardKind::BlockSilently:
        // The finalizing overlay is already on screen; no prompt, no close, and
        // no alternative route that would let the half-written container the
        // guard exists to prevent arise anyway.
        report(CloseGuardKindKey(prompt.kind));
        cancelCloseGuard();
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
    const auto current = currentState();
    const auto next = EvaluateCloseGuard(current);
    const bool same_work = [&] {
        if (next.kind != prompt_.kind)
            return false;
        switch (prompt_.kind) {
        case CloseGuardKind::ConfirmRemux:
            return current.remux_generation == prompted_state_.remux_generation;
        case CloseGuardKind::ConfirmExport:
            return current.export_generation == prompted_state_.export_generation;
        case CloseGuardKind::ConfirmRecording:
            return current.recording_generation == prompted_state_.recording_generation;
        case CloseGuardKind::ConfirmEditor:
            return current.editor_revision == prompted_state_.editor_revision;
        default:
            return false;
        }
    }();
    // Recheck before effects: finalization may have begun, another operation
    // may now have priority, or the prompt may describe an older workspace.
    if (same_work) {
        switch (prompt_.kind) {
        case CloseGuardKind::ConfirmRemux:
            waived_remux_ = current.remux_generation;
            emit cancelRemuxRequested();
            break;
        case CloseGuardKind::ConfirmExport:
            waived_export_ = current.export_generation;
            emit cancelExportRequested();
            break;
        case CloseGuardKind::ConfirmRecording:
            waived_recording_ = current.recording_generation;
            emit stopRecordingRequested();
            break;
        case CloseGuardKind::ConfirmEditor:
            waived_editor_ = current.editor_revision;
            break;
        default:
            break;
        }
    }
    // requestClose reports the allow decision that queues application quit,
    // as well as every follow-up prompt or finalize block.
    if (requestClose())
        emit closeApproved();
}

void ShellAdapter::cancelCloseGuard() {
    waived_remux_.reset();
    waived_export_.reset();
    waived_recording_.reset();
    waived_editor_.reset();
    clearPrompt();
}

void ShellAdapter::publish(const CloseGuardPrompt& prompt, const CloseGuardState& state) {
    prompt_ = prompt;
    prompted_state_ = state;
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
