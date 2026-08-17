// The Live Verify precondition policy, on its own.
//
// The single property everything else in the control channel leans on is here:
// `availableActions` and dispatch validation read the SAME predicates. If they
// ever came apart, a runner would be told an action is available, send it, and
// be refused -- with a transcript that contradicts itself. AvailabilityAgreesWith
// Evaluation below is the test that makes that impossible to introduce.

#include "live_verify/LiveVerifyAutomationState.h"
#include "live_verify/LiveVerifyCommandPolicy.h"
#include "live_verify/LiveVerifyProtocol.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

using namespace exosnap::live_verify;

namespace {

AutomationState Ready() {
    AutomationState state;
    state.page = QString::fromLatin1(page_name::kRecord);
    state.recording_state = QStringLiteral("Ready");
    state.can_start = true;
    state.can_select_source = true;
    return state;
}

AutomationState Recording() {
    AutomationState state;
    state.page = QString::fromLatin1(page_name::kRecord);
    state.recording_state = QStringLiteral("Recording");
    state.can_stop = true;
    state.can_pause = true;
    state.can_split = true;
    state.can_capture_frame = true;
    return state;
}

AutomationState EditOpenOnRecord() {
    AutomationState state = Ready();
    state.recording_state = QStringLiteral("Completed");
    state.can_start = true;
    state.can_open_edit = true;
    state.edit_session_open = true;
    state.edit_visible = true;
    state.edit_playback = QStringLiteral("paused");
    return state;
}

// Every state worth sweeping the whole table against. Deliberately includes the
// awkward combinations -- a blocking surface over an open edit session, a
// running export, a blocked transport -- because those are where two tables
// would disagree first.
AutomationState UpdateOffered();

QVector<AutomationState> InterestingStates() {
    QVector<AutomationState> states = {Ready(), Recording(), EditOpenOnRecord(), UpdateOffered()};

    // The update area, in the three shapes that decide whether its commands are
    // reachable: an offer, an offer the product currently refuses to act on, and
    // a card with nothing to apply.
    AutomationState update_blocked = UpdateOffered();
    update_blocked.update_blocker = QStringLiteral("recording");
    states.append(update_blocked);
    AutomationState update_none = UpdateOffered();
    update_none.update_state = QStringLiteral("uptodate");
    update_none.update_available = false;
    update_none.update_available_version.clear();
    states.append(update_none);

    AutomationState blocked_transport = Ready();
    blocked_transport.recording_state = QStringLiteral("Blocked");
    blocked_transport.can_start = false;
    states.append(blocked_transport);

    for (const char* surface : {blocking_surface_name::kRecovery, blocking_surface_name::kCrashReport,
                                blocking_surface_name::kRecordingError}) {
        AutomationState under_surface = EditOpenOnRecord();
        under_surface.blocking_surface = QString::fromLatin1(surface);
        states.append(under_surface);
    }

    AutomationState exporting = EditOpenOnRecord();
    exporting.edit_export_running = true;
    states.append(exporting);

    for (const char* page : {page_name::kSettings, page_name::kDiagnostics, page_name::kLogs, page_name::kAbout}) {
        AutomationState elsewhere = EditOpenOnRecord();
        elsewhere.page = QString::fromLatin1(page);
        elsewhere.edit_visible = false;
        states.append(elsewhere);
    }

    AutomationState popups_open = Ready();
    popups_open.source_picker_open = true;
    popups_open.notification_hub_open = true;
    states.append(popups_open);

    return states;
}

QStringList ActionsIn(const QJsonObject& state) {
    QStringList actions;
    for (const QJsonValue& value : state.value(QStringLiteral("availableActions")).toArray())
        actions.append(value.toString());
    return actions;
}

AutomationState UpdateOffered() {
    AutomationState state = Ready();
    state.update_state = QStringLiteral("available");
    state.update_channel = QStringLiteral("stable");
    state.update_current_version = QStringLiteral("0.9.0");
    state.update_available_version = QStringLiteral("0.9.1");
    state.update_available = true;
    state.update_action_enabled = true;
    return state;
}

} // namespace

