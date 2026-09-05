#pragma once
// NotificationNames.h -- stable text spellings for the notification enums.
//
// The automation channel addresses types and actions by name, and a report reads
// them back the same way. Both directions live here so a name can never mean one
// thing on the way in and another on the way out.
//
// The spellings are lowerCamelCase of the enumerator, which is the convention
// every other automation surface in this product already uses.

#include <QString>

#include "notifications/NotificationEvent.h"

namespace exosnap::notifications {

// Empty for a value outside the enum, which is unreachable through the public
// API and therefore not worth a fallback spelling that could be mistaken for one.
[[nodiscard]] QString NotificationTypeName(NotificationType type);
[[nodiscard]] QString NotificationActionName(NotificationAction action);

// Parse the spellings above. Return false (leaving *out untouched) for anything
// they do not name, so a caller reports an unknown name rather than silently
// raising the first enumerator.
[[nodiscard]] bool ParseNotificationType(const QString& name, NotificationType* out);
[[nodiscard]] bool ParseNotificationAction(const QString& name, NotificationAction* out);

} // namespace exosnap::notifications
