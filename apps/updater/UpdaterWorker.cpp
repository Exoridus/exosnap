// UpdaterWorker.cpp -- the background update pipeline (both install modes).
//
// Failure mapping follows the failure matrix: every early return emits exactly one
// failed(FailureCase, detail) signal; the GUI routes Retry back into run()
// with RetryEntryStep(case). Downloaded artifacts and the swap plan are kept
// as members so mid-pipeline retries (B1/B2/B3/C1) do not re-download.

#include "UpdaterWorker.h"

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <QElapsedTimer>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <variant>

#include <update/http_download.h>
#include <update/install_mode_detector.h>
#include <update/manifest_io.h>
#include <update/package_verifier.h>
#include <update/release_locator.h>
#include <update/update_checker.h>
#include <update/zip_extract.h>

namespace fs = std::filesystem;

using exosnap::update::DownloadProgress;
using exosnap::update::DownloadToFile;
using exosnap::update::ExtractZip;
using exosnap::update::FetchReleasesJson;
using exosnap::update::InstallMode;
using exosnap::update::IsDowngrade;
using exosnap::update::LocateRelease;
using exosnap::update::MakeSwapPlan;
using exosnap::update::ParseManifest;
using exosnap::update::ParseSemVer;
using exosnap::update::ReadInstallPath;
using exosnap::update::RestoreBackup;
using exosnap::update::SelectPackage;
using exosnap::update::SemVer;
using exosnap::update::StageRename;
using exosnap::update::SwapError;
using exosnap::update::UpdateManifest;
using exosnap::update::VerifyInstalledVersion;
using exosnap::update::VerifyManifestSignature;
using exosnap::update::VerifyPackage;
using exosnap::update::VerifyResult;
using exosnap::update::WaitForInstanceMutex;
using exosnap::update::WaitForProcessExit;

namespace {

constexpr const wchar_t* kAppWindowTitle = L"ExoSnap";
constexpr const wchar_t* kInstanceMutexName = L"ExoSnap_SingleInstance_Mutex";
constexpr const wchar_t* kExeName = L"exosnap.exe";
constexpr const char* kDefaultReleasesUrl = "https://api.github.com/repos/Exoridus/exosnap/releases";

constexpr std::chrono::seconds kCloseAppTimeout{60};
constexpr std::chrono::seconds kInstanceMutexTimeout{15};

// Best-effort recursive delete; true when the path is gone afterwards.
[[nodiscard]] bool RemoveTree(const std::wstring& dir) {
    std::error_code ec;
    fs::remove_all(fs::path(dir), ec);
    std::error_code ec2;
    return !fs::exists(fs::path(dir), ec2);
}

// Narrow (ACP) path for the update engine's std::string seams (VerifyPackage
// opens files through narrow ifstream paths on MSVC).
[[nodiscard]] std::string ToLocal8Bit(const std::wstring& w) {
    return QString::fromStdWString(w).toLocal8Bit().toStdString();
}

// The two-paths detail line for SwapError::RestoreFailed (worst case: the old
// tree is stranded in the backup dir). Appended under the B3 footer by main.
[[nodiscard]] QString RestoreFailedDetail(const exosnap::update::SwapPlan& plan) {
    return QStringLiteral("Restore failed - your previous version is at \"%1\"; the install path \"%2\" "
                          "could not be put back.")
        .arg(QString::fromStdWString(plan.backup_dir), QString::fromStdWString(plan.install_dir));
}

} // namespace

// ---------------------------------------------------------------------------
// Pure planning helpers
// ---------------------------------------------------------------------------

std::optional<std::wstring> ResolveStagedRoot(const std::wstring& extract_dir) {
    std::error_code ec;
    const fs::path root(extract_dir);
    if (!fs::is_directory(root, ec)) {
        return std::nullopt;
    }

    // Flat layout: the exe marks the root, regardless of any subdirectories.
    if (fs::exists(root / kExeName, ec)) {
        return extract_dir;
    }

    // Otherwise accept exactly one entry that is a directory (the ZIP's single
    // top-level folder). Stray siblings mean descending would silently drop
    // content -- refuse instead of guessing.
    std::optional<fs::path> only_entry;
    size_t entries = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
        ++entries;
        if (entries > 1) {
            return std::nullopt;
        }
        only_entry = entry.path();
    }
    if (entries != 1 || !only_entry.has_value()) {
        return std::nullopt;
    }
    std::error_code ec2;
    if (!fs::is_directory(*only_entry, ec2)) {
        return std::nullopt;
    }
    // The resolved root must actually carry exosnap.exe; a nested folder without
    // it is an unusable package, not a layout we can descend into.
    if (!fs::exists(*only_entry / kExeName, ec2)) {
        return std::nullopt;
    }
    return only_entry->wstring();
}