TEST(LiveVerifyPolicy, AvailabilityAgreesWithEvaluationInEveryState) {
    for (const AutomationState& state : InterestingStates()) {
        const QStringList available = AvailableActions(state);
        for (const CommandDescriptor& command : AllCommands()) {
            const bool allowed = Evaluate(command, state).allowed();
            const bool listed = available.contains(command.name);
            if (!command.mutating) {
                // Queries are always answerable and deliberately not "actions":
                // a list containing every snapshot would be a list of things
                // that are always true.
                EXPECT_FALSE(listed) << command.name.toStdString();
                continue;
            }
            EXPECT_EQ(allowed, listed) << command.name.toStdString() << " in " << state.page.toStdString() << "/"
                                       << state.recording_state.toStdString();
        }
    }
}

TEST(LiveVerifyPolicy, ARefusalAlwaysNamesItsCauseOnBothSides) {
    // The whole point of requires/actual: a runner answers "why" without reading
    // a word of English, so the keys must match and the values must differ.
    for (const AutomationState& state : InterestingStates()) {
        for (const CommandDescriptor& command : AllCommands()) {
            const PreconditionVerdict verdict = Evaluate(command, state);
            if (verdict.allowed())
                continue;
            EXPECT_FALSE(verdict.message.isEmpty()) << command.name.toStdString();
            ASSERT_EQ(verdict.requirements.keys(), verdict.actual.keys()) << command.name.toStdString();
            ASSERT_FALSE(verdict.requirements.isEmpty()) << command.name.toStdString();
            for (const QString& key : verdict.requirements.keys()) {
                EXPECT_NE(verdict.requirements.value(key), verdict.actual.value(key))
                    << command.name.toStdString() << "/" << key.toStdString();
            }
        }
    }
}

TEST(LiveVerifyPolicy, APreconditionOnlyEverAnswersInvalidStateOrBlocked) {
    for (const AutomationState& state : InterestingStates()) {
        for (const CommandDescriptor& command : AllCommands()) {
            const PreconditionVerdict verdict = Evaluate(command, state);
            if (verdict.allowed())
                continue;
            EXPECT_TRUE(verdict.code == QLatin1String(error_code::kInvalidState) ||
                        verdict.code == QLatin1String(error_code::kBlocked))
                << command.name.toStdString() << ": " << verdict.code.toStdString();
        }
    }
}

TEST(LiveVerifyPolicy, ReadyOnRecordOffersExactlyTheActionsTheSurfaceOffers) {
    const QStringList actions = AvailableActions(Ready());
    for (const QString& expected :
         {QStringLiteral("ui.navigate"), QStringLiteral("record.start"), QStringLiteral("record.selectTarget"),
          QStringLiteral("sourcePicker.open"), QStringLiteral("notificationHub.open")}) {
        EXPECT_TRUE(actions.contains(expected)) << expected.toStdString();
    }
    // Nothing from a state the user is not in.
    for (const QString& absent :
         {QStringLiteral("record.stop"), QStringLiteral("record.pause"), QStringLiteral("edit.seek"),
          QStringLiteral("edit.close"), QStringLiteral("ui.reveal")}) {
        EXPECT_FALSE(actions.contains(absent)) << absent.toStdString();
    }
}

TEST(LiveVerifyPolicy, AnOpenEditSessionOffersTheWholeEditSurface) {
    const QStringList actions = AvailableActions(EditOpenOnRecord());
    for (const QString& expected :
         {QStringLiteral("edit.playPause"), QStringLiteral("edit.seek"), QStringLiteral("edit.setTrimIn"),
          QStringLiteral("edit.setTrimOut"), QStringLiteral("edit.timelineHome"), QStringLiteral("edit.timelineEnd"),
          QStringLiteral("edit.close"), QStringLiteral("ui.navigate")}) {
        EXPECT_TRUE(actions.contains(expected)) << expected.toStdString();
    }
}

TEST(LiveVerifyPolicy, ABlockingSurfaceShrinksTheListWithoutEmptyingTheTransport) {
    AutomationState state = Recording();
    state.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecordingError);
    const QStringList actions = AvailableActions(state);

    // The shell is off limits while an unanswered question is on screen.
    EXPECT_FALSE(actions.contains(QStringLiteral("ui.navigate")));
    EXPECT_FALSE(actions.contains(QStringLiteral("sourcePicker.open")));
    EXPECT_FALSE(actions.contains(QStringLiteral("record.start")));
    // The transport is not: a running recording that cannot be stopped is the
    // worse state, and that asymmetry is the product's.
    EXPECT_TRUE(actions.contains(QStringLiteral("record.stop")));
    EXPECT_TRUE(actions.contains(QStringLiteral("record.pause")));
}

