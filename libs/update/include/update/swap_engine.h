#pragma once
// swap_engine.h -- staged directory swap for the ExoSnap self-update flow.
//
// No Qt. The only platform dependency is <windows.h> in the .cpp (renames via
// MoveFileExW, version info via the Version API, waits via the process/mutex
// primitives). This header exposes pure value types plus free functions so the
// swap logic can be reasoned about (and, where side-effect-free, tested) in
// isolation from the UI strand.
//
// Failure model (data-loss-critical): a failed swap must always leave a
// runnable install behind. StageRename renames install -> backup, then
// staging -> install. If the second rename fails, the first is undone
// (backup -> install) before returning so the OLD version is live again.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <update/update_types.h>
#include <vector>

namespace exosnap::update {

// ---------------------------------------------------------------------------
// Swap plan -- the three sibling directories involved in a staged swap.
// ---------------------------------------------------------------------------

struct SwapPlan {
    std::wstring install_dir; // live dir containing exosnap.exe
    std::wstring staging_dir; // "<install_dir>.new" -- extracted new version root
    std::wstring backup_dir;  // "<install_dir>.old"
    SemVer target_version;
};

// Derives the ".new" / ".old" sibling directories from install_dir.
[[nodiscard]] SwapPlan MakeSwapPlan(const std::wstring& install_dir, SemVer target);

// ---------------------------------------------------------------------------
// Swap errors. Each records how far the swap progressed so the caller knows
// which install tree is live: "nothing touched" means the old install is
// untouched and running; "old install restored and live again" means the
// second rename failed but the compensating rename put the old version back;
// "worst case" means even the restore failed and the old tree is stranded in
// the backup dir.
// ---------------------------------------------------------------------------

enum class SwapError : uint8_t {
    None = 0,
    StagingMissing,  // staging_dir absent or has no exosnap.exe -> nothing touched, old install intact
    BackupCollision, // backup_dir already exists and can't be cleared -> nothing touched, old install intact
    RenameOldFailed, // install->backup failed -> nothing touched, old install intact
    RenameNewFailed, // staging->install failed -> old install restored and live again
    RestoreFailed,   // restore itself failed -> worst case: old tree stranded in backup dir, report red + paths
};

// ---------------------------------------------------------------------------
// Process / mutex waits -- used to make sure the old instance is gone before
// the swap and to detect a re-launched instance holding the single-instance
// mutex.
// ---------------------------------------------------------------------------

// True once the process is gone (or was never running). Access failures under
// SYNCHRONIZE do NOT count as "gone": the wait falls back to lower-privilege
// probes so an elevated old app cannot fool a non-elevated updater into
// starting the swap while its exe image is still locked. False if the process
// is still alive when the timeout elapses.
[[nodiscard]] bool WaitForProcessExit(uint32_t pid, std::chrono::milliseconds timeout);

// True once the named single-instance mutex exists (the new app has come up),
// polled every 250 ms. A mutex openable only in another security context
// (ERROR_ACCESS_DENIED) still counts as present. False if it never appears
// before the timeout elapses.
[[nodiscard]] bool WaitForInstanceMutex(const wchar_t* mutex_name, std::chrono::milliseconds timeout);

// ---------------------------------------------------------------------------
// Window discovery -- the close/handoff message must reach exactly the app
// process this updater was launched for (--app-pid), AND specifically its
// main window, never merely "a" top-level window that process happens to
// own. Two distinct ways this goes wrong if only one of {pid, title} is
// checked:
//   - Title only: a second already-running instance (a developer's own
//     separate build, say) can carry the same title; the message then hands
//     off to the wrong instance while the real target waits out its close
//     timeout untouched.
//   - PID only: ExoSnap's own process owns MORE than one top-level window
//     when a system tray icon is active (Qt creates a hidden native window
//     for tray callbacks) -- EnumWindows can return that hidden window
//     before the real main window, and posting to it is silently swallowed
//     (MainWindow::nativeEvent only reacts to its own HWND), so the app
//     never closes and the updater eventually times out.
// Matching therefore requires BOTH: owner PID and exact window title.
// ---------------------------------------------------------------------------

struct TopLevelWindow {
    void* handle; // native window handle (HWND)
    uint32_t owner_pid;
    std::wstring title; // exact window text (empty for untitled/hidden windows)
};

// Pure: the first candidate owned by target_pid with title == target_title,
// or nullptr if none matches. Candidates come from the caller -- a real
// enumeration walk in production, a fabricated list in tests -- so the
// matching rule can be proven without depending on real desktop window state.
[[nodiscard]] void* SelectWindowForProcess(const std::vector<TopLevelWindow>& candidates, uint32_t target_pid,
                                           const std::wstring& target_title);

// Real Win32 enumeration + selection: walks all top-level windows and returns
// the one owned by target_pid whose title is exactly target_title, or nullptr
// if none is currently open.
[[nodiscard]] void* FindTopLevelWindowForProcess(uint32_t target_pid, const std::wstring& target_title);

// ---------------------------------------------------------------------------
// The swap itself.
// ---------------------------------------------------------------------------

// rename install->backup, staging->install. If the second rename fails the
// first is undone before returning (RenameNewFailed => old version is live
// again; RestoreFailed => the compensating rename also failed).
[[nodiscard]] SwapError StageRename(const SwapPlan& plan);

// VERSIONINFO FileVersion (major.minor.patch) of an exe; nullopt when unreadable.
// The numeric FIXEDFILEINFO can never carry a prerelease suffix, so this alone
// cannot distinguish 0.9.0-rc4 from 0.9.0.
[[nodiscard]] std::optional<SemVer> ReadFileVersion(const std::wstring& exe_path);

// VERSIONINFO ProductVersion string (e.g. "0.9.0-rc4") of an exe; nullopt when
// the StringFileInfo block or the value is missing. This is the full release
// identity embedded by the build (exosnap_version.rc.in).
[[nodiscard]] std::optional<std::string> ReadProductVersionString(const std::wstring& exe_path);

// Pure decision helper for VerifyInstalledVersion, exposed for tests. The full
// ProductVersion string is authoritative and mandatory for prerelease targets;
// without it an RC identity cannot be proven and verification fails closed.
// Final targets may fall back to the prerelease-blind numeric FileVersion for
// foreign or legacy binaries that carry no parseable ProductVersion string.
[[nodiscard]] bool InstalledVersionMatches(const std::optional<SemVer>& file_version,
                                           const std::optional<SemVer>& product_version, const SemVer& target);

// <install>\exosnap.exe exists and its version equals plan.target_version
// (full release identity, prerelease-aware — see InstalledVersionMatches).
[[nodiscard]] bool VerifyInstalledVersion(const SwapPlan& plan);

// backup->install (undo of a completed swap, or recovery after verify failed).
[[nodiscard]] SwapError RestoreBackup(const SwapPlan& plan);

// Best-effort recursive delete of the backup directory. True on success (or
// if it was already gone).
[[nodiscard]] bool CleanupBackup(const SwapPlan& plan);

// Heals an interrupted StageRename. install->backup (rename 1) and
// staging->install (rename 2) are two separate MoveFileExW calls; a process
// kill landing in the narrow window between them -- or between rename 2
// failing and its own compensating restore -- can leave install_dir gone
// while backup_dir still holds the last-known-good tree (SwapError::
// RestoreFailed is exactly this state reported at the time). This restores
// backup_dir -> install_dir so a runnable install exists again. A no-op
// (returns true, nothing touched) when install_dir already carries
// exosnap.exe, or when there is no backup_dir to restore from -- there is
// nothing for this function to repair in either case.
[[nodiscard]] bool RepairOrphanedSwap(const SwapPlan& plan);

} // namespace exosnap::update
