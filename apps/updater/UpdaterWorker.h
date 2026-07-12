#pragma once

// UpdaterWorker.h -- the background update pipeline of the standalone updater.
//
// A QObject that main.cpp moves onto a QThread. It runs the full
// download -> close-app -> install -> verify -> launch pipeline for both
// install modes (portable staged swap, MSI handoff) and reports progress
// exclusively through signals; the pure UpdaterController (and the window) are
// driven only on the GUI thread via queued connections. No QtWidgets here --
// Qt Core + the Qt-free update engine only.
//
// The planning pieces (staged-root resolution, retry routing, the msiexec
// command line) are free functions so they are unit-testable without a thread,
// a network, or an install tree (tests/test_updater_worker_plan.cpp).

#include <QObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <update/swap_engine.h>
#include <update/update_types.h>

#include "UpdaterArgs.h"
#include "UpdaterController.h" // UpStep / FailureCase

// ---------------------------------------------------------------------------
// Pure planning helpers
// ---------------------------------------------------------------------------

// Where the usable application root sits under `extract_dir` after a portable
// ZIP extraction. The release ZIP wraps everything in a single top-level
// folder ("ExoSnap-<ver>-windows-x64-portable/"); a flat layout is accepted
// too:
//   - extract_dir itself contains exosnap.exe                -> extract_dir
//   - exactly one entry, a directory that contains exosnap.exe -> that directory
//   - anything else (missing, empty, several entries, no exe) -> nullopt
[[nodiscard]] std::optional<std::wstring> ResolveStagedRoot(const std::wstring& extract_dir);

// Which pipeline step a Retry / Re-download press re-enters for a failure:
// A1/A2 -> Download, B1 -> CloseApp, B2/B3/C1/C2 -> Install, B4 -> Launch.
[[nodiscard]] UpStep RetryEntryStep(FailureCase c);

// msiexec parameter string for a silent install: /i "<msi>" /qn /norestart.
[[nodiscard]] std::wstring BuildMsiexecParams(const std::wstring& msi_path);

// Start <install_dir>\exosnap.exe detached (working dir = install_dir).
// Shared by the pipeline's Launch step and the window's "Open ..." actions.
[[nodiscard]] bool LaunchExoSnapFrom(const std::wstring& install_dir);

// Open `path` read-only with a deny-write / deny-delete share mode and return
// the raw Win32 HANDLE (as void*; INVALID_HANDLE_VALUE on failure — the caller
// owns and must close it via ClosePackageLock / CloseHandle). While this handle
// is open the OS refuses every write, rename, and delete open on the file — from
// any process, same user or not — and its open child handle also pins the parent
// directories against rename. That is what closes the verify->consume TOCTOU
// window: the verified bytes cannot be swapped before an elevated msiexec (or
// the portable extractor) reads them. msiexec still opens the source read-only,
// which the deny-write lock permits.
[[nodiscard]] void* OpenPackageWriteLock(const std::wstring& path);

// Close a handle returned by OpenPackageWriteLock (nullptr / INVALID are no-ops).
// Used as the deleter for the worker's owned lock handle.
void ClosePackageLock(void* handle) noexcept;

// Blocks until `process_handle` (a Win32 HANDLE, passed as void* so this header
// stays windows.h-free) is signaled, `cancel` flips true, or `timeout` elapses
// -- polled at a fixed short interval rather than a single infinite wait.
// Returns true only when the handle was actually observed signaled (the caller
// may then read GetExitCodeProcess); false means the wait gave up early
// (cancelled or timed out) and the caller must treat that as a failure rather
// than block forever. This is what keeps a wedged msiexec (a hung custom
// action, a suppressed-but-still-open reboot prompt under /qn) from pinning
// the worker thread in an unkillable WaitForSingleObject(..., INFINITE) --
// without it, main.cpp's shutdown path has no choice but to TerminateThread()
// the worker after its 30 s budget, which can abandon the elevated handoff
// mid-flight.
[[nodiscard]] bool WaitForProcessOrCancel(void* process_handle, std::chrono::milliseconds timeout,
                                          const std::atomic<bool>& cancel);

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

class UpdaterWorker : public QObject {
    Q_OBJECT
  public:
    explicit UpdaterWorker(UpdaterArgs args, QObject* parent = nullptr);

    // Cooperative cancel for in-flight downloads (process shutdown). Safe from
    // any thread.
    void requestCancel() { cancel_.store(true); }

  public slots:
    // Run the pipeline from `entry` to the end (initial run: Download; retries
    // re-enter per RetryEntryStep). Invoked queued from the GUI thread; emits
    // either allDone() or exactly one failed(...) before returning.
    void run(UpStep entry);

  signals:
    void stepStarted(UpStep step);
    void downloadProgress(quint64 received, quint64 total);
    void stepDone(UpStep step);
    // The version the channel resolved to ("0.9.0") -- lets the GUI swap its
    // controller from the placeholder to-version to the real one.
    void releaseResolved(QString to_version);
    void allDone();
    void failed(FailureCase c, QString detail);

  private:
    [[nodiscard]] bool runDownload();
    [[nodiscard]] bool runCloseApp();
    [[nodiscard]] bool runInstallPortable();
    [[nodiscard]] bool runInstallMsi();
    [[nodiscard]] bool runVerify();
    [[nodiscard]] bool runLaunch();

    // Wipe plan_.staging_dir, extract the kept package into it and descend the
    // single top-level ZIP folder so exosnap.exe sits directly in staging. When
    // the extracted package has no usable exe (bad layout), *unusable_package is
    // set so the caller can drop the kept package and re-download instead of
    // looping an unwinnable install.
    [[nodiscard]] bool StagePortablePackage(QString* error, bool* unusable_package = nullptr);

    // Take a deny-write / deny-delete lock on package_path_ and verify its bytes
    // against `expected_sha256` THROUGH that handle, keeping the handle in
    // locked_package_ so the verified package cannot be swapped before it is
    // consumed (elevated msiexec / portable extraction). On a hash mismatch the
    // file is deleted (no partial binary retained). Any prior lock is released
    // first so a re-download can replace the file.
    [[nodiscard]] bool LockAndVerifyPackage(const std::string& expected_sha256, QString* error);

    const UpdaterArgs args_;
    std::atomic<bool> cancel_{false};

    // Pipeline state kept across retries (B1/B2/B3/C1 re-enter mid-pipeline).
    exosnap::update::UpdateManifest manifest_{};
    exosnap::update::SwapPlan plan_{}; // portable swap plan
    std::wstring package_path_;        // downloaded .zip / .msi
    std::string package_sha256_;       // expected SHA-256 of package_path_ (for the install re-lock guard)
    std::wstring launch_dir_;          // where the Launch step finds exosnap.exe
    // %TEMP%\ExoSnapUpdate\<ver>\ -- holds the manifest, its signature, and the
    // package for the whole run. Removed on the success path once Launch has
    // confirmed the new instance is up (runLaunch); error paths already clean
    // up their own artifacts inline and leave this alone so a Retry can reuse
    // what was already downloaded/staged.
    std::wstring download_dir_;
    bool have_package_ = false; // Download completed at least once

    // Deny-write / deny-delete lock on package_path_, held from verification
    // until the package is consumed (closes the verify->consume TOCTOU window).
    std::unique_ptr<void, decltype(&ClosePackageLock)> locked_package_{nullptr, &ClosePackageLock};
};

Q_DECLARE_METATYPE(UpStep)
Q_DECLARE_METATYPE(FailureCase)
