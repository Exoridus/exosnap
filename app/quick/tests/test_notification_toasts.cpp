#include <gtest/gtest.h>

#include <QSignalSpy>

#include "NotificationHubPolicy.h"
#include "NotificationToastModel.h"
#include "NotificationsAdapter.h"
#include "models/RecordingFailurePolicy.h"

// The out-of-window toast stack and the low-storage producer.
//
// The manager already owns the visibility rules (one timed toast at a time,
// standing toasts stack, timed toast last) and notification_manager_tests covers
// them. What is new here is that the Quick frontend MIRRORS that set faithfully
// instead of keeping a second idea of what is on screen, and that the one
// low-storage producer maps the disk-guard auto-stop onto the right event.

namespace exosnap::quick {
namespace {

using notifications::NotificationAction;
using notifications::NotificationEvent;
using notifications::NotificationType;

NotificationEvent MakeEvent(NotificationType type, const QString& title,
                            NotificationAction action = NotificationAction::None,
                            NotificationAction secondary = NotificationAction::None) {
    NotificationEvent event;
    event.type = type;
    event.title = title;
    event.body = QStringLiteral("body");
    event.action = action;
    event.secondary_action = secondary;
    event.action_payload = QStringLiteral("C:/out/clip.mkv");
    return event;
}

QVariant RoleAt(QAbstractItemModel* model, int row, const char* name) {
    const QHash<int, QByteArray> roles = model->roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == name)
            return model->data(model->index(row, 0), it.key());
    }
    return {};
}

class ToastStackTest : public ::testing::Test {
  protected:
    NotificationsAdapter adapter_;
};

// The saved toast is the first and only place a finished session's problem count
// is stated: nothing is raised while the recording is still running.
TEST(SavedRecordingBodyTest, ACleanRecordingIsJustItsName) {
    EXPECT_EQ(notifications::SavedRecordingBody(QStringLiteral("clip.mkv"), 0), QStringLiteral("clip.mkv"));
    EXPECT_EQ(notifications::SavedRecordingBody(QStringLiteral("clip.mkv"), -1), QStringLiteral("clip.mkv"));
}

TEST(SavedRecordingBodyTest, ObservedProblemsAreCountedAndPluralized) {
    EXPECT_EQ(notifications::SavedRecordingBody(QStringLiteral("clip.mkv"), 1),
              QString::fromUtf8("clip.mkv \xc2\xb7 1 problem observed"));
    EXPECT_EQ(notifications::SavedRecordingBody(QStringLiteral("clip.mkv"), 2),
              QString::fromUtf8("clip.mkv \xc2\xb7 2 problems observed"));
}

TEST_F(ToastStackTest, StartsEmpty) {
    EXPECT_EQ(adapter_.toastModel()->rowCount(), 0);
}

TEST_F(ToastStackTest, MirrorsTheManagersVisibleSet) {
    adapter_.manager().Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("Storage running low"),
                                         NotificationAction::ChangeFolder));

    ASSERT_EQ(adapter_.toastModel()->rowCount(), 1);
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "title").toString(), QStringLiteral("Storage running low"));
    // LowStorage reports a condition that still holds: standing, so no
    // countdown hairline and no auto-dismiss.
    EXPECT_TRUE(RoleAt(adapter_.toastModel(), 0, "standing").toBool());
    // "error", not "caution": crossing the hard-stop threshold ENDS the
    // recording, which is the severity rung AdvisoryStatusForType reserves for
    // something that failed or was lost.
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "tone").toString(), QStringLiteral("error"));
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "actionCount").toInt(), 1);
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "primaryAction").toInt(),
              static_cast<int>(NotificationAction::ChangeFolder));
}

TEST_F(ToastStackTest, ATimedToastCountsDownAndAStandingOneDoesNot) {
    adapter_.manager().Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("Recording saved"),
                                         NotificationAction::Edit, NotificationAction::OpenFolder));

    ASSERT_EQ(adapter_.toastModel()->rowCount(), 1);
    EXPECT_FALSE(RoleAt(adapter_.toastModel(), 0, "standing").toBool());
    // Just shown, so effectively the full dwell is still ahead.
    EXPECT_GT(RoleAt(adapter_.toastModel(), 0, "remainingFraction").toDouble(), 0.9);
    // Two actions get their own named buttons; one would make the card itself
    // the target.
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "actionCount").toInt(), 2);
}

TEST_F(ToastStackTest, ToastsDisabledLeavesTheStackEmptyButStillRecordsTheHub) {
    adapter_.manager().SetToastsEnabled(false);
    adapter_.manager().Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("Recording saved")));

    EXPECT_EQ(adapter_.toastModel()->rowCount(), 0) << "\"Show notifications\" gates the glance";
    EXPECT_TRUE(adapter_.hasEntries()) << "the hub is the record and is fed unconditionally";
}

// ── "Show notifications": the setting that used to suppress nothing ──────────────
//
// The switch was surfaced, persisted and exported to automation, and
// NotificationManager::SetToastsEnabled had no non-test caller at all, so turning it
// off changed nothing whatsoever. The manager's own suppression rules were never the
// gap; the path from the setting to the manager was. These cases pin that path at the
// object the composition root applies the persisted value to.

