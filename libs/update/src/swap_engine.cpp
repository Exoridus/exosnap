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
    // Trim trailing separators before deriving the ".new"/".old" siblings. The
    // MSI's [INSTALLFOLDER] registry value always ends with a backslash; without
    // this, "<install>\" + ".new" would place the staging tree *inside* the
    // install dir ("<install>\.new") instead of beside it.
    std::wstring normalized = install_dir;
    while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }

    SwapPlan plan;
    plan.install_dir = normalized;
    plan.staging_dir = normalized + L".new";
    plan.backup_dir = normalized + L".old";
    plan.target_version = target;
    return plan;
}

// ---------------------------------------------------------------------------
// Staged swap
// ---------------------------------------------------------------------------

SwapError StageRename(const SwapPlan& plan) {
    // 1. Staging must exist and carry an exosnap.exe, otherwise there is
    //    nothing valid to promote -- reject with nothing touched, old install
    //    intact.
    if (!FileExists(plan.staging_dir) || !FileExists((fs::path(plan.staging_dir) / kExeName).wstring())) {
        return SwapError::StagingMissing;
    }

    // 2. A stale backup from an aborted earlier swap must be cleared first, or
    //    the install->backup rename would collide. Failure here still means
    //    nothing has been moved yet -- old install intact.
    if (FileExists(plan.backup_dir) && !RemoveTree(plan.backup_dir)) {
        return SwapError::BackupCollision;
    }

    // 3. install -> backup. The old version now lives at backup_dir; the
    //    install path is free. Failure leaves the old install in place,
    //    untouched.
    if (!RenameDir(plan.install_dir, plan.backup_dir)) {
        return SwapError::RenameOldFailed;
    }

    // 4. staging -> install. On success the new version is live.
    if (!RenameDir(plan.staging_dir, plan.install_dir)) {
        // Compensate: put the old version back where it belongs. The install
        // path is free again (step 3 emptied it), so this rename should
        // succeed; if it does the old install is restored and live again. If
        // even this fails we are in the worst case and must report red with
        // paths.
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

bool RepairOrphanedSwap(const SwapPlan& plan) {
    const std::wstring install_exe = (fs::path(plan.install_dir) / kExeName).wstring();
    if (FileExists(install_exe)) {
        return true; // install_dir looks intact -- nothing to repair
    }
    if (!FileExists(plan.backup_dir)) {
        return true; // no backup to restore from -- not an orphan this can fix
    }
    return RestoreBackup(plan) == SwapError::None;
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

std::optional<std::string> ReadProductVersionString(const std::wstring& exe_path) {
    DWORD ignored = 0;
    const DWORD size = ::GetFileVersionInfoSizeW(exe_path.c_str(), &ignored);
    if (size == 0) {
        return std::nullopt;
    }

    std::vector<uint8_t> buffer(size);
    if (::GetFileVersionInfoW(exe_path.c_str(), 0, size, buffer.data()) == 0) {
        return std::nullopt;
    }

    // Query the translation table instead of assuming a fixed language block so
    // this also reads binaries whose StringFileInfo is not 040904b0.
    struct LangCodepage {
        WORD language;
        WORD codepage;
    };
    LangCodepage* translations = nullptr;
    UINT translations_len = 0;
    if (::VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations),
                         &translations_len) == 0 ||
        translations == nullptr || translations_len < sizeof(LangCodepage)) {
        return std::nullopt;
    }

    const size_t count = translations_len / sizeof(LangCodepage);
    for (size_t i = 0; i < count; ++i) {
        wchar_t query[64];
        ::swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\ProductVersion", translations[i].language,
                     translations[i].codepage);
        wchar_t* value = nullptr;
        UINT value_len = 0;
        if (::VerQueryValueW(buffer.data(), query, reinterpret_cast<LPVOID*>(&value), &value_len) == 0 ||
            value == nullptr || value_len == 0) {
            continue;
        }
        // value_len counts wchar_t units including the terminator.
        std::wstring wide(value, value + value_len);
        while (!wide.empty() && wide.back() == L'\0') {
            wide.pop_back();
        }
        if (wide.empty()) {
            continue;
        }
        std::string narrow;
        narrow.reserve(wide.size());
        bool ascii = true;
        for (wchar_t wc : wide) {
            if (wc > 0x7f) {
                ascii = false;
                break;
            }
            narrow.push_back(static_cast<char>(wc));
        }
        if (ascii) {
            return narrow;
        }
    }
    return std::nullopt;
}

