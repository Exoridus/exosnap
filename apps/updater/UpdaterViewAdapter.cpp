#include "UpdaterViewAdapter.h"

#include <QVariantMap>
#include <QtMath>

#include <array>

namespace exosnap::updater {
namespace {

QString WidgetLabelForStatus(StepStatus status, bool manual) {
    switch (status) {
    case StepStatus::Done:
        return QStringLiteral("done");
    case StepStatus::Working:
        return QStringLiteral("working");
    case StepStatus::Failed:
        return manual ? QStringLiteral("manual") : QStringLiteral("failed");
    case StepStatus::Queued:
    default:
        return QStringLiteral("queued");
    }
}

QString StatusKey(StepStatus status) {
    switch (status) {
    case StepStatus::Done:
        return QStringLiteral("done");
    case StepStatus::Working:
        return QStringLiteral("working");
    case StepStatus::Failed:
        return QStringLiteral("failed");
    case StepStatus::Queued:
    default:
        return QStringLiteral("queued");
    }
}

} // namespace

UpdaterViewAdapter::UpdaterViewAdapter(QObject* parent) : QObject(parent) {
    render(UpdaterUiState{});
}

QStringList UpdaterViewAdapter::stepLabels() {
    return {
        QStringLiteral("Downloading update"),   QStringLiteral("Closing previous version"),
        QStringLiteral("Installing new files"), QStringLiteral("Verifying installation"),
        QStringLiteral("Launching ExoSnap"),
    };
}

void UpdaterViewAdapter::render(const UpdaterUiState& state) {
    state_ = state;

    const bool failed_terminal =
        state.variant == TerminalVariant::Amber || state.variant == TerminalVariant::Red;
    failed_terminal_ = failed_terminal;
    const bool prompting = state.prompt != PromptKind::None;
    const bool success_done = state.variant == TerminalVariant::Success;

    // Eyebrow. Four truths, one line; a manual window at rest says what it IS,
    // not what it is doing.
    if (failed_terminal) {
        eyebrow_ = state.verification_reinstall ? QStringLiteral("EXOSNAP WAS NOT REINSTALLED")
                                                : QStringLiteral("EXOSNAP WAS NOT UPDATED");
    } else if (prompting) {
        eyebrow_ = state.prompt == PromptKind::UpToDate    ? QStringLiteral("EXOSNAP IS UP TO DATE")
                   : state.prompt == PromptKind::Cancelled ? QStringLiteral("EXOSNAP WAS NOT UPDATED")
                                                           : QStringLiteral("EXOSNAP UPDATER");
    } else {
        eyebrow_ = state.verification_reinstall ? QStringLiteral("REINSTALLING EXOSNAP")
                                                : QStringLiteral("UPDATING EXOSNAP");
    }

    // Ring. Indeterminate until the run has measured something.
    indeterminate_ = state.variant == TerminalVariant::None && !state.determinate;
    ring_percent_ = qRound(state.ring * 100.0);
    if (state.variant == TerminalVariant::Amber) {
        ring_tone_ = QStringLiteral("warning");
        ring_glyph_ = QStringLiteral("warning");
        ring_description_ = QStringLiteral("Update didn't complete");
    } else if (state.variant == TerminalVariant::Red) {
        ring_tone_ = QStringLiteral("error");
        ring_glyph_ = QStringLiteral("cross");
        ring_description_ = QStringLiteral("Update failed");
    } else if (state.variant == TerminalVariant::Green || state.variant == TerminalVariant::Success ||
               state.variant == TerminalVariant::RebootRequired) {
        ring_tone_ = QStringLiteral("success");
        ring_glyph_ = QStringLiteral("check");
        ring_description_ = QStringLiteral("Update complete");
    } else {
        ring_tone_ = QStringLiteral("accent");
        ring_glyph_.clear();
        ring_description_ = indeterminate_ ? QStringLiteral("Preparing update, progress not measurable yet")
                                           : QStringLiteral("%1 percent").arg(ring_percent_);
    }

    // Status row. The same footprint in every state; terminal states use a
    // short summary here and the card below owns the actionable detail.
    status_text_visible_ = !prompting;
    if (prompting) {
        status_glyph_.clear();
        status_tone_ = QStringLiteral("neutral");
        status_headline_.clear();
        status_line_.clear();
    } else if (state.variant == TerminalVariant::None) {
        status_glyph_ = QStringLiteral("spinner");
        status_tone_ = QStringLiteral("accent");
        status_headline_.clear();
        status_line_ = state.status_line;
    } else {
        status_line_.clear();
        switch (state.variant) {
        case TerminalVariant::Amber:
            status_glyph_ = QStringLiteral("warning");
            status_tone_ = QStringLiteral("warning");
            status_headline_ = QStringLiteral("Update didn't complete");
            break;
        case TerminalVariant::Red:
            status_glyph_ = QStringLiteral("cross");
            status_tone_ = QStringLiteral("error");
            status_headline_ = QStringLiteral("Update failed");
            break;
        case TerminalVariant::Green:
            status_glyph_ = QStringLiteral("check");
            status_tone_ = QStringLiteral("success");
            status_headline_ = QStringLiteral("Update complete — version %1 is ready").arg(state.to_version);
            break;
        case TerminalVariant::RebootRequired:
            status_glyph_ = QStringLiteral("check");
            status_tone_ = QStringLiteral("success");
            status_headline_ = QStringLiteral("Update installed — restart Windows to finish");
            break;
        case TerminalVariant::Success:
            status_glyph_ = QStringLiteral("check");
            status_tone_ = QStringLiteral("accent");
            status_headline_ = QStringLiteral("Update complete - version %1 is ready").arg(state.to_version);
            break;
        case TerminalVariant::None:
            break;
        }
    }

    rebuildStepRows();

    // State / result panel. A prompt renders through the SAME panel as a
    // terminal result; only tone and glyph differ.
    const bool result_terminal = prompting || (state.variant != TerminalVariant::None && !success_done);
    if (result_terminal) {
        panel_kind_ = QStringLiteral("result");
        if (prompting) {
            panel_tone_ = state.prompt == PromptKind::UpToDate    ? QStringLiteral("success")
                          : state.prompt == PromptKind::Cancelled ? QStringLiteral("neutral")
                                                                  : QStringLiteral("accent");
        } else if (state.variant == TerminalVariant::Red) {
            panel_tone_ = QStringLiteral("error");
        } else if (state.variant == TerminalVariant::Green || state.variant == TerminalVariant::RebootRequired) {
            panel_tone_ = QStringLiteral("success");
        } else {
            panel_tone_ = QStringLiteral("warning");
        }

        if (prompting) {
            switch (state.prompt) {
            case PromptKind::Idle:
                panel_glyph_ = QStringLiteral("shield");
                break;
            case PromptKind::UpdateAvailable:
                panel_glyph_ = QStringLiteral("download");
                break;
            case PromptKind::ReadyToApply:
                panel_glyph_ = QStringLiteral("layers");
                break;
            case PromptKind::Cancelled:
                panel_glyph_ = QStringLiteral("dot");
                break;
            case PromptKind::UpToDate:
            case PromptKind::None:
                panel_glyph_ = QStringLiteral("check");
                break;
            }
        } else if (state.variant == TerminalVariant::Amber) {
            panel_glyph_ = QStringLiteral("warning");
        } else if (state.variant == TerminalVariant::Red) {
            panel_glyph_ = QStringLiteral("cross");
        } else {
            panel_glyph_ = QStringLiteral("check");
        }

        panel_title_ = state.headline;
        panel_detail_ = state.detail_text;
        panel_safety_ = state.safety_text;
    } else {
        UpStep active = UpStep::Count;
        for (int i = 0; i < int(UpStep::Count); ++i) {
            if (state.steps[size_t(i)] == StepStatus::Working) {
                active = static_cast<UpStep>(i);
                break;
            }
        }

        panel_kind_ = QStringLiteral("working");
        if (success_done) {
            panel_title_ = QStringLiteral("Version %1 is ready").arg(state.to_version);
            panel_detail_ = QStringLiteral("The update is installed and verified.");
            panel_safety_ = QStringLiteral("ExoSnap is starting automatically.");
            panel_glyph_ = QStringLiteral("check");
            panel_tone_ = QStringLiteral("success");
        } else {
            panel_tone_ = QStringLiteral("accent");
            switch (active) {
            case UpStep::Download:
                panel_title_ = state.verification_reinstall ? QStringLiteral("Downloading update again")
                                                            : QStringLiteral("Downloading update");
                panel_detail_ = QStringLiteral("Fetching and checking the signed update.");
                panel_safety_ = QStringLiteral("Your installed version remains unchanged until installation begins.");
                panel_glyph_ = QStringLiteral("download");
                break;
            case UpStep::CloseApp:
                panel_title_ = QStringLiteral("Closing previous version");
                panel_detail_ = QStringLiteral("Waiting for ExoSnap to close.");
                panel_safety_ = QStringLiteral("The verified package is ready before ExoSnap closes.");
                panel_glyph_ = QStringLiteral("close");
                break;
            case UpStep::Install:
                panel_title_ = state.verification_reinstall ? QStringLiteral("Reinstalling ExoSnap")
                                                            : QStringLiteral("Installing new files");
                panel_detail_ = QStringLiteral("Replacing the application files.");
                panel_safety_ = QStringLiteral("Keep your computer on while the application files are replaced.");
                panel_glyph_ = QStringLiteral("layers");
                break;
            case UpStep::Verify:
                panel_title_ = QStringLiteral("Verifying installation");
                panel_detail_ = QStringLiteral("Checking the installed files.");
                panel_safety_ = QStringLiteral("The installed files are checked against the signed release.");
                panel_glyph_ = QStringLiteral("shield");
                break;
            case UpStep::Launch:
                panel_title_ = QStringLiteral("Launching ExoSnap");
                panel_detail_ = QStringLiteral("Starting the updated app.");
                panel_safety_ = QStringLiteral("ExoSnap reopens automatically when the handoff completes.");
                panel_glyph_ = QStringLiteral("spinner");
                break;
            case UpStep::Count:
                panel_title_ = QStringLiteral("Preparing update");
                panel_detail_ = QStringLiteral("Getting the updater ready.");
                panel_safety_ = QStringLiteral("Your installed version remains unchanged.");
                panel_glyph_ = QStringLiteral("spinner");
                break;
            }
        }
    }

    // Action row.
    const bool critical = state.steps[size_t(UpStep::Install)] == StepStatus::Working ||
                          state.steps[size_t(UpStep::Verify)] == StepStatus::Working ||
                          state.steps[size_t(UpStep::Launch)] == StepStatus::Working;
    primary_action_ = result_terminal ? state.primary_action : QString();
    secondary_action_ = result_terminal ? state.secondary_action : QString();
    if (result_terminal) {
        hint_.clear();
        close_action_visible_ = false;
        close_action_label_.clear();
        close_action_enabled_ = true;
    } else {
        close_action_visible_ = true;
        if (success_done) {
            // Deliberately no hint: the panel already states what happens next.
            hint_.clear();
            close_action_label_ = QStringLiteral("Close");
            close_action_enabled_ = true;
        } else if (!critical) {
            hint_ = QStringLiteral("Cancelling discards this update run.");
            close_action_label_ = QStringLiteral("Cancel update");
            close_action_enabled_ = true;
        } else {
            hint_ = QStringLiteral("This phase cannot be interrupted.");
            close_action_label_ = QStringLiteral("Close");
            close_action_enabled_ = false;
        }
    }

    // Close X. Refuse to interrupt an install, verify or launch in flight; a
    // prompt state has nothing in flight, so closing it is just closing.
    close_blocked_ = critical;
    const bool terminal = state.variant != TerminalVariant::None;
    const bool settled = terminal || prompting;
    close_enabled_ = !close_blocked_;
    close_tooltip_ = close_blocked_ ? QStringLiteral("Please wait - updating")
                      : settled    ? QStringLiteral("Close")
                                   : QStringLiteral("Cancel update and close");
    close_accessible_name_ = close_blocked_ ? QStringLiteral("Close unavailable while updating")
                             : settled     ? QStringLiteral("Close updater")
                                           : QStringLiteral("Cancel update and close");
    cancel_confirmation_required_ = !settled && !close_blocked_;
    if (!cancel_confirmation_required_ && cancel_confirmation_visible_) {
        cancel_confirmation_visible_ = false;
        emit cancelConfirmationVisibleChanged();
    }

    emit stateChanged();
}

void UpdaterViewAdapter::rebuildStepRows() {
    const QStringList labels = stepLabels();
    const bool manual = state_.variant == TerminalVariant::Green;
    QVariantList rows;
    rows.reserve(labels.size());
    for (int i = 0; i < labels.size(); ++i) {
        const StepStatus status = state_.steps[size_t(i)];
        const bool failed_is_manual = manual && status == StepStatus::Failed;
        rows.push_back(QVariantMap{
            {QStringLiteral("label"), labels.at(i)},
            {QStringLiteral("status"), StatusKey(status)},
            {QStringLiteral("tag"), WidgetLabelForStatus(status, failed_is_manual)},
            {QStringLiteral("manual"), failed_is_manual},
            {QStringLiteral("accessible"), QStringLiteral("%1, %2").arg(labels.at(i), WidgetLabelForStatus(
                                                                                   status, failed_is_manual))},
        });
    }
    step_rows_ = std::move(rows);
}

const QString& UpdaterViewAdapter::eyebrow() const noexcept {
    return eyebrow_;
}

const QString& UpdaterViewAdapter::fromVersion() const noexcept {
    return state_.from_version;
}

const QString& UpdaterViewAdapter::toVersion() const noexcept {
    return state_.to_version;
}

bool UpdaterViewAdapter::hasTarget() const noexcept {
    return !state_.to_version.isEmpty();
}

bool UpdaterViewAdapter::failedTerminal() const noexcept {
    return failed_terminal_;
}

qreal UpdaterViewAdapter::ring() const noexcept {
    return state_.ring;
}

bool UpdaterViewAdapter::indeterminate() const noexcept {
    return indeterminate_;
}

const QString& UpdaterViewAdapter::ringTone() const noexcept {
    return ring_tone_;
}

const QString& UpdaterViewAdapter::ringGlyph() const noexcept {
    return ring_glyph_;
}

int UpdaterViewAdapter::ringPercent() const noexcept {
    return ring_percent_;
}

const QString& UpdaterViewAdapter::ringDescription() const noexcept {
    return ring_description_;
}

const QString& UpdaterViewAdapter::statusLine() const noexcept {
    return status_line_;
}

const QString& UpdaterViewAdapter::statusHeadline() const noexcept {
    return status_headline_;
}

const QString& UpdaterViewAdapter::statusGlyph() const noexcept {
    return status_glyph_;
}

const QString& UpdaterViewAdapter::statusTone() const noexcept {
    return status_tone_;
}

bool UpdaterViewAdapter::statusTextVisible() const noexcept {
    return status_text_visible_;
}

const QVariantList& UpdaterViewAdapter::stepRows() const noexcept {
    return step_rows_;
}

const QString& UpdaterViewAdapter::panelKind() const noexcept {
    return panel_kind_;
}

const QString& UpdaterViewAdapter::panelTone() const noexcept {
    return panel_tone_;
}

const QString& UpdaterViewAdapter::panelGlyph() const noexcept {
    return panel_glyph_;
}

const QString& UpdaterViewAdapter::panelTitle() const noexcept {
    return panel_title_;
}

const QString& UpdaterViewAdapter::panelDetail() const noexcept {
    return panel_detail_;
}

const QString& UpdaterViewAdapter::panelSafety() const noexcept {
    return panel_safety_;
}

const QString& UpdaterViewAdapter::primaryAction() const noexcept {
    return primary_action_;
}

const QString& UpdaterViewAdapter::secondaryAction() const noexcept {
    return secondary_action_;
}

const QString& UpdaterViewAdapter::hint() const noexcept {
    return hint_;
}

const QString& UpdaterViewAdapter::closeActionLabel() const noexcept {
    return close_action_label_;
}

bool UpdaterViewAdapter::closeActionVisible() const noexcept {
    return close_action_visible_;
}

bool UpdaterViewAdapter::closeActionEnabled() const noexcept {
    return close_action_enabled_;
}

bool UpdaterViewAdapter::closeEnabled() const noexcept {
    return close_enabled_;
}

const QString& UpdaterViewAdapter::closeTooltip() const noexcept {
    return close_tooltip_;
}

const QString& UpdaterViewAdapter::closeAccessibleName() const noexcept {
    return close_accessible_name_;
}

bool UpdaterViewAdapter::cancelConfirmationVisible() const noexcept {
    return cancel_confirmation_visible_;
}

void UpdaterViewAdapter::setCancelConfirmationVisible(bool visible) {
    if (cancel_confirmation_visible_ == visible)
        return;
    cancel_confirmation_visible_ = visible;
    emit cancelConfirmationVisibleChanged();
}

QStringList UpdaterViewAdapter::footerButtonLabels() const {
    QStringList labels;
    if (close_action_visible_) {
        labels << close_action_label_;
        return labels;
    }
    if (!primary_action_.isEmpty())
        labels << primary_action_;
    if (!secondary_action_.isEmpty())
        labels << secondary_action_;
    return labels;
}

bool UpdaterViewAdapter::requestClose() {
    if (close_blocked_)
        return false;
    if (cancel_confirmation_required_) {
        setCancelConfirmationVisible(true);
        emit cancelConfirmationRequested();
        return false;
    }
    emit closeRequested();
    return true;
}

void UpdaterViewAdapter::confirmCancelAndClose() {
    cancel_confirmation_required_ = false;
    setCancelConfirmationVisible(false);
    emit closeRequested();
}

void UpdaterViewAdapter::dismissCancelConfirmation() {
    setCancelConfirmationVisible(false);
}

void UpdaterViewAdapter::activateAction(const QString& action) {
    if (action == QStringLiteral("Retry") || action == QStringLiteral("Re-download"))
        emit retryRequested();
    else if (action == QStringLiteral("Open ExoSnap"))
        emit openExoSnapRequested();
    else if (action == QStringLiteral("Check for updates") || action == QStringLiteral("Check again"))
        emit checkRequested();
    else if (action == QStringLiteral("Download update"))
        emit downloadRequested();
    else if (action == QStringLiteral("Install now"))
        emit applyRequested();
    else if (action == close_action_label_ && close_action_visible_ && close_action_enabled_)
        (void)requestClose();
    else
        emit closeRequested();
}

} // namespace exosnap::updater
