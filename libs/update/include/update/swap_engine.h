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
// The swap itself.
// ---------------------------------------------------------------------------

// rename install->backup, staging->install. If the second rename fails the
// first is undone before returning (RenameNewFailed => old version is live
// again; RestoreFailed => the compensating rename also failed).
[[nodiscard]] SwapError StageRename(const SwapPlan& plan);

// VERSIONINFO FileVersion (major.minor.patch) of an exe; nullopt when unreadable.
[[nodiscard]] std::optional<SemVer> ReadFileVersion(const std::wstring& exe_path);

// <install>\exosnap.exe exists and its version equals plan.target_version.
[[nodiscard]] bool VerifyInstalledVersion(const SwapPlan& plan);

// backup->install (undo of a completed swap, or recovery after verify failed).
[[nodiscard]] SwapError RestoreBackup(const SwapPlan& plan);

// Best-effort recursive delete of the backup directory. True on success (or
// if it was already gone).
[[nodiscard]] bool CleanupBackup(const SwapPlan& plan);

} // namespace exosnap::update