bool InstalledVersionMatches(const std::optional<SemVer>& file_version, const std::optional<SemVer>& product_version,
                             const SemVer& target) {
    // The ProductVersion string is the full release identity (prerelease-aware)
    // and is authoritative whenever present: it is the only signal that can
    // tell 0.9.0-rc4 apart from 0.9.0.
    if (product_version.has_value()) {
        return *product_version == target;
    }

    // A prerelease target can only be proven by the full ProductVersion string.
    // Numeric VERSIONINFO fields collapse 0.9.0-rc4 and 0.9.0 onto the same
    // 0.9.0 base, so accepting the fallback here would let a missing or malformed
    // ProductVersion silently erase the release identity this check exists to prove.
    if (target.is_prerelease) {
        return false;
    }

    // Fallback for binaries without a parseable ProductVersion string: the
    // numeric FIXEDFILEINFO is accepted only for a final target. The package
    // SHA-256 was already verified before the swap; this remains a legacy sanity
    // check, not the integrity gate.
    if (!file_version.has_value()) {
        return false;
    }
    return file_version->major == target.major && file_version->minor == target.minor &&
           file_version->patch == target.patch;
}

bool VerifyInstalledVersion(const SwapPlan& plan) {
    const std::wstring exe = (fs::path(plan.install_dir) / kExeName).wstring();
    if (!FileExists(exe)) {
        return false;
    }
    std::optional<SemVer> product;
    if (const std::optional<std::string> product_str = ReadProductVersionString(exe)) {
        product = ParseSemVer(*product_str);
    }
    return InstalledVersionMatches(ReadFileVersion(exe), product, plan.target_version);
}

// ---------------------------------------------------------------------------
// Process / mutex waits
// ---------------------------------------------------------------------------

