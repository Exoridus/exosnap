#pragma once

// WindowIdentity.h -- semantic identity for ExoSnap's native top-level windows.
//
// The invariant this file exists to enforce:
//
//   No automation or process-control decision may identify a window solely by
//   its title.
//
// A window title is OS-visible identity meant for humans and for the taskbar. It
// is translated (`qsTr`), it is product copy, and Wave B found several top-level
// windows sharing the string "ExoSnap" because Qt gives every QWindow the
// application's title by default. Primary automation identity is therefore
// `role` + process identity; the title is reported alongside so a human report
// and a machine decision can be reconciled, never so the machine can match on it.
//
// The role is derived from the QML objectName, which is internal and stable, and
// the derivation lives HERE rather than at each call site -- `scripts/lib/
// LiveVerifyChecks.ps1` currently hardcodes two objectNames, which is exactly the
// coupling a role replaces.

#include <QJsonObject>
#include <QString>

#include <vector>

namespace exosnap::observability {

// The product roles a native top-level window can have. Closed set: a window
// whose objectName matches nothing here is reported with role `unknown` rather
// than silently omitted -- an unnamed top-level window is a finding, not a gap.
namespace window_role {
inline constexpr const char* kMain = "main";
inline constexpr const char* kRecordingOverlay = "recordingOverlay";
inline constexpr const char* kDiagnosticsOverlay = "diagnosticsOverlay";
inline constexpr const char* kQuickControls = "quickControls";
inline constexpr const char* kNotificationToast = "notificationToast";
inline constexpr const char* kCountdown = "countdown";
inline constexpr const char* kUnknown = "unknown";
} // namespace window_role

// Pure: the semantic role for a QML objectName. `is_root` distinguishes the
// application's own root window, which carries no overlay objectName.
[[nodiscard]] QString WindowRoleForObjectName(const QString& object_name, bool is_root);

struct WindowFacts {
    QString role;
    QString object_name;
    QString title;
    bool visible = false;
    bool exposed = false;
    // A hidden window has no platform window, and asking for its handle would
    // CREATE one. False here means "no native window exists", not "unknown".
    bool native_window_created = false;
    QString screen;
    // Present only when a native window exists.
    QJsonObject native;
};

// The `windows.snapshot` payload: every native top-level window this process
// owns, keyed by role, plus the process identity that makes the role addressable
// across a handoff.
[[nodiscard]] QJsonObject WindowSnapshotToJson(const std::vector<WindowFacts>& windows, qint64 process_id);

} // namespace exosnap::observability