TEST(LiveVerifyPolicy, ScrollingIsOfferedOnlyWhereSomethingScrolls) {
    EXPECT_FALSE(IsScrollableSurface(QString::fromLatin1(page_name::kRecord)));
    EXPECT_FALSE(IsScrollableSurface(QString::fromLatin1(page_name::kAbout)));
    EXPECT_TRUE(IsScrollableSurface(QString::fromLatin1(page_name::kSettings)));
    EXPECT_TRUE(IsScrollableSurface(QString::fromLatin1(page_name::kDiagnostics)));
    EXPECT_TRUE(IsScrollableSurface(QString::fromLatin1(page_name::kLogs)));

    AutomationState on_settings = Ready();
    on_settings.page = QString::fromLatin1(page_name::kSettings);
    const QStringList actions = AvailableActions(on_settings);
    EXPECT_TRUE(actions.contains(QStringLiteral("ui.reveal")));
    EXPECT_TRUE(actions.contains(QStringLiteral("ui.scrollHome")));
    EXPECT_TRUE(actions.contains(QStringLiteral("ui.scrollEnd")));
}

TEST(LiveVerifyPolicy, StartIsBlockedByASurfaceAndInvalidByAState) {
    AutomationState under_surface = Ready();
    under_surface.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecovery);
    const CommandDescriptor* start = FindCommand(QStringLiteral("record.start"));
    ASSERT_NE(start, nullptr);
    EXPECT_EQ(Evaluate(*start, under_surface).code, QLatin1String(error_code::kBlocked));

    AutomationState mid_recording = Recording();
    EXPECT_EQ(Evaluate(*start, mid_recording).code, QLatin1String(error_code::kInvalidState));

    AutomationState diagnostics_blocker = Ready();
    diagnostics_blocker.recording_state = QStringLiteral("Blocked");
    diagnostics_blocker.can_start = false;
    EXPECT_EQ(Evaluate(*start, diagnostics_blocker).code, QLatin1String(error_code::kBlocked));
}

TEST(LiveVerifyPolicy, TheStateSnapshotCarriesTheSameActionsTheTableDoes) {
    for (const AutomationState& state : InterestingStates()) {
        EXPECT_EQ(ActionsIn(StateToJson(state, 7)), AvailableActions(state));
    }
}

TEST(LiveVerifyPolicy, SettingsAndProfilesAreLockedWhileARecordingIsInFlight) {
    AutomationState ready = Ready();
    ready.profile_id = QStringLiteral("preset.mine");
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("settings.set")), ready).allowed());
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("profiles.select")), ready).allowed());

    for (const char* live : {"Countdown", "Preparing", "Recording", "Paused", "Stopping", "Saving"}) {
        AutomationState state = ready;
        state.recording_state = QString::fromLatin1(live);
        const PreconditionVerdict verdict = Evaluate(*FindCommand(QStringLiteral("settings.set")), state);
        EXPECT_FALSE(verdict.allowed()) << live;
        // `blocked`, not `invalid_state`: the state is a perfectly ordinary one
        // and a product rule refuses anyway -- which is what the Settings
        // controls do while the transport is live.
        EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kBlocked)) << live;
        EXPECT_FALSE(AvailableActions(state).contains(QStringLiteral("settings.set"))) << live;
    }

    // Reading is never refused. An observation a product state can turn off is
    // useless in exactly the state a check is trying to explain.
    AutomationState recording = ready;
    recording.recording_state = QStringLiteral("Recording");
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("settings.get")), recording).allowed());
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("settings.describe")), recording).allowed());
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("profiles.list")), recording).allowed());
}