namespace {

// Clamp a millisecond duration into the DWORD range accepted by the Win32 wait
// APIs. A negative count must not wrap into a near-INFINITE timeout, and we cap
// at MAXDWORD-1 so we never accidentally pass INFINITE (0xFFFFFFFF).
[[nodiscard]] DWORD ClampTimeoutMs(std::chrono::milliseconds timeout) noexcept {
    const auto count = timeout.count();
    if (count <= 0) {
        return 0;
    }
    constexpr long long kMax = static_cast<long long>(MAXDWORD) - 1;
    if (count > kMax) {
        return static_cast<DWORD>(kMax);
    }
    return static_cast<DWORD>(count);
}

// Poll GetExitCodeProcess on an already-open handle until the process reports a
// real exit code or the deadline passes. Returns true only on observed exit.
[[nodiscard]] bool PollExitCode(HANDLE proc, std::chrono::steady_clock::time_point deadline) noexcept {
    for (;;) {
        DWORD code = 0;
        if (::GetExitCodeProcess(proc, &code) != 0 && code != STILL_ACTIVE) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// Poll OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) until the pid can no
// longer be opened because it is gone (ERROR_INVALID_PARAMETER), or the
// deadline passes. Any other open/failure state keeps polling.
[[nodiscard]] bool PollUntilPidGone(DWORD pid, std::chrono::steady_clock::time_point deadline) noexcept {
    for (;;) {
        HANDLE probe = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (probe != nullptr) {
            ::CloseHandle(probe);
        } else if (::GetLastError() == ERROR_INVALID_PARAMETER) {
            return true; // pid no longer refers to any process
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // namespace

bool WaitForProcessExit(uint32_t pid, std::chrono::milliseconds timeout) {
    // SYNCHRONIZE is the ideal access -- it lets WaitForSingleObject block on
    // the process object directly. But an elevated old app + a non-elevated
    // updater cannot be granted SYNCHRONIZE on that process, and we must NOT
    // treat that access failure as "already exited": starting the swap while
    // the old, still-running exe image is locked on disk would fail the rename.
    // So we fall back to lower-privilege probes that survive the elevation gap.
    const DWORD dwpid = static_cast<DWORD>(pid);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    HANDLE proc = ::OpenProcess(SYNCHRONIZE, FALSE, dwpid);
    if (proc != nullptr) {
        const DWORD r = ::WaitForSingleObject(proc, ClampTimeoutMs(timeout));
        ::CloseHandle(proc);
        return r == WAIT_OBJECT_0;
    }

    const DWORD err = ::GetLastError();
    if (err == ERROR_INVALID_PARAMETER) {
        // The pid does not refer to any live process -- it is genuinely gone.
        return true;
    }
    if (err != ERROR_ACCESS_DENIED) {
        // Some other unexpected failure: be conservative and treat the process
        // as still present rather than racing the swap against a locked image.
        return false;
    }

    // Access denied under SYNCHRONIZE. PROCESS_QUERY_LIMITED_INFORMATION is
    // grantable across the elevation boundary, so retry with it.
    HANDLE query = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwpid);
    if (query != nullptr) {
        const bool exited = PollExitCode(query, deadline);
        ::CloseHandle(query);
        return exited;
    }

    if (::GetLastError() == ERROR_INVALID_PARAMETER) {
        return true; // gone between the two opens
    }

    // Even the limited-information open was denied. Fall back to polling the
    // open itself until the pid stops resolving to any process.
    return PollUntilPidGone(dwpid, deadline);
}

bool WaitForInstanceMutex(const wchar_t* mutex_name, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        HANDLE m = ::OpenMutexW(SYNCHRONIZE, FALSE, mutex_name);
        if (m != nullptr) {
            ::CloseHandle(m);
            return true; // an instance holds it -> the new app is up
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            // The mutex EXISTS but in another security context (e.g. the new
            // app runs elevated, this updater does not). Existence is what we
            // are testing for, so treat it as present -> the new app is up.
            return true;
        }
        // Only ERROR_FILE_NOT_FOUND (or any other transient failure) means the
        // mutex is not there yet -- the new instance has not come up. Keep
        // polling until it appears or the deadline passes.
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void* SelectWindowForProcess(const std::vector<TopLevelWindow>& candidates, uint32_t target_pid,
                             const std::wstring& target_title) {
    for (const TopLevelWindow& candidate : candidates) {
        if (candidate.owner_pid == target_pid && candidate.title == target_title) {
            return candidate.handle;
        }
    }
    return nullptr;
}

namespace {
struct CollectWindowsContext {
    DWORD target_pid;
    std::vector<TopLevelWindow> candidates;
};

BOOL CALLBACK CollectWindowIfOwnedByProcess(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<CollectWindowsContext*>(lparam);
    DWORD owner_pid = 0;
    ::GetWindowThreadProcessId(hwnd, &owner_pid);
    if (owner_pid != ctx->target_pid) {
        return TRUE;
    }
    // Hidden helper windows (e.g. Qt's system-tray callback window) are also
    // owned by this process and have no title -- collect every candidate so
    // SelectWindowForProcess can pick the one that also matches by title,
    // rather than grabbing whichever window EnumWindows happens to visit first.
    wchar_t title[256] = {};
    const int len = ::GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    ctx->candidates.push_back(TopLevelWindow{hwnd, owner_pid, std::wstring(title, static_cast<size_t>(len))});
    return TRUE;
}
} // namespace

void* FindTopLevelWindowForProcess(uint32_t target_pid, const std::wstring& target_title) {
    CollectWindowsContext ctx{static_cast<DWORD>(target_pid), {}};
    ::EnumWindows(CollectWindowIfOwnedByProcess, reinterpret_cast<LPARAM>(&ctx));
    return SelectWindowForProcess(ctx.candidates, target_pid, target_title);
}

} // namespace exosnap::update
