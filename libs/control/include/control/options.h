#pragma once

// control/options.h -- the one gate that turns a control channel on.
//
// A normal launch has no endpoint. Not "an endpoint nobody connects to" -- no
// pipe is created, no thread is started, nothing is logged. The only way in is
// an explicit argv option carrying a run id:
//
//     exosnap.exe          --live-verify-control <run-id>
//     exosnap-updater.exe  --automation-control  <run-id>
//
// Deliberately NOT triggered by: a Debug configuration, an environment
// variable, the presence of a developer tool, or a settings key. Every one of
// those can be true on a user's machine without the user asking for it, and
// these channels drive real recordings and real installations.
//
// The run id doubles as the connection credential and as the pipe-name suffix,
// so it must be unguessable (the runner mints a GUID) and must survive being
// pasted into a pipe path -- hence the character allowlist below.

#include <QString>
#include <QStringList>

namespace exosnap::control {

struct ControlOptions {
    bool requested = false;
    QString run_id;
    // Set when the option was present but its value was missing or malformed.
    // The entry point must refuse to start rather than silently fall back to a
    // normal launch: a runner that believes it armed the channel and got an
    // ordinary process instead would report the wrong thing.
    QString error;
};

// True for a run id of 8..64 characters drawn from [A-Za-z0-9._-].
[[nodiscard]] bool IsValidRunId(const QString& run_id);

// Finds `option` in `arguments` and validates the run id that follows it.
[[nodiscard]] ControlOptions ParseControlOptions(const QStringList& arguments, const QString& option);

// "\\\\.\\pipe\\ExoSnap.<role>.<run-id>". Pure so the runner-side name and the
// server-side name are provably the same string.
//
// The ROLE is what lets one runner hold both endpoints of the same run id at
// once -- it drives the application, the application launches the updater with
// the same id, and the runner attaches to the updater without a second
// handshake vocabulary. Existing roles: "LiveVerify" (the application),
// "Updater" (exosnap-updater.exe).
[[nodiscard]] QString PipeName(const QString& role, const QString& run_id);

} // namespace exosnap::control
