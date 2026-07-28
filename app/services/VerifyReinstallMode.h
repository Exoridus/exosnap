#pragma once

// VerifyReinstallMode.h -- the verification-reinstall CLI opt-in (ADR 0055).
//
// A release candidate that ships a wrong version identity cannot be told apart
// from a correct one by looking at the update card: the check only ever offers
// something NEWER, so the production update path (fetch -> signature -> hash ->
// swap -> relaunch) is never exercised against the build under test. This flag
// unlocks exactly one extra offer -- the byte-identical version -- for the
// lifetime of one app run.
//
// Deliberately NOT persisted anywhere: it is read from argv, held in memory and
// gone after a restart. There is no settings key, no environment variable and no
// UI switch, so a user cannot end up stuck in it.
//
// Pure parse seam (mirrors ElevatedRelaunch): no Win32, no I/O.

#include <QStringList>

namespace exosnap::services {

// Command-line flag spelling. Exposed for tests and for the launcher wiring.
inline constexpr const char* kVerifyUpdateReinstallFlag = "--verify-update-reinstall";

// Pure: true when `args` (typically QCoreApplication::arguments(), argv[0]
// included) carries the verification-reinstall flag. Exact match only -- no
// prefix or "=value" form, so a longer unrelated flag can never enable it.
[[nodiscard]] bool HasVerifyUpdateReinstallRequest(const QStringList& args);

} // namespace exosnap::services
