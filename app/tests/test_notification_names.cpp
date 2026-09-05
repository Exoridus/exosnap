#include <gtest/gtest.h>

#include <QSet>
#include <QString>

#include "notifications/NotificationNames.h"

using exosnap::notifications::NotificationAction;
using exosnap::notifications::NotificationActionName;
using exosnap::notifications::NotificationType;
using exosnap::notifications::NotificationTypeName;
using exosnap::notifications::ParseNotificationAction;
using exosnap::notifications::ParseNotificationType;

namespace {

// The last enumerator of each enum. Adding a value after these is the one edit
// this file cannot see, so both enums are also probed one past the end below:
// a new enumerator that nobody named makes CompleteAndUnique fail, and a new
// enumerator that WAS named makes the past-the-end probe fail and points here.
constexpr auto kLastType = NotificationType::WindowCaptureStalled;
constexpr auto kLastAction = NotificationAction::SendReport;

} // namespace

TEST(NotificationTypeNames, CompleteAndUnique) {
    QSet<QString> seen;
    for (int i = 0; i <= static_cast<int>(kLastType); ++i) {
        const auto type = static_cast<NotificationType>(i);
        const QString name = NotificationTypeName(type);
        EXPECT_FALSE(name.isEmpty()) << "NotificationType " << i << " has no spelling";
        EXPECT_FALSE(seen.contains(name)) << "duplicate spelling: " << name.toStdString();
        seen.insert(name);

        NotificationType parsed{};
        ASSERT_TRUE(ParseNotificationType(name, &parsed)) << name.toStdString() << " does not parse back";
        EXPECT_EQ(parsed, type);
    }
}

TEST(NotificationActionNames, CompleteAndUnique) {
    QSet<QString> seen;
    for (int i = 0; i <= static_cast<int>(kLastAction); ++i) {
        const auto action = static_cast<NotificationAction>(i);
        const QString name = NotificationActionName(action);
        EXPECT_FALSE(name.isEmpty()) << "NotificationAction " << i << " has no spelling";
        EXPECT_FALSE(seen.contains(name)) << "duplicate spelling: " << name.toStdString();
        seen.insert(name);

        NotificationAction parsed{};
        ASSERT_TRUE(ParseNotificationAction(name, &parsed)) << name.toStdString() << " does not parse back";
        EXPECT_EQ(parsed, action);
    }
}

// If either of these starts failing, an enumerator was appended and this file
// has to learn about it -- the tables above cannot cover what they cannot see.
TEST(NotificationNames, NothingIsNamedPastTheLastEnumerator) {
    EXPECT_TRUE(NotificationTypeName(static_cast<NotificationType>(static_cast<int>(kLastType) + 1)).isEmpty());
    EXPECT_TRUE(NotificationActionName(static_cast<NotificationAction>(static_cast<int>(kLastAction) + 1)).isEmpty());
}

TEST(NotificationNames, UnknownSpellingsAreRefused) {
    NotificationType type = NotificationType::Saved;
    EXPECT_FALSE(ParseNotificationType(QStringLiteral("nosuchtype"), &type));
    // Refused, not defaulted: the caller's value is left exactly as it was.
    EXPECT_EQ(type, NotificationType::Saved);

    NotificationAction action = NotificationAction::Edit;
    EXPECT_FALSE(ParseNotificationAction(QStringLiteral(""), &action));
    EXPECT_EQ(action, NotificationAction::Edit);
}

TEST(NotificationNames, ParsingIgnoresCase) {
    NotificationType type{};
    ASSERT_TRUE(ParseNotificationType(QStringLiteral("UNEXPECTEDSTOP"), &type));
    EXPECT_EQ(type, NotificationType::UnexpectedStop);
}