TEST(LiveVerifyPolicy, ABuiltInProfileCannotBeRenamedOrDeleted) {
    AutomationState state = Ready();
    state.profile_id = QStringLiteral("preset.default");
    state.profile_built_in = true;

    for (const char* command : {"profiles.rename", "profiles.delete"}) {
        const PreconditionVerdict verdict = Evaluate(*FindCommand(QString::fromLatin1(command)), state);
        EXPECT_FALSE(verdict.allowed()) << command;
        EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kBlocked)) << command;
        EXPECT_FALSE(AvailableActions(state).contains(QString::fromLatin1(command))) << command;
    }
    // Selecting one is fine -- read-only is about editing it, not using it.
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("profiles.select")), state).allowed());

    state.profile_built_in = false;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("profiles.delete")), state).allowed());
}

TEST(LiveVerifyPolicy, BlockingSurfaceActionsAreOfferedOnlyByTheirOwnSurface) {
    // The inverse of every other precondition in the table: these commands
    // REQUIRE a blocking surface, because they are that surface's own buttons.
    const std::pair<const char*, const char*> owned[] = {
        {"recovery.dismiss", blocking_surface_name::kRecovery},
        {"crashReport.send", blocking_surface_name::kCrashReport},
        {"crashReport.decline", blocking_surface_name::kCrashReport},
        {"recordingError.dismiss", blocking_surface_name::kRecordingError},
    };

    for (const auto& [command, surface] : owned) {
        AutomationState absent = Ready();
        EXPECT_FALSE(Evaluate(*FindCommand(QString::fromLatin1(command)), absent).allowed()) << command;
        EXPECT_FALSE(AvailableActions(absent).contains(QString::fromLatin1(command))) << command;

        AutomationState present = Ready();
        present.blocking_surface = QString::fromLatin1(surface);
        EXPECT_TRUE(Evaluate(*FindCommand(QString::fromLatin1(command)), present).allowed()) << command;
        EXPECT_TRUE(AvailableActions(present).contains(QString::fromLatin1(command))) << command;

        // And never by a DIFFERENT surface: answering the crash question is not
        // the same as answering the recovery one.
        AutomationState other = Ready();
        other.blocking_surface = QString::fromLatin1(surface == QLatin1String(blocking_surface_name::kRecovery)
                                                         ? blocking_surface_name::kCrashReport
                                                         : blocking_surface_name::kRecovery);
        EXPECT_FALSE(Evaluate(*FindCommand(QString::fromLatin1(command)), other).allowed()) << command;
    }
}

TEST(LiveVerifyPolicy, RecoveryActionsNeedACandidateAndAFailureReportNeedsConsent) {
    AutomationState empty = Ready();
    empty.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecovery);
    empty.recovery_candidate_count = 0;
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("recovery.continue")), empty).allowed());
    // Dismissing an empty surface is still a real thing to do.
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("recovery.dismiss")), empty).allowed());

    empty.recovery_candidate_count = 2;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("recovery.continue")), empty).allowed());

    AutomationState failure = Ready();
    failure.blocking_surface = QString::fromLatin1(blocking_surface_name::kRecordingError);
    failure.recording_error_can_send_report = false;
    const PreconditionVerdict refused = Evaluate(*FindCommand(QStringLiteral("recordingError.sendReport")), failure);
    EXPECT_FALSE(refused.allowed());
    // Consent, or no report to send. Either way the button is not there, and
    // the command must not be either.
    EXPECT_EQ(refused.code, QString::fromLatin1(error_code::kBlocked));
    failure.recording_error_can_send_report = true;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("recordingError.sendReport")), failure).allowed());
}

TEST(LiveVerifyPolicy, ExportStartAndCancelAnswerToOppositeStates) {
    AutomationState open = Ready();
    open.edit_session_open = true;
    open.can_export = true;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("export.start")), open).allowed());
    // Nothing to cancel yet.
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("export.cancel")), open).allowed());

    AutomationState running = open;
    running.edit_export_running = true;
    running.can_export = false;
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("export.start")), running).allowed());
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("export.cancel")), running).allowed());
    // The same running export is what stops the session being closed under it.
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("edit.close")), running).allowed());

    // Without an edit session there is nothing to export at all.
    AutomationState closed = Ready();
    closed.can_export = true;
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("export.start")), closed).allowed());
}

