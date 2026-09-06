#pragma once

#include <QString>
#include <QStringList>

namespace exosnap::services {

// ---------------------------------------------------------------------------
// RelaunchResult
// ---------------------------------------------------------------------------
// Outcome of an attempt to relaunch the current executable elevated.
//   Launched      — UAC consent granted, a new elevated process was started.
//   UserDeclined   — the user dismissed the UAC prompt (ERROR_CANCELLED). This
//                    is a NORMAL, graceful outcome (ADR 0033): the app stays
//                    non-elevated, the feature stays disabled, no retry loop.
//   Failed         — ShellExecuteEx failed for another reason.
enum class RelaunchResult { Launched, UserDeclined, Failed };

// ---------------------------------------------------------------------------
// RelaunchHandoff
// ---------------------------------------------------------------------------
// Transient state carried across the elevated relaunch. Persisted settings are
// NOT handed off here (they are read from disk by the new instance); only the
// volatile bits that would otherwise be lost: the page the user is currently on
// and a flag to re-enable the feature whose opt-in triggered the relaunch.
struct RelaunchHandoff {
    // Nav page label to land on after relaunch (kPageDescriptors nav_label, e.g.
    // "Diagnostics" / "Settings"). Empty means "no explicit page".
    QString page_name;

    // When true, the relaunched (now elevated) instance should turn the
    // present-diagnostics opt-in on. The toggle is only persisted AFTER the
    // relaunch succeeds, so a UAC decline never leaves the opt-in stuck on.
    bool reenable_present_diag = false;
};

// Command-line flag spellings used for the handoff. Exposed for tests.
inline constexpr const char* kRelaunchPageFlag = "--relaunch-page";
inline constexpr const char* kReenablePresentDiagFlag = "--reenable-present-diag";

// Pure: build the handoff argument list. The list is empty when the handoff
// carries nothing (no page, no flag). Roundtrips with ParseRelaunchArgs.
[[nodiscard]] QStringList BuildRelaunchArgs(const RelaunchHandoff& handoff);

// Pure: parse a handoff out of an argument list (typically the relaunched
// process's own argv, minus argv[0]). Unknown args are ignored.
[[nodiscard]] RelaunchHandoff ParseRelaunchArgs(const QStringList& args);

// Win32: relaunch `exe_path` with `args` via ShellExecuteExW + the "runas" verb,
// raising the UAC consent prompt. Maps ERROR_CANCELLED to UserDeclined. Never
// call this during an active recording (the caller enforces that guard).
RelaunchResult RelaunchAsAdmin(const QString& exe_path, const QStringList& args);

// ADR 0033: withdraw the present-diagnostics opt-in after a relaunch that never
// became elevated (UAC declined, or ShellExecuteEx failed outright). The
// opt-in was written before the relaunch was attempted, on the assumption that
// the elevated successor would be the one measuring it; when no successor
// exists, leaving it on would read as an active feature that nothing is
// running. A no-op (returns true) when the opt-in is already off.
//
// `settings_file_path` empty resolves the default settings location the same
// way AppSettingsStore's own default constructor does (honouring
// EXOSNAP_CONFIG_DIR), which is the store the relaunching process itself read
// from. Returns false only when the settings store could not be saved.
bool WithdrawPresentDiagnosticsOptIn(const QString& settings_file_path = QString());

} // namespace exosnap::services