UpStep RetryEntryStep(FailureCase c) {
    switch (c) {
    case FailureCase::DownloadFailed:       // A1
    case FailureCase::VerifyDownloadFailed: // A2 (file already deleted)
        return UpStep::Download;
    case FailureCase::AppWontClose: // B1 (download kept)
        return UpStep::CloseApp;
    case FailureCase::InstallFailed:       // B2 (staging kept)
    case FailureCase::VerifyInstallFailed: // B3 (previous version restored)
    case FailureCase::UacDeclined:         // C1 (re-handoff)
    case FailureCase::MsiFailed:           // C2
        return UpStep::Install;
    case FailureCase::LaunchFailed: // B4 (soft success; manual start)
        return UpStep::Launch;
    }
    return UpStep::Download;
}

std::wstring BuildMsiexecParams(const std::wstring& msi_path) {
    return L"/i \"" + msi_path + L"\" /qn /norestart";
}

bool LaunchExoSnapFrom(const std::wstring& install_dir) {
    const std::wstring exe = (fs::path(install_dir) / kExeName).wstring();
    std::wstring cmdline = L"\"" + exe + L"\""; // CreateProcessW may modify the buffer
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                                     nullptr, install_dir.c_str(), &si, &pi);
    if (ok == 0) {
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

UpdaterWorker::UpdaterWorker(UpdaterArgs args, QObject* parent) : QObject(parent), args_(std::move(args)) {
    qRegisterMetaType<UpStep>("UpStep");
    qRegisterMetaType<FailureCase>("FailureCase");
}

void UpdaterWorker::run(UpStep entry) {
    cancel_.store(false);

    // A mid-pipeline entry without pipeline state (should not happen -- Retry
    // only re-enters steps that failed after Download populated it) falls back
    // to a clean full run.
    if (entry != UpStep::Download && !have_package_) {
        entry = UpStep::Download;
    }

    if (int(entry) <= int(UpStep::Download) && !runDownload()) {
        return;
    }
    if (int(entry) <= int(UpStep::CloseApp) && !runCloseApp()) {
        return;
    }
    if (int(entry) <= int(UpStep::Install)) {
        const bool ok =
            args_.install_mode == InstallMode::Portable ? runInstallPortable() : runInstallMsi();
        if (!ok) {
            return;
        }
    }
    if (int(entry) <= int(UpStep::Verify) && !runVerify()) {
        return;
    }
    (void)runLaunch();
}

// ── Step 1: Download ─────────────────────────────────────────────────────────
bool UpdaterWorker::runDownload() {
    emit stepStarted(UpStep::Download);

    const std::string base_url = args_.base_url.isEmpty() ? std::string(kDefaultReleasesUrl)
                                                          : args_.base_url.toStdString();
    std::string fetch_error;
    const std::optional<std::string> releases_json = FetchReleasesJson(base_url, fetch_error);
    if (!releases_json.has_value()) {
        emit failed(FailureCase::DownloadFailed, QString::fromStdString(fetch_error)); // A1
        return false;
    }

    std::string parse_error;
    const auto release = LocateRelease(*releases_json, args_.channel, &parse_error);
    if (!release.has_value()) {
        emit failed(FailureCase::DownloadFailed, // A1
                    parse_error.empty()
                        ? QStringLiteral("No release with an update manifest found for this channel.")
                        : QString::fromStdString(parse_error));
        return false;
    }
    emit releaseResolved(QString::fromStdString(release->version.ToString()));

    // %TEMP%\ExoSnapUpdate\<ver>\ -- downloads (manifest + package) live here.
    std::error_code ec;
    const fs::path download_dir =
        fs::temp_directory_path(ec) / L"ExoSnapUpdate" / fs::path(release->version.ToString());
    if (ec) {
        emit failed(FailureCase::DownloadFailed, QStringLiteral("No usable temp directory.")); // A1
        return false;
    }
    fs::create_directories(download_dir, ec);
    if (ec) {
        emit failed(FailureCase::DownloadFailed, QStringLiteral("Can't create the download directory.")); // A1
        return false;
    }

    // Manifest download.
    if (release->manifest_url.empty()) {
        emit failed(FailureCase::DownloadFailed, QStringLiteral("The release carries no update manifest.")); // A1
        return false;
    }
    const fs::path manifest_path = download_dir / L"update-manifest.json";
    if (const auto err = DownloadToFile(release->manifest_url, manifest_path.wstring(), {}, cancel_)) {
        emit failed(FailureCase::DownloadFailed, QString::fromStdString(*err)); // A1
        return false;
    }
    std::string manifest_json;
    {
        std::ifstream in(manifest_path, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        manifest_json = buf.str();
        if (!in) {
            emit failed(FailureCase::DownloadFailed, QStringLiteral("Can't read the downloaded manifest.")); // A1
            return false;
        }
    }

    // Manifest parse + signature -- BEFORE any field is acted upon (ADR 0012).
    const exosnap::update::ParseResult parsed = ParseManifest(manifest_json);
    if (std::holds_alternative<std::string>(parsed)) {
        emit failed(FailureCase::VerifyDownloadFailed, // A2 -- corrupt manifest is a security stop
                    QString::fromStdString(std::get<std::string>(parsed)));
        return false;
    }
    UpdateManifest manifest = std::get<UpdateManifest>(parsed);
    if (VerifyManifestSignature(manifest, manifest_json) != VerifyResult::Ok) {
        emit failed(FailureCase::VerifyDownloadFailed, QStringLiteral("Manifest signature invalid.")); // A2
        return false;
    }

    // Downgrade guard (unparseable current version defends as 0.0.0 -- never
    // blocks, the manifest minimum_accepted_version still applies).
    const SemVer current = ParseSemVer(args_.current_version.toStdString()).value_or(SemVer{});
    if (IsDowngrade(manifest, current)) {
        emit failed(FailureCase::VerifyDownloadFailed, // A2 -- blocked, nothing installed
                    QStringLiteral("The offered version %1 is below the installed version - downgrade blocked.")
                        .arg(QString::fromStdString(manifest.version.ToString())));
        return false;
    }

    const exosnap::update::PackageEntry* package = SelectPackage(manifest, args_.install_mode);
    if (package == nullptr) {
        emit failed(FailureCase::DownloadFailed, // A1
                    QStringLiteral("The update manifest has no %1 package.")
                        .arg(args_.install_mode == InstallMode::Portable ? QStringLiteral("portable")
                                                                         : QStringLiteral("installer")));
        return false;
    }

    // Package download, with byte progress (throttled to ~12 Hz).
    const bool portable = args_.install_mode == InstallMode::Portable;
    const fs::path package_path = download_dir / (portable ? L"package.zip" : L"package.msi");
    QElapsedTimer throttle;
    throttle.start();
    const auto on_progress = [this, &throttle](const DownloadProgress& p) {
        if (throttle.elapsed() >= 80 || (p.bytes_total != 0 && p.bytes_received >= p.bytes_total)) {
            throttle.restart();
            emit downloadProgress(p.bytes_received, p.bytes_total);
        }
    };
    if (const auto err = DownloadToFile(package->url, package_path.wstring(), on_progress, cancel_)) {
        emit failed(FailureCase::DownloadFailed, QString::fromStdString(*err)); // A1
        return false;
    }

    // SHA-256 gate -- a mismatch deletes the file inside VerifyPackage.
    const VerifyResult verdict = VerifyPackage({ToLocal8Bit(package_path.wstring()), package->sha256_hex});
    if (verdict != VerifyResult::Ok) {
        const QString detail =
            verdict == VerifyResult::PackageNotFound
                ? QStringLiteral("No package for this install type in the manifest.")
                : QStringLiteral("SHA-256 mismatch - the file was deleted.");
        emit failed(FailureCase::VerifyDownloadFailed, detail); // A2 -- hard stop
        return false;
    }

    manifest_ = std::move(manifest);
    package_path_ = package_path.wstring();
    have_package_ = true;

    if (portable) {
        plan_ = MakeSwapPlan(args_.install_dir.toStdWString(), manifest_.version);
        QString stage_error;
        if (!StagePortablePackage(&stage_error)) {
            emit failed(FailureCase::DownloadFailed, stage_error); // A1 -- retry re-downloads cleanly
            return false;
        }
    }

    emit stepDone(UpStep::Download);
    return true;
}

// Wipe + extract + descend. ExtractZip leaves partial output on a mid-fail, so
// the staging dir is always recreated from scratch (including on retry).
bool UpdaterWorker::StagePortablePackage(QString* error, bool* unusable_package) {
    if (!RemoveTree(plan_.staging_dir)) {
        *error = QStringLiteral("Can't clear the staging directory.");
        return false;
    }
    if (const auto err = ExtractZip(package_path_, plan_.staging_dir, {})) {
        (void)RemoveTree(plan_.staging_dir);
        *error = QString::fromStdString(*err);
        return false;
    }

    const std::optional<std::wstring> staged_root = ResolveStagedRoot(plan_.staging_dir);
    if (!staged_root.has_value()) {
        (void)RemoveTree(plan_.staging_dir);
        // The package extracted but carries no usable exe: re-staging it can
        // never succeed, so flag it unusable rather than a transient failure.
        if (unusable_package != nullptr) {
            *unusable_package = true;
        }
        *error = QStringLiteral("The downloaded package has an unexpected layout.");
        return false;
    }
    if (*staged_root == plan_.staging_dir) {
        return true; // flat zip: exosnap.exe already sits at the staging root
    }

    // Descend the single top-level folder: move it beside staging, drop the
    // now-empty wrapper, and rename it into place (same volume -> pure renames).
    const std::wstring hoist = plan_.staging_dir + L".hoist";
    (void)RemoveTree(hoist);
    if (::MoveFileExW(staged_root->c_str(), hoist.c_str(), 0) == 0 || !RemoveTree(plan_.staging_dir) ||
        ::MoveFileExW(hoist.c_str(), plan_.staging_dir.c_str(), 0) == 0) {
        (void)RemoveTree(hoist);
        (void)RemoveTree(plan_.staging_dir);
        *error = QStringLiteral("Can't arrange the staged files next to the install directory.");
        return false;
    }
    return true;
}

// ── Step 2: CloseApp ─────────────────────────────────────────────────────────
bool UpdaterWorker::runCloseApp() {
    emit stepStarted(UpStep::CloseApp);

    if (args_.app_pid != 0) {
        // The app keeps running until we ask it to close (close handshake).
        if (const HWND app_window = ::FindWindowW(nullptr, kAppWindowTitle)) {
            ::PostMessageW(app_window, WM_CLOSE, 0, 0);
        }
        if (!WaitForProcessExit(args_.app_pid, kCloseAppTimeout)) {
            emit failed(FailureCase::AppWontClose, QString()); // B1 -- download kept, Retry re-enters here
            return false;
        }
    }

    emit stepDone(UpStep::CloseApp);
    return true;
}

// ── Step 3a: Install (portable staged swap) ──────────────────────────────────
bool UpdaterWorker::runInstallPortable() {
    emit stepStarted(UpStep::Install);

    // Retry resilience (B2/B3 re-enter here): if the staging tree is gone or
    // incomplete, re-stage from the kept package; if even the package is gone,
    // fall back to the download-failed path so Retry restarts from Download.
    std::error_code ec;
    if (!fs::exists(fs::path(plan_.staging_dir) / kExeName, ec)) {
        if (package_path_.empty() || !fs::exists(fs::path(package_path_), ec)) {
            have_package_ = false;
            emit failed(FailureCase::DownloadFailed, // A1 -> Retry re-downloads
                        QStringLiteral("The downloaded update is no longer available."));
            return false;
        }
        QString stage_error;
        bool unusable_package = false;
        if (!StagePortablePackage(&stage_error, &unusable_package)) {
            if (unusable_package) {
                // The kept package can never install: drop it so Retry starts
                // over from Download instead of looping an unwinnable B2.
                have_package_ = false;
                emit failed(FailureCase::DownloadFailed, stage_error); // A1 -> re-download
            } else {
                emit failed(FailureCase::InstallFailed, stage_error); // B2
            }
            return false;
        }
    }

    switch (StageRename(plan_)) {
    case SwapError::None:
        break;
    case SwapError::StagingMissing: // nothing touched, old install intact
        emit failed(FailureCase::InstallFailed, QStringLiteral("The staged files are missing.")); // B2
        return false;
    case SwapError::BackupCollision: // nothing touched
        emit failed(FailureCase::InstallFailed,
                    QStringLiteral("A leftover backup directory could not be cleared.")); // B2
        return false;
    case SwapError::RenameOldFailed: // nothing touched
        emit failed(FailureCase::InstallFailed,
                    QStringLiteral("The current installation is in use and could not be moved.")); // B2
        return false;
    case SwapError::RenameNewFailed: // old version already restored and live again
        emit failed(FailureCase::VerifyInstallFailed, QString()); // B3 -- footer says "restored"
        return false;
    case SwapError::RestoreFailed: // worst case: old tree stranded in backup dir
        emit failed(FailureCase::VerifyInstallFailed, RestoreFailedDetail(plan_)); // B3 + both paths
        return false;
    }

    launch_dir_ = plan_.install_dir;
    emit stepDone(UpStep::Install);
    return true;
}

// ── Step 3b: Install (MSI handoff) ───────────────────────────────────────────
bool UpdaterWorker::runInstallMsi() {
    emit stepStarted(UpStep::Install);

    const std::wstring params = BuildMsiexecParams(package_path_);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // UAC elevation
    sei.lpFile = L"msiexec.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;

    if (::ShellExecuteExW(&sei) == 0) {
        if (::GetLastError() == ERROR_CANCELLED) {
            emit failed(FailureCase::UacDeclined, QString()); // C1 -- Retry re-handoffs
        } else {
            emit failed(FailureCase::MsiFailed, QString::number(::GetLastError())); // C2
        }
        return false;
    }
    if (sei.hProcess == nullptr) {
        emit failed(FailureCase::MsiFailed, QStringLiteral("no process")); // C2 -- can't observe the install
        return false;
    }

    ::WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(sei.hProcess, &exit_code);
    ::CloseHandle(sei.hProcess);
    if (exit_code != 0) {
        emit failed(FailureCase::MsiFailed, QString::number(exit_code)); // C2, code in detail
        return false;
    }

    emit stepDone(UpStep::Install);
    return true;
}

// ── Step 4: Verify ───────────────────────────────────────────────────────────
bool UpdaterWorker::runVerify() {
    emit stepStarted(UpStep::Verify);

    if (args_.install_mode == InstallMode::Portable) {
        if (!VerifyInstalledVersion(plan_)) {
            const SwapError restore = RestoreBackup(plan_);
            emit failed(FailureCase::VerifyInstallFailed, // B3
                        restore == SwapError::RestoreFailed ? RestoreFailedDetail(plan_) : QString());
            return false;
        }
        launch_dir_ = plan_.install_dir;
    } else {
        // The MSI stamps [INSTALLFOLDER] (may carry a trailing backslash --
        // MakeSwapPlan normalises it) into the registry post-install.
        const std::wstring install_dir = ReadInstallPath().value_or(args_.install_dir.toStdWString());
        if (install_dir.empty()) {
            emit failed(FailureCase::VerifyInstallFailed,
                        QStringLiteral("The installer did not record an install location.")); // B3
            return false;
        }
        const exosnap::update::SwapPlan msi_plan = MakeSwapPlan(install_dir, manifest_.version);
        if (!VerifyInstalledVersion(msi_plan)) {
            emit failed(FailureCase::VerifyInstallFailed, QString()); // B3
            return false;
        }
        launch_dir_ = msi_plan.install_dir;
    }

    emit stepDone(UpStep::Verify);
    return true;
}

// ── Step 5: Launch ───────────────────────────────────────────────────────────
bool UpdaterWorker::runLaunch() {
    emit stepStarted(UpStep::Launch);

    if (!LaunchExoSnapFrom(launch_dir_)) {
        // B4 soft success: the new version is installed and verified; the
        // backup is kept for safety (cleared by the next update's StageRename).
        emit failed(FailureCase::LaunchFailed, QString());
        return false;
    }

    if (!WaitForInstanceMutex(kInstanceMutexName, kInstanceMutexTimeout)) {
        if (args_.install_mode == InstallMode::Portable) {
            // The new build never came up -- put the old version back.
            const SwapError restore = RestoreBackup(plan_);
            emit failed(FailureCase::VerifyInstallFailed, // B3
                        restore == SwapError::RestoreFailed ? RestoreFailedDetail(plan_) : QString());
        } else {
            // MSI: nothing to restore; the install itself verified fine.
            emit failed(FailureCase::LaunchFailed, QString()); // B4
        }
        return false;
    }

    if (args_.install_mode == InstallMode::Portable) {
        (void)exosnap::update::CleanupBackup(plan_);
    }
    emit stepDone(UpStep::Launch);
    emit allDone();
    return true;
}