TEST(LiveVerifyPolicy, MarkerAndCountdownCancelFollowTheTransportsOwnPredicates) {
    AutomationState ready = Ready();
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("record.addMarker")), ready).allowed());
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("record.cancelCountdown")), ready).allowed());

    AutomationState counting = ready;
    counting.recording_state = QStringLiteral("Countdown");
    counting.countdown_active = true;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("record.cancelCountdown")), counting).allowed());
    // A marker belongs to a running recording, and a countdown is not one yet.
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("record.addMarker")), counting).allowed());

    AutomationState recording = ready;
    recording.recording_state = QStringLiteral("Recording");
    recording.can_add_marker = true;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("record.addMarker")), recording).allowed());
    EXPECT_FALSE(Evaluate(*FindCommand(QStringLiteral("record.cancelCountdown")), recording).allowed());
}

TEST(LiveVerifyPolicy, NoCommandIsListedTwiceAndEveryOneIsFindable) {
    QStringList names;
    for (const CommandDescriptor& command : AllCommands()) {
        EXPECT_FALSE(names.contains(command.name)) << command.name.toStdString();
        names.append(command.name);
        EXPECT_EQ(FindCommand(command.name), &command);
        EXPECT_TRUE(command.minimum_protocol >= kMinimumProtocolVersion &&
                    command.minimum_protocol <= kLatestProtocolVersion)
            << command.name.toStdString();
    }
    EXPECT_EQ(FindCommand(QStringLiteral("nothing.likeThis")), nullptr);
}

TEST(LiveVerifyPolicy, EveryDescribedParameterIsOneTheValidatorUnderstands) {
    for (const CommandDescriptor& command : AllCommands()) {
        for (const CommandParameter& parameter : command.parameters) {
            EXPECT_FALSE(parameter.name.isEmpty()) << command.name.toStdString();
            // "any" is a declared decision, not a hole: an unlisted type is
            // silently accepted by the shared validator, so this list is what
            // turns a typo into a failure.
            EXPECT_TRUE(parameter.type == QLatin1String("string") || parameter.type == QLatin1String("int") ||
                        parameter.type == QLatin1String("bool") || parameter.type == QLatin1String("enum") ||
                        parameter.type == QLatin1String("any"))
                << command.name.toStdString() << "/" << parameter.type.toStdString();
            EXPECT_EQ(parameter.type == QLatin1String("enum"), !parameter.values.isEmpty())
                << command.name.toStdString() << "/" << parameter.name.toStdString();
        }
    }
}

TEST(LiveVerifyPolicy, DescribeIsFilteredByProtocol) {
    const QJsonArray v1 = DescribeCommands(1).value(QStringLiteral("commands")).toArray();
    const QJsonArray v2 = DescribeCommands(2).value(QStringLiteral("commands")).toArray();
    EXPECT_EQ(v1.size(), CommandNamesForProtocol(1).size());
    EXPECT_EQ(v2.size(), CommandNamesForProtocol(2).size());
    EXPECT_GT(v2.size(), v1.size());
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

TEST(LiveVerifyUpdatePolicy, ApplyNeedsAnOfferedUpdate) {
    // The card's primary button re-checks in every state that is not an offer.
    // Accepting update.apply there would report an update starting when a check
    // started -- the false success this protocol version exists to remove.
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("update.apply")), UpdateOffered()).allowed());

    for (const QString& card : {QStringLiteral("unchecked"), QStringLiteral("uptodate"), QStringLiteral("checking"),
                                QStringLiteral("scoop"), QStringLiteral("pending"), QStringLiteral("error")}) {
        AutomationState state = UpdateOffered();
        state.update_state = card;
        const PreconditionVerdict verdict = Evaluate(*FindCommand(QStringLiteral("update.apply")), state);
        EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kInvalidState)) << card.toStdString();
        EXPECT_EQ(verdict.actual.value(QStringLiteral("updateState")).toString(), card);
        EXPECT_FALSE(AvailableActions(state).contains(QStringLiteral("update.apply"))) << card.toStdString();
    }
}

TEST(LiveVerifyUpdatePolicy, AVerificationReinstallIsAlsoAnOffer) {
    // ADR 0055: the offered version IS the running one, on purpose, and its
    // button launches the updater exactly like a normal update's.
    AutomationState state = UpdateOffered();
    state.update_state = QStringLiteral("verify-reinstall");
    state.update_available_version = state.update_current_version;
    EXPECT_TRUE(Evaluate(*FindCommand(QStringLiteral("update.apply")), state).allowed());
}