TEST_F(ToastStackTest, TheShowNotificationsSettingSuppressesTheGlanceAndKeepsTheRecord) {
    adapter_.applyShowNotifications(false);

    adapter_.manager().Enqueue(
        MakeEvent(NotificationType::Saved, QStringLiteral("Recording saved"), NotificationAction::Edit));
    // A standing card too: the setting is about whether anything appears on screen,
    // so it is not a timed-only gate.
    adapter_.manager().Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("Storage running low"),
                                         NotificationAction::ChangeFolder));

    EXPECT_EQ(adapter_.toastModel()->rowCount(), 0) << "the setting gates the toast glance";
    EXPECT_EQ(adapter_.model()->rowCount(), 2) << "the hub is the record and keeps every event";
}

TEST_F(ToastStackTest, ApplyingShowNotificationsOffClearsWhatIsAlreadyOnScreen) {
    adapter_.manager().Enqueue(MakeEvent(NotificationType::LowStorage, QStringLiteral("Storage running low"),
                                         NotificationAction::ChangeFolder));
    ASSERT_EQ(adapter_.toastModel()->rowCount(), 1);

    // Turning the setting off mid-session has to take effect now, not from the next
    // event on — a standing card would otherwise sit there forever.
    adapter_.applyShowNotifications(false);

    EXPECT_EQ(adapter_.toastModel()->rowCount(), 0);
    EXPECT_EQ(adapter_.model()->rowCount(), 1) << "clearing the glance never unrecords it";
}

TEST_F(ToastStackTest, TurningShowNotificationsBackOnLetsTheNextToastThrough) {
    adapter_.applyShowNotifications(false);
    adapter_.manager().Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("Suppressed")));
    ASSERT_EQ(adapter_.toastModel()->rowCount(), 0);

    adapter_.applyShowNotifications(true);
    adapter_.manager().Enqueue(MakeEvent(NotificationType::Saved, QStringLiteral("Recording saved")));

    ASSERT_EQ(adapter_.toastModel()->rowCount(), 1);
    EXPECT_EQ(RoleAt(adapter_.toastModel(), 0, "title").toString(), QStringLiteral("Recording saved"));
}

TEST_F(ToastStackTest, ActionIsAddressedBySequenceAndRetiresTheToast) {
    const quint64 sequence = adapter_.manager().Enqueue(MakeEvent(
        NotificationType::LowStorage, QStringLiteral("Storage running low"), NotificationAction::ChangeFolder));

    QSignalSpy spy(&adapter_, &NotificationsAdapter::actionTriggered);
    adapter_.triggerToastAction(static_cast<qint64>(sequence), static_cast<int>(NotificationAction::ChangeFolder));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(1).toString(), QStringLiteral("C:/out/clip.mkv"));
    EXPECT_EQ(adapter_.toastModel()->rowCount(), 0) << "acting on a toast retires it";
}

TEST_F(ToastStackTest, AnActionTheEventDoesNotOfferIsIgnored) {
    const quint64 sequence = adapter_.manager().Enqueue(MakeEvent(
        NotificationType::LowStorage, QStringLiteral("Storage running low"), NotificationAction::ChangeFolder));

    QSignalSpy spy(&adapter_, &NotificationsAdapter::actionTriggered);
    // A stale delegate must not be able to replay an action this card never had.
    adapter_.triggerToastAction(static_cast<qint64>(sequence), static_cast<int>(NotificationAction::RelaunchElevated));

    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(adapter_.toastModel()->rowCount(), 1);
}

TEST_F(ToastStackTest, AnActionOnARetiredSequenceIsIgnored) {
    const quint64 sequence = adapter_.manager().Enqueue(MakeEvent(
        NotificationType::LowStorage, QStringLiteral("Storage running low"), NotificationAction::ChangeFolder));
    adapter_.dismissToast(static_cast<qint64>(sequence));
    ASSERT_EQ(adapter_.toastModel()->rowCount(), 0);

    QSignalSpy spy(&adapter_, &NotificationsAdapter::actionTriggered);
    adapter_.triggerToastAction(static_cast<qint64>(sequence), static_cast<int>(NotificationAction::ChangeFolder));

    EXPECT_EQ(spy.count(), 0);
}

TEST_F(ToastStackTest, AnchorGeometryIsPublishedForTheHostingScreen) {
    QSignalSpy spy(&adapter_, &NotificationsAdapter::toastAnchorChanged);
    const QRect work_area(0, 0, 2560, 1400);

    adapter_.setToastAnchorGeometry(work_area);

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(adapter_.toastAnchorGeometry(), work_area);
    // Idempotent: dragging inside one screen must not churn the binding.
    adapter_.setToastAnchorGeometry(work_area);
    EXPECT_EQ(spy.count(), 1);
}

// ─── The low-storage producer ────────────────────────────────────────────────

TEST(LowStorageProducer, DiskGuardAutoStopIsRecognisedFromTheResult) {
    UiRecordingResult result;
    result.succeeded = false;
    result.error_phase = models::kDiskSpaceErrorPhase;

    // The one thing the frontend has to get right: recognise the guard's own
    // stop. There is deliberately no second disk poller and no second threshold
    // — the coordinator's guard is what actually measures and acts.
    EXPECT_TRUE(models::IsDiskSpaceAutoStop(result));
    // And it must not ALSO raise the modal failure surface.
    EXPECT_FALSE(models::BuildRecordingFailureReport(result).has_value());
}

TEST(LowStorageProducer, AnOrdinaryFailureIsNotADiskStop) {
    UiRecordingResult result;
    result.succeeded = false;
    result.error_phase = L"Encode";

    EXPECT_FALSE(models::IsDiskSpaceAutoStop(result));
    EXPECT_TRUE(models::BuildRecordingFailureReport(result).has_value());
}

} // namespace
} // namespace exosnap::quick
