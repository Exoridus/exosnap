#pragma once

// LiveVerifyOptions.h -- the one gate that turns the application's control
// channel on.
//
//     exosnap.exe --live-verify-control <run-id>
//
// The gate mechanics (run-id validation, the endpoint name) are shared with the
// updater's automation endpoint and live in libs/control (control/options.h);
// what belongs here is the option this executable answers to and the ROLE its
// endpoint carries in the pipe name. The role is what lets one runner hold the
// application's endpoint and the updater's endpoint of the same run at once.
//
// Deliberately NOT triggered by: a Debug configuration, an environment
// variable, the presence of a developer tool, or a settings key. Every one of
// those can be true on a user's machine without the user asking for it, and the
// control channel drives real recordings.

#include <control/options.h>

#include <QString>
#include <QStringList>

namespace exosnap::live_verify {

inline constexpr const char* kControlOption = "--live-verify-control";
// The endpoint's role, and therefore part of its name. Unchanged from before
// the lift: the endpoint is still "\\\\.\\pipe\\ExoSnap.LiveVerify.<run-id>",
// so every runner and every script that already knows the name keeps working.
inline constexpr const char* kControlRole = "LiveVerify";

using exosnap::control::ControlOptions;
using exosnap::control::IsValidRunId;

[[nodiscard]] ControlOptions ParseControlOptions(const QStringList& arguments);

// "\\\\.\\pipe\\ExoSnap.LiveVerify.<run-id>". Pure so the runner-side name and
// the server-side name are provably the same string.
[[nodiscard]] QString PipeNameForRunId(const QString& run_id);

} // namespace exosnap::live_verify