TEST(LiveVerifyUpdatePolicy, ADisabledCardActionBlocksApply) {
    // `blocked`, not `invalid_state`: the card is offering an update and the
    // product still refuses to act on it right now.
    AutomationState state = UpdateOffered();
    state.update_action_enabled = false;
    const PreconditionVerdict verdict = Evaluate(*FindCommand(QStringLiteral("update.apply")), state);
    EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kBlocked));
    EXPECT_EQ(verdict.requirements.value(QStringLiteral("updateActionEnabled")).toBool(), true);
    EXPECT_EQ(verdict.actual.value(QStringLiteral("updateActionEnabled")).toBool(), false);
}

TEST(LiveVerifyUpdatePolicy, EveryBlockerRefusesBothCommandsWithItsReason) {
    // One rule, read by the card's own guard and by these preconditions: a
    // client cannot be told an action is available and then refused by the
    // intent behind it.
    for (const QString& blocker :
         {QStringLiteral("recording"), QStringLiteral("finalizing"), QStringLiteral("updaterRunning")}) {
        AutomationState state = UpdateOffered();
        state.update_blocker = blocker;
        for (const QString& name : {QStringLiteral("update.check"), QStringLiteral("update.apply")}) {
            const PreconditionVerdict verdict = Evaluate(*FindCommand(name), state);
            EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kBlocked))
                << name.toStdString() << " under " << blocker.toStdString();
            EXPECT_EQ(verdict.actual.value(QStringLiteral("updateBlocker")).toString(), blocker);
            EXPECT_FALSE(AvailableActions(state).contains(name));
        }
    }
}

TEST(LiveVerifyUpdatePolicy, ASecondCheckWhileOneRunsIsInvalidState) {
    // Single-flight is the engine's own rule; a second request would be admitted
    // and then discarded, which is not something to report as accepted.
    AutomationState state = UpdateOffered();
    state.update_state = QStringLiteral("checking");
    state.update_checking = true;
    const PreconditionVerdict verdict = Evaluate(*FindCommand(QStringLiteral("update.check")), state);
    EXPECT_EQ(verdict.code, QString::fromLatin1(error_code::kInvalidState));
}

TEST(LiveVerifyUpdatePolicy, BothActionsAreAsynchronous) {
    // A check answers through the card's next state; an apply's real completion
    // is a different process entirely. Neither may claim `settled`.
    for (const QString& name : {QStringLiteral("update.check"), QStringLiteral("update.apply")}) {
        const CommandDescriptor* command = FindCommand(name);
        ASSERT_NE(command, nullptr) << name.toStdString();
        EXPECT_TRUE(command->mutating) << name.toStdString();
        EXPECT_EQ(command->settle, Settle::Asynchronous) << name.toStdString();
    }
    const CommandDescriptor* query = FindCommand(QStringLiteral("update.getState"));
    ASSERT_NE(query, nullptr);
    EXPECT_FALSE(query->mutating);
    EXPECT_EQ(query->settle, Settle::NotApplicable);
}

TEST(LiveVerifyUpdatePolicy, TheStateSnapshotCarriesTheUpdateFacts) {
    const QJsonObject update = StateToJson(UpdateOffered(), 7).value(QStringLiteral("update")).toObject();
    EXPECT_EQ(update.value(QStringLiteral("state")).toString(), QStringLiteral("available"));
    EXPECT_EQ(update.value(QStringLiteral("availableVersion")).toString(), QStringLiteral("0.9.1"));
    EXPECT_EQ(update.value(QStringLiteral("currentVersion")).toString(), QStringLiteral("0.9.0"));
    EXPECT_TRUE(update.value(QStringLiteral("updateAvailable")).toBool());
    EXPECT_TRUE(update.value(QStringLiteral("blocker")).isNull());

    AutomationState none = Ready();
    const QJsonObject empty = StateToJson(none, 1).value(QStringLiteral("update")).toObject();
    EXPECT_TRUE(empty.value(QStringLiteral("availableVersion")).isNull())
        << "an empty string is a value; null is \"there is none\"";
}
