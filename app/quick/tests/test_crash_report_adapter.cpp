#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QVariantMap>

#include "CrashReportAdapter.h"

// The consent surface. Everything asserted here is a privacy property, not a
// layout one: what a click commits, what it does NOT commit, and that the draft
// tick never leaks between decisions.

namespace exosnap::quick {
namespace {

CrashReportContext MakeContext(bool dump_available = true, bool recording_was_active = false) {
    CrashReportContext context;
    context.dump_available = dump_available;
    context.recording_was_active = recording_was_active;
    context.version = QStringLiteral("0.9.0 · build a5d55f1");
    context.encoder = QStringLiteral("NVENC AV1 → MKV");
    return context;
}

QString RowValue(const QVariantList& rows, const QString& label) {
    for (const QVariant& entry : rows) {
        const QVariantMap row = entry.toMap();
        if (row.value(QStringLiteral("label")).toString() == label)
            return row.value(QStringLiteral("value")).toString();
    }
    return {};
}

class CrashReportAdapterTest : public ::testing::Test {
  protected:
    CrashReportAdapter adapter_;
};

TEST_F(CrashReportAdapterTest, StartsInactive) {
    EXPECT_FALSE(adapter_.active());
    EXPECT_FALSE(adapter_.rememberChoice());
}

TEST_F(CrashReportAdapterTest, SummaryReportsThePreviousSessionsOwnFacts) {
    adapter_.present(MakeContext(), /*crash_folder_available=*/true);

    EXPECT_TRUE(adapter_.active());
    EXPECT_EQ(RowValue(adapter_.summaryRows(), QStringLiteral("SESSION")),
              QStringLiteral("Did not shut down normally"));
    EXPECT_EQ(RowValue(adapter_.summaryRows(), QStringLiteral("CRASH DUMP")), QStringLiteral("Available"));
    // The client never symbolicates; claiming a local cause would be a guess.
    EXPECT_EQ(RowValue(adapter_.summaryRows(), QStringLiteral("CAUSE")), QStringLiteral("Not available locally"));
    EXPECT_EQ(RowValue(adapter_.summaryRows(), QStringLiteral("VERSION")), QStringLiteral("0.9.0 · build a5d55f1"));
}

TEST_F(CrashReportAdapterTest, MissingDumpIsStatedRatherThanHidden) {
    adapter_.present(MakeContext(/*dump_available=*/false), /*crash_folder_available=*/true);

    EXPECT_EQ(RowValue(adapter_.summaryRows(), QStringLiteral("CRASH DUMP")), QStringLiteral("Unavailable"));
    EXPECT_TRUE(adapter_.availabilityText().contains(QStringLiteral("limited session context")));
    // Both variants must still carry the promise the user decides on.
    EXPECT_TRUE(adapter_.availabilityText().contains(QStringLiteral("Nothing is sent unless you choose to")));
}

// ─── What each action commits ────────────────────────────────────────────────

TEST_F(CrashReportAdapterTest, SendWithoutRememberIsAOneShotAndPersistsNothing) {
    adapter_.present(MakeContext(), true);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::decisionMade);

    adapter_.sendReport();

    ASSERT_EQ(spy.count(), 1);
    const auto decision = spy.at(0).at(0).value<CrashReportDecision>();
    EXPECT_TRUE(decision.send_current_report);
    EXPECT_FALSE(decision.persisted_policy.has_value()) << "one report sent is not a standing permission";
    EXPECT_EQ(decision.consent_action, CrashConsentAction::SendPendingOnce);
    EXPECT_FALSE(adapter_.active());
}

TEST_F(CrashReportAdapterTest, SendWithRememberGrantsPersistentConsent) {
    adapter_.present(MakeContext(), true);
    adapter_.setRememberChoice(true);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::decisionMade);

    adapter_.sendReport();

    ASSERT_EQ(spy.count(), 1);
    const auto decision = spy.at(0).at(0).value<CrashReportDecision>();
    ASSERT_TRUE(decision.persisted_policy.has_value());
    EXPECT_EQ(*decision.persisted_policy, CrashReportPolicy::AlwaysSend);
    EXPECT_EQ(decision.consent_action, CrashConsentAction::GrantPersistent);
}

TEST_F(CrashReportAdapterTest, DontSendWithRememberRevokesAndStopsAsking) {
    adapter_.present(MakeContext(), true);
    adapter_.setRememberChoice(true);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::decisionMade);

    adapter_.dontSend();

    ASSERT_EQ(spy.count(), 1);
    const auto decision = spy.at(0).at(0).value<CrashReportDecision>();
    EXPECT_FALSE(decision.send_current_report);
    ASSERT_TRUE(decision.persisted_policy.has_value());
    EXPECT_EQ(*decision.persisted_policy, CrashReportPolicy::NeverSend);
    EXPECT_EQ(decision.consent_action, CrashConsentAction::Revoke);
}

TEST_F(CrashReportAdapterTest, DismissCommitsNothingAtAll) {
    adapter_.present(MakeContext(), true);
    // Even with the tick set: it is draft state until a real button commits it.
    adapter_.setRememberChoice(true);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::decisionMade);

    adapter_.dismiss();

    ASSERT_EQ(spy.count(), 1);
    const auto decision = spy.at(0).at(0).value<CrashReportDecision>();
    EXPECT_FALSE(decision.send_current_report);
    EXPECT_FALSE(decision.persisted_policy.has_value());
    EXPECT_EQ(decision.consent_action, CrashConsentAction::None)
        << "closing the surface must never change a consent state";
    EXPECT_FALSE(adapter_.active());
}

TEST_F(CrashReportAdapterTest, RememberDoesNotSurviveIntoTheNextDecision) {
    adapter_.present(MakeContext(), true);
    adapter_.setRememberChoice(true);
    adapter_.dismiss();

    adapter_.present(MakeContext(), true);

    EXPECT_FALSE(adapter_.rememberChoice()) << "every prompt starts from privacy-by-default";
}

TEST_F(CrashReportAdapterTest, OpenCrashFolderKeepsTheDecisionOnScreen) {
    adapter_.present(MakeContext(), /*crash_folder_available=*/true);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::openCrashFolderRequested);

    adapter_.openCrashFolder();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(adapter_.active()) << "looking at the folder is how the user decides; the surface has to stay";
}

TEST_F(CrashReportAdapterTest, OpenCrashFolderIsRefusedWithoutAFolder) {
    adapter_.present(MakeContext(), /*crash_folder_available=*/false);
    QSignalSpy spy(&adapter_, &CrashReportAdapter::openCrashFolderRequested);

    adapter_.openCrashFolder();

    EXPECT_EQ(spy.count(), 0);
}

TEST_F(CrashReportAdapterTest, ActionsOnAnInactiveSurfaceCommitNothing) {
    QSignalSpy spy(&adapter_, &CrashReportAdapter::decisionMade);

    adapter_.sendReport();
    adapter_.dontSend();
    adapter_.dismiss();

    EXPECT_EQ(spy.count(), 0);
}

// ─── The prompt disposition the composition root reads ───────────────────────

TEST(CrashPromptDisposition, PolicyDecidesWhetherThereIsAnythingToAsk) {
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::AskEveryTime), CrashPromptDisposition::ShowPrompt);
    // Both non-default policies were already applied to the SDK at startup, so
    // prompting would re-ask a question the user answered once and for all.
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::AlwaysSend), CrashPromptDisposition::SuppressAndSend);
    EXPECT_EQ(ResolveCrashPromptDisposition(CrashReportPolicy::NeverSend), CrashPromptDisposition::SuppressWithoutSend);
}

} // namespace
} // namespace exosnap::quick
