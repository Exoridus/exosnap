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
QVector<AutomationState> InterestingStates() {
    QVector<AutomationState> states = {Ready(), Recording(), EditOpenOnRecord()};

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
            EXPECT_TRUE(parameter.type == QLatin1String("string") || parameter.type == QLatin1String("int") ||
                        parameter.type == QLatin1String("bool") || parameter.type == QLatin1String("enum"))
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
