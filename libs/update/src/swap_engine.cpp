// swap_engine.cpp -- staged directory swap for the ExoSnap self-update flow.
//
// Renames go through MoveFileExW with dwFlags = 0 (NO MOVEFILE_COPY_ALLOWED):
// a same-volume directory move is a single metadata rename and is what makes
// the swap atomic and instantly reversible. Allowing a copy fallback would
// turn a failed cross-volume move into a half-copied directory -- exactly the
// data-loss window this engine exists to avoid. The staging tree is therefore
// required to live on the same volume as the install dir (the updater extracts
// it into "<install>.new" for precisely this reason).
//
// Recursive deletes and existence probes use std::filesystem (standard C++,
// Qt-free); only the rename/version/wait primitives are WinAPI.

#include <update/swap_engine.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace exosnap::update {

namespace {

constexpr const wchar_t* kExeName = L"exosnap.exe";

// MoveFileExW(existing, target, 0): same-volume atomic rename. No copy fallback.
[[nodiscard]] bool RenameDir(const std::wstring& from, const std::wstring& to) noexcept {
    return ::MoveFileExW(from.c_str(), to.c_str(), 0) != 0;
}

// Best-effort recursive delete. True if the path is gone afterwards.
[[nodiscard]] bool RemoveTree(const std::wstring& dir) noexcept {
    std::error_code ec;
    if (!fs::exists(fs::path(dir), ec))
        return true;
    fs::remove_all(fs::path(dir), ec);
    // Re-probe: remove_all can report an error yet still have cleared the tree,
    // or vice versa. The post-condition (path absent) is what matters.
    std::error_code ec2;
    return !fs::exists(fs::path(dir), ec2);
}

[[nodiscard]] bool FileExists(const std::wstring& path) noexcept {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

} // namespace

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

SwapPlan MakeSwapPlan(const std::wstring& install_dir, SemVer target) {
    SwapPlan plan;
    plan.install_dir = install_dir;
    plan.staging_dir = install_dir + L".new";
    plan.backup_dir = install_dir + L".old";
    plan.target_version = target;
    return plan;
}

// ---------------------------------------------------------------------------
// Staged swap
// ---------------------------------------------------------------------------

SwapError StageRename(const SwapPlan& plan) {
    // 1. Staging must exist and carry an exosnap.exe, otherwise there is
    //    nothing valid to promote -- reject with the install untouched (B2).
    if (!FileExists(plan.staging_dir) || !FileExists((fs::path(plan.staging_dir) / kExeName).wstring())) {
        return SwapError::StagingMissing;
    }

    // 2. A stale backup from an aborted earlier swap must be cleared first, or
    //    the install->backup rename would collide. Failure here is still B2:
    //    nothing has been moved yet.
    if (FileExists(plan.backup_dir) && !RemoveTree(plan.backup_dir)) {
        return SwapError::BackupCollision;
    }

    // 3. install -> backup. The old version now lives at backup_dir; the
    //    install path is free. Failure leaves the old install in place (B2).
    if (!RenameDir(plan.install_dir, plan.backup_dir)) {
        return SwapError::RenameOldFailed;
    }

    // 4. staging -> install. On success the new version is live.
    if (!RenameDir(plan.staging_dir, plan.install_dir)) {
        // Compensate: put the old version back where it belongs. The install
        // path is free again (step 3 emptied it), so this rename should
        // succeed; if it does the old version is live (B3). If even this
        // fails we are in the worst case and must report red with paths.
        if (!RenameDir(plan.backup_dir, plan.install_dir)) {
            return SwapError::RestoreFailed;
        }
        return SwapError::RenameNewFailed;
    }

    return SwapError::None;
}

SwapError RestoreBackup(const SwapPlan& plan) {
    // Undo a completed (or verify-failed) swap: backup -> install. The current
    // install tree (the new, unwanted version) must be cleared first because a
    // directory rename cannot overwrite an existing directory.
    if (!FileExists(plan.backup_dir)) {
        return SwapError::RestoreFailed;
    }
    if (FileExists(plan.install_dir) && !RemoveTree(plan.install_dir)) {
        return SwapError::RestoreFailed;
    }
    if (!RenameDir(plan.backup_dir, plan.install_dir)) {
        return SwapError::RestoreFailed;
    }
    return SwapError::None;
}

bool CleanupBackup(const SwapPlan& plan) {
    return RemoveTree(plan.backup_dir);
}

// ---------------------------------------------------------------------------
// Version verification
// ---------------------------------------------------------------------------

std::optional<SemVer> ReadFileVersion(const std::wstring& exe_path) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exe_path.c_str(), &ignored);
    if (size == 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(size);
    if (::GetFileVersionInfoW(exe_path.c_str(), 0, size, buffer.data()) == 0) {
        return std::nullopt;
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_len = 0;
    if (::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_len) == 0 || info == nullptr ||
        info_len < sizeof(VS_FIXEDFILEINFO)) {
        return std::nullopt;
    }

    SemVer v;
    v.major = HIWORD(info->dwFileVersionMS);
    v.minor = LOWORD(info->dwFileVersionMS);
    v.patch = HIWORD(info->dwFileVersionLS);
    // LOWORD(dwFileVersionLS) is the build number -- intentionally ignored.
    return v;
}

bool VerifyInstalledVersion(const SwapPlan& plan) {
    const std::wstring exe = (fs::path(plan.install_dir) / kExeName).wstring();
    if (!FileExists(exe)) {
        return false;
    }
    const std::optional<SemVer> found = ReadFileVersion(exe);
    return found.has_value() && *found == plan.target_version;
}

// ---------------------------------------------------------------------------
// Process / mutex waits
// ---------------------------------------------------------------------------

bool WaitForProcessExit(uint32_t pid, std::chrono::milliseconds timeout) {
    HANDLE proc = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (proc == nullptr) {
        // No handle: the pid is invalid/gone (ERROR_INVALID_PARAMETER) or the
        // object is inaccessible (ERROR_ACCESS_DENIED). In every case we cannot
        // -- and need not -- wait on it: it is not an exosnap instance holding
        // our files. Treat as already exited.
        return true;
    }
    const DWORD ms = static_cast<DWORD>(timeout.count());
    const DWORD r = ::WaitForSingleObject(proc, ms);
    ::CloseHandle(proc);
    return r == WAIT_OBJECT_0;
}

bool WaitForInstanceMutex(const wchar_t* mutex_name, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        HANDLE m = ::OpenMutexW(SYNCHRONIZE, FALSE, mutex_name);
        if (m == nullptr) {
            // The named object no longer exists -> no instance holds it.
            return true;
        }
        ::CloseHandle(m);
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // namespace exosnap::update
