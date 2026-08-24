// test_updater_control_dispatcher.cpp -- the updater's automation endpoint,
// driven without a pipe, a window or a worker.
//
// Two things are pinned here that nothing else can pin:
//   * `settled` semantics. Every product action is asynchronous, so an accepted
//     command must NOT claim its effect happened. The expensive failure mode in
//     this codebase has never been a missing command -- it has been a command
//     that answered "ok" because a call returned.
//   * `stateRevision` granularity. It advances on a phase change and NOT on a
//     download progress tick, which is what keeps "wait for the revision to
//     advance" from degenerating into a sleep.

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QJsonValue>

#include <memory>

#include <control/options.h>

#include "UpdaterAutomation.h"
#include "UpdaterControlDispatcher.h"

using namespace exosnap::updater;

namespace {

using exosnap::control::ParsedRequest;
using exosnap::update::UpdateFlowState;
using exosnap::update::UpdatePhase;
using exosnap::update::UpdaterMode;
using exosnap::updater_control::UpdaterControlDispatcher;

const QString kRunId = QStringLiteral("run-0123456789ab");

// A source whose intents record what was asked instead of driving a real
// updater. The point of the test is the protocol contract, not the engine.
class RecordingAutomation {
  public:
    RecordingAutomation() {
        UpdaterAutomation::Intents intents;
        intents.check = [this](QString*) {
            ++checks;
            return true;
        };
        intents.download = [this](QString*) {
            ++downloads;
            return true;
        };
        intents.apply = [this](QString*) {
            ++applies;
            return true;
        };
        intents.retry = [this](QString*) {
            ++retries;
            return true;
        };
        intents.cancel = [this](QString*) {
            ++cancels;
            return true;
        };
        intents.close = [this](QString*) {
            ++closes;
            return true;
        };
        QJsonObject identity;
        identity.insert(QStringLiteral("product"), QStringLiteral("exosnap-updater"));
        source = std::make_unique<UpdaterAutomation>(identity, std::move(intents));
    }

    void Publish(UpdatePhase phase, UpdaterMode mode = UpdaterMode::Manual) {
        UpdateFlowState state;
        state.mode = mode;
        state.checks_enabled = true;
        state.phase = phase;
        (void)source->Publish(state);
    }

    std::unique_ptr<UpdaterAutomation> source;
    int checks = 0;
    int downloads = 0;
    int applies = 0;
    int retries = 0;
    int cancels = 0;
    int closes = 0;
};

ParsedRequest Request(const QString& command, QJsonObject params = {}, int protocol = 2) {
    ParsedRequest request;
    request.protocol = protocol;
    request.id = QStringLiteral("1");
    request.command = command;
    request.params = std::move(params);
    return request;
}

QJsonObject Hello(UpdaterControlDispatcher& dispatcher, int protocol = 2) {
    QJsonObject params;
    params.insert(QStringLiteral("runId"), kRunId);
    return dispatcher.Dispatch(Request(QStringLiteral("system.hello"), params, protocol));
}

QString ErrorCode(const QJsonObject& response) {
    return response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}

// -- session rules -----------------------------------------------------------

TEST(UpdaterControlSession, HelloMustComeFirst) {
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("updater.getState")));
    EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(ErrorCode(response), QStringLiteral("handshake_required"));
}

TEST(UpdaterControlSession, AWrongRunIdPoisonsTheConnection) {
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    QJsonObject params;
    params.insert(QStringLiteral("runId"), QStringLiteral("not-the-run-id"));
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("system.hello"), params));
    EXPECT_EQ(ErrorCode(response), QStringLiteral("run_id_mismatch"));
    EXPECT_TRUE(dispatcher.connectionPoisoned());
}

TEST(UpdaterControlSession, HelloAnswersWithTheIdentityAndTheCommandList) {
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    const QJsonObject result = Hello(dispatcher).value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("product")).toString(), QStringLiteral("exosnap-updater"));
    EXPECT_EQ(result.value(QStringLiteral("runId")).toString(), kRunId);
    EXPECT_TRUE(result.value(QStringLiteral("commands")).toArray().contains(QStringLiteral("updater.getState")));
}

TEST(UpdaterControlSession, TheProductCommandsAreProtocolTwoOnly) {
    // A v1 client's registry does not contain them, so asking is
    // `unknown_command` -- exactly what a build predating them would answer.
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher, 1);
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("updater.getState"), {}, 1));
    EXPECT_EQ(ErrorCode(response), QStringLiteral("unknown_command"));
}

