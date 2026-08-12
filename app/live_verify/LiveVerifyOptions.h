#pragma once

// LiveVerifyOptions.h -- the one gate that turns the control channel on.
//
// A normal launch has no endpoint. Not "an endpoint nobody connects to" -- no
// pipe is created, no thread is started, nothing is logged. The only way in is
// an explicit argv option carrying a run id:
//
//     exosnap.exe --live-verify-control <run-id>
//
// Deliberately NOT triggered by: a Debug configuration, an environment
// variable, the presence of a developer tool, or a settings key. Every one of
// those can be true on a user's machine without the user asking for it, and the
// control channel drives real recordings.
//
// The run id doubles as the connection credential and as the pipe-name suffix,
// so it must be unguessable (the runner mints a GUID) and must survive being
// pasted into a pipe path -- hence the character allowlist below.

#include <QString>
#include <QStringList>

namespace exosnap::live_verify {

inline constexpr const char* kControlOption = "--live-verify-control";

struct ControlOptions {
    bool requested = false;
    QString run_id;
    // Set when --live-verify-control was present but its value was missing or
    // malformed. The entry point must refuse to start rather than silently fall
    // back to a normal launch: a runner that believes it armed the channel and
    // got a normal application instead would report the wrong thing.
    QString error;
};

// True for a run id of 8..64 characters drawn from [A-Za-z0-9._-].
[[nodiscard]] bool IsValidRunId(const QString& run_id);

[[nodiscard]] ControlOptions ParseControlOptions(const QStringList& arguments);

// "\\\\.\\pipe\\ExoSnap.LiveVerify.<run-id>". Pure so the runner-side name and
// the server-side name are provably the same string.
[[nodiscard]] QString PipeNameForRunId(const QString& run_id);

} // namespace exosnap::live_verify
