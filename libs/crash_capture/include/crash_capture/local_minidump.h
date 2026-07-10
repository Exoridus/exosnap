#pragma once

// local_minidump.h — In-process minidump fallback for builds without Crashpad.
//
// Why this exists:
//   Crashpad only runs when sentry-native is fetched and linked
//   (EXOSNAP_ENABLE_CRASH_CAPTURE=ON, which is OFF by default). In every other
//   build — including every local developer build — nothing wrote a dump when
//   the process died. Only the clean_exit sidecar recorded *that* a crash
//   happened, never *where*. That made teardown crashes undiagnosable.
//
//   This installs a last-chance SetUnhandledExceptionFilter that writes a
//   minidump next to the other crash artifacts. It is deliberately modest:
//   an out-of-process handler (Crashpad) survives cases an in-process one
//   cannot — heap corruption, stack exhaustion, or a crash inside the writer
//   itself. For an access violation on a dangling pointer, which is what the
//   recording teardown produces, an in-process dump is sufficient and needs no
//   elevation, no registry keys, and no network.
//
// Limitations, stated honestly:
//   - Not installed when Crashpad is active; Crashpad owns the filter.
//   - std::terminate / abort() bypass the unhandled-exception filter.
//   - A sufficiently corrupted process may fail to write the dump.

#include <string>

namespace exosnap::crash_capture {

// True when the in-process fallback should be installed: only when no
// out-of-process handler (Crashpad) is capturing crashes. Pure; the caller
// passes what it knows about the build.
[[nodiscard]] bool ShouldInstallLocalMinidumpHandler(bool crashpad_active) noexcept;

// Build the dump file name for a crash at the given wall-clock time.
// Format: "exosnap-<YYYYMMDD>-<HHMMSS>-<pid>.dmp" — sorts chronologically and
// stays unique across concurrent processes. Pure, so it is unit-pinned.
[[nodiscard]] std::string MakeMinidumpFileName(int year, int month, int day, int hour, int minute, int second,
                                               unsigned long pid);

// Install the last-chance exception filter. Dumps are written into crash_dir,
// which must already exist. Returns false when crash_dir is empty or too long
// to hold a dump path. Idempotent: a second call replaces the stored directory
// and keeps the single installed filter.
//
// Safe to call once at startup, before any worker threads exist.
bool InstallLocalMinidumpHandler(const std::string& crash_dir);

} // namespace exosnap::crash_capture