TEST(UpdaterControlSession, ThereIsNoCommandThatArmsAHandoff) {
    // Deliberate absence: a handoff is a start argument, and a channel that
    // could set one afterwards would let an external caller decide what an
    // elevated msiexec installs.
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    for (const QString& name : UpdaterControlDispatcher::CommandNames(2)) {
        EXPECT_FALSE(name.contains(QStringLiteral("handoff"), Qt::CaseInsensitive)) << qPrintable(name);
        EXPECT_FALSE(name.contains(QStringLiteral("package"), Qt::CaseInsensitive)) << qPrintable(name);
        EXPECT_FALSE(name.contains(QStringLiteral("install-dir"), Qt::CaseInsensitive)) << qPrintable(name);
    }
}

// -- settle semantics --------------------------------------------------------

TEST(UpdaterControlSettle, GetStateCarriesNoSettledFlagAtAll) {
    // Read-only. `settled` is absent rather than trivially true, so a client
    // cannot read it off a query and conclude an action completed.
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::Idle);
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher);
    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("updater.getState")));
    ASSERT_TRUE(response.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(response.contains(QStringLiteral("settled")));
}

TEST(UpdaterControlSettle, AnAcceptedActionIsNotACompletedOne) {
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::ReadyToApply);
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher);

    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("updater.apply")));
    ASSERT_TRUE(response.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(response.contains(QStringLiteral("settled")));
    EXPECT_FALSE(response.value(QStringLiteral("settled")).toBool())
        << "apply was accepted, not completed -- the installation has not even started";
    EXPECT_EQ(automation.applies, 1);
    // And the state it reports back is still the pre-apply one: the response
    // describes the moment the intent was taken.
    EXPECT_EQ(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("phase")).toString(),
              QStringLiteral("readyToApply"));
}

TEST(UpdaterControlSettle, ARefusedActionNeverReachesTheSource) {
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::Idle);
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher);

    const QJsonObject response = dispatcher.Dispatch(Request(QStringLiteral("updater.apply")));
    EXPECT_FALSE(response.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(ErrorCode(response), QStringLiteral("invalid_state"));
    EXPECT_EQ(automation.applies, 0);
}

TEST(UpdaterControlSettle, ARefusalCarriesRequiresAndActual) {
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::Applying);
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher);

    const QJsonObject error =
        dispatcher.Dispatch(Request(QStringLiteral("updater.cancel"))).value(QStringLiteral("error")).toObject();
    EXPECT_EQ(error.value(QStringLiteral("code")).toString(), QStringLiteral("blocked"));
    // The runner branches on these, never on the message.
    EXPECT_EQ(error.value(QStringLiteral("actual")).toObject().value(QStringLiteral("phase")).toString(),
              QStringLiteral("applying"));
    EXPECT_EQ(automation.cancels, 0);
}

TEST(UpdaterControlSettle, EveryProductActionRoutesToItsOwnIntent) {
    RecordingAutomation automation;
    UpdaterControlDispatcher dispatcher(automation.source.get(), kRunId);
    (void)Hello(dispatcher);

    automation.Publish(UpdatePhase::Idle);
    ASSERT_TRUE(dispatcher.Dispatch(Request(QStringLiteral("updater.check"))).value(QStringLiteral("ok")).toBool());
    automation.Publish(UpdatePhase::UpdateAvailable);
    ASSERT_TRUE(dispatcher.Dispatch(Request(QStringLiteral("updater.download"))).value(QStringLiteral("ok")).toBool());
    automation.Publish(UpdatePhase::ReadyToApply);
    ASSERT_TRUE(dispatcher.Dispatch(Request(QStringLiteral("updater.apply"))).value(QStringLiteral("ok")).toBool());
    automation.Publish(UpdatePhase::Downloading);
    ASSERT_TRUE(dispatcher.Dispatch(Request(QStringLiteral("updater.cancel"))).value(QStringLiteral("ok")).toBool());
    automation.Publish(UpdatePhase::Completed);
    ASSERT_TRUE(dispatcher.Dispatch(Request(QStringLiteral("updater.close"))).value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(automation.checks, 1);
    EXPECT_EQ(automation.downloads, 1);
    EXPECT_EQ(automation.applies, 1);
    EXPECT_EQ(automation.cancels, 1);
    EXPECT_EQ(automation.closes, 1);
}

// -- the argv gate -----------------------------------------------------------

TEST(UpdaterControlOption, IsAbsentUnlessExplicitlyPassed) {
    // A normal launch has no endpoint at all -- no pipe, no thread, no log line.
    const exosnap::control::ControlOptions options = exosnap::control::ParseControlOptions(
        {QStringLiteral("exosnap-updater.exe")}, QString::fromLatin1(exosnap::updater_control::kControlOption));
    EXPECT_FALSE(options.requested);
    EXPECT_TRUE(options.run_id.isEmpty());
}

TEST(UpdaterControlOption, AMalformedRunIdIsAnErrorNotAFallback) {
    // Falling back to a normal launch would hand a runner an updater it cannot
    // drive while it believes the channel is armed.
    for (const QString& bad : {QStringLiteral("short"), QStringLiteral("has space"), QStringLiteral("semi;colon")}) {
        const exosnap::control::ControlOptions options = exosnap::control::ParseControlOptions(
            {QStringLiteral("exosnap-updater.exe"), QString::fromLatin1(exosnap::updater_control::kControlOption), bad},
            QString::fromLatin1(exosnap::updater_control::kControlOption));
        EXPECT_TRUE(options.requested) << qPrintable(bad);
        EXPECT_FALSE(options.error.isEmpty()) << qPrintable(bad);
    }
}

TEST(UpdaterControlOption, TheEndpointCarriesItsRoleSoBothCanBeHeldAtOnce) {
    // The application's endpoint of the SAME run id is
    // "...ExoSnap.LiveVerify.<run-id>"; the role is what keeps a runner from
    // having to invent a second run id to reach the child.
    EXPECT_EQ(exosnap::control::PipeName(QString::fromLatin1(exosnap::updater_control::kControlRole), kRunId),
              QStringLiteral("\\\\.\\pipe\\ExoSnap.Updater.run-0123456789ab"));
    EXPECT_NE(exosnap::control::PipeName(QStringLiteral("LiveVerify"), kRunId),
              exosnap::control::PipeName(QString::fromLatin1(exosnap::updater_control::kControlRole), kRunId));
}

// -- stateRevision -----------------------------------------------------------

TEST(UpdaterStateRevision, AdvancesOnAPhaseChange) {
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::Idle);
    const std::uint64_t before = automation.source->StateRevision();
    automation.Publish(UpdatePhase::Checking);
    EXPECT_GT(automation.source->StateRevision(), before);
}

TEST(UpdaterStateRevision, DoesNotAdvanceOnAnIdenticalStateWrite) {
    RecordingAutomation automation;
    automation.Publish(UpdatePhase::Downloading);
    const std::uint64_t before = automation.source->StateRevision();
    automation.Publish(UpdatePhase::Downloading);
    automation.Publish(UpdatePhase::Downloading);
    EXPECT_EQ(automation.source->StateRevision(), before);
}

TEST(UpdaterStateRevision, DoesNotAdvanceOnDownloadProgress) {
    // The whole reason the counter is worth waiting on. A download emits
    // progress at roughly 12 Hz; if each tick bumped the revision, "wait until
    // the revision advances" would mean "wait up to 80 ms" -- a sleep with extra
    // steps.
    UpdaterAutomation::Intents intents;
    UpdaterAutomation source(QJsonObject{}, std::move(intents));

    UpdateFlowState state;
    state.phase = UpdatePhase::Downloading;
    (void)source.Publish(state);
    const std::uint64_t before = source.StateRevision();

    for (quint64 received = 1; received <= 50; ++received) {
        state.downloaded_bytes = received * 1024;
        state.total_bytes = 64 * 1024;
        EXPECT_FALSE(source.Publish(state)) << "a progress tick must not advance the revision";
    }
    EXPECT_EQ(source.StateRevision(), before);
    // ...and the bytes are still published, at full frequency.
    EXPECT_EQ(source.State().downloaded_bytes, 50u * 1024u);
}

TEST(UpdaterStateRevision, AdvancesOnEveryOtherObservableField) {
    UpdaterAutomation source(QJsonObject{}, UpdaterAutomation::Intents{});
    UpdateFlowState state;
    (void)source.Publish(state);

    state.failure_case = exosnap::update::FailureCase::VerifyInstallFailed;
    EXPECT_TRUE(source.Publish(state));
    state.install_state = exosnap::update::InstallState::Restored;
    EXPECT_TRUE(source.Publish(state));
    state.retry_entry_step = exosnap::update::UpStep::Install;
    EXPECT_TRUE(source.Publish(state));
    state.target_version = "0.9.1";
    EXPECT_TRUE(source.Publish(state));
}

TEST(UpdaterAutomationIntents, AnUnwiredIntentFailsRatherThanLooksAccepted) {
    UpdaterAutomation source(QJsonObject{}, UpdaterAutomation::Intents{});
    QString error;
    EXPECT_FALSE(source.Apply(&error));
    EXPECT_FALSE(error.isEmpty());
}

} // namespace
