#include "bootstrap/ProductionBootstrap.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>

#include <utility>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion
#include "diagnostics/AppLog.h"
#include "diagnostics/EngineLogBridge.h"
#include "diagnostics/StartupClock.h"
#include "diagnostics/StartupTrace.h"
#include "exosnap_resource.h"
#include "services/ElevatedRelaunch.h"

#if defined(Q_OS_WIN)
#include <crash_capture/crash_capture.h>
#include <update/install_mode_detector.h>
#include <update/swap_engine.h>
#include <update/update_types.h>

#include <filesystem>
#include <system_error>

#include <windows.h>
#endif

namespace exosnap::bootstrap {
namespace {

// Shared with apps/updater (kInstanceMutexName) and swap_engine's
// WaitForInstanceMutex: the updater proves the relaunched app came up by
// waiting for exactly this name. Renaming it here without renaming it there
// turns a successful update into a "the new version never started" report.
constexpr const wchar_t* kSingleInstanceMutexName = L"ExoSnap_SingleInstance_Mutex";

constexpr const char* kAppIconResourcePath = ":/brand/exosnap-logo-idle.ico";

void LogPerfMilestone(const QString& label, qint64 elapsed_ms) {
    diagnostics::AppLog::info(QStringLiteral("perf"), QStringLiteral("%1 %2 ms").arg(label).arg(elapsed_ms));
    diagnostics::StartupTrace::instance().record(label, elapsed_ms);
}

} // namespace

PreAppResult RunPreApplicationPhase() {
#if defined(Q_OS_WIN)
    // Restrict the loader's default DLL search order before anything else can
    // run (including Qt's own initialization, which may LoadLibrary plugins).
    // Without this the implicit search order can fall back to directories that
    // let a planted DLL sitting next to a portable-ZIP extraction be preferred
    // over the real one. LOAD_LIBRARY_SEARCH_APPLICATION_DIR keeps the app's own
    // directory (where the windeployqt-deployed Qt/FFmpeg DLLs live) searchable.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // Self-heal an update swap interrupted between its two directory renames
    // (see swap_engine.h). A process kill in that narrow window -- or between a
    // failed second rename and its own compensating restore -- can leave the
    // registry-recorded install dir gone with the last-known-good tree stranded
    // in its ".old" sibling. The updater runs this after every swap; this covers
    // the case where the updater itself never got to. A no-op when the install
    // dir already carries exosnap.exe (the overwhelmingly common case) or when
    // no install path is recorded at all (a dev build).
    if (const auto install_dir = update::ReadInstallPath(); install_dir.has_value() && !install_dir->empty()) {
        const auto swap_plan = update::MakeSwapPlan(*install_dir, update::SemVer{});
        (void)update::RepairOrphanedSwap(swap_plan);
    }
#endif

    // PERF-MEASURE: start the process-global startup clock before anything else
    // so first-paint / preview-live milestones measure true start->milestone
    // latency rather than latency from whenever measuring became convenient.
    diagnostics::StartupClock().start();

    PreAppResult result;
    result.main_start_ms = diagnostics::StartupClock().elapsed();
    return result;
}

PostAppResult MarkApplicationConstructed() {
    PostAppResult result;
    result.qapplication_created_ms = diagnostics::StartupClock().elapsed();
    return result;
}

void ApplyApplicationMetadata() {
    QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
}

bool IsolateHarnessConfigDir(const QString& harness_id) {
    if (qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR"))
        return false;

    const QString isolated = QDir(QDir::tempPath()).filePath(QStringLiteral("exosnap-%1").arg(harness_id));
    QDir().mkpath(isolated);
    qputenv("EXOSNAP_CONFIG_DIR", isolated.toUtf8());
    qInfo().noquote() << QStringLiteral("%1: isolated config dir %2").arg(harness_id, isolated);
    return true;
}

bool AlignQmlDiskCacheWithConfigDir() {
    if (!qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR"))
        return false;
    // An explicit choice by the caller outranks this one.
    if (qEnvironmentVariableIsSet("QML_DISK_CACHE_PATH"))
        return false;

    const QString config_dir = qEnvironmentVariable("EXOSNAP_CONFIG_DIR");
    if (config_dir.isEmpty())
        return false;

    const QString cache_dir = QDir(config_dir).filePath(QStringLiteral("qmlcache"));
    QDir().mkpath(cache_dir);
    qputenv("QML_DISK_CACHE_PATH", QDir::toNativeSeparators(cache_dir).toUtf8());
    return true;
}

QIcon InstallApplicationIcon() {
    const QString icon_path = QString::fromLatin1(kAppIconResourcePath);
    if (!QFile::exists(icon_path))
        qWarning().noquote() << "Runtime app icon resource missing:" << icon_path;

    QIcon app_icon(icon_path);
    if (app_icon.isNull()) {
        qWarning().noquote() << "Runtime app icon failed to load from resource:" << icon_path;
        return app_icon;
    }

    // An icon that loads but exposes no sizes renders as nothing at every scale;
    // it is worth a warning because the symptom (blank taskbar entry) otherwise
    // looks like a platform problem rather than a packaging one.
    if (app_icon.availableSizes().isEmpty())
        qWarning().noquote() << "Runtime app icon loaded but reports no available sizes.";

    QGuiApplication::setWindowIcon(app_icon);
    return app_icon;
}

void ApplyNativeWindowIcons(void* native_window_handle) {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(native_window_handle);
    if (hwnd == nullptr) {
        qWarning().noquote() << "HWND unavailable while applying WM_SETICON.";
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        qWarning().noquote() << "GetModuleHandleW failed while applying WM_SETICON. error="
                             << static_cast<unsigned long>(GetLastError());
        return;
    }

    HICON small_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));

    if (small_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON failed to load ICON_SMALL from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }

    if (big_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON failed to load ICON_BIG from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
#else
    (void)native_window_handle;
#endif
}

ProductionBootstrap::ProductionBootstrap(BootstrapOptions options) : options_(std::move(options)) {
}

ProductionBootstrap::~ProductionBootstrap() {
    Shutdown();

    // The single-instance mutex is deliberately NOT released here on the normal
    // path. Holding it until the OS reclaims the handle at process exit keeps
    // the "one instance" window closed for the whole teardown; releasing it a
    // few instructions earlier would let a launch racing our exit slip past the
    // guard and come up alongside a process still holding capture devices open.
    // The one case that must hand it over -- the elevated relaunch -- releases
    // it explicitly in RunPendingElevatedRelaunch().
}

void ProductionBootstrap::InitializeLogging(const PreAppResult& pre_app, const PostAppResult& post_app) {
    if (logging_initialized_)
        return;
    logging_initialized_ = true;

    // PERF-MEASURE: bring the sink up as early as possible so the startup
    // milestones land in the physical log file instead of being dropped for
    // having happened before logging existed. AppLog::init() is idempotent, so a
    // later call from the frontend stays a harmless no-op.
    diagnostics::AppLog::init();
    exosnap::InitializeEngineLogging();

    LogPerfMilestone(QStringLiteral("main-start"), pre_app.main_start_ms);
    LogPerfMilestone(QStringLiteral("qapplication-created"), post_app.qapplication_created_ms);
}

void ProductionBootstrap::RecordStartupMilestone(const QString& label, qint64 elapsed_ms) {
    if (!logging_initialized_)
        return;
    LogPerfMilestone(label, elapsed_ms);
}

SingleInstanceOutcome ProductionBootstrap::AcquireSingleInstance() {
    if (options_.suppress_single_instance)
        return SingleInstanceOutcome::Suppressed;

#if defined(Q_OS_WIN)
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);

        // A second launch is almost always the user reaching for the app again,
        // so surface the instance they already have instead of exiting silently.
        const std::wstring title = options_.single_instance_window_title.isEmpty()
                                       ? std::wstring(kAppWindowTitle)
                                       : options_.single_instance_window_title.toStdWString();
        HWND existing = FindWindowW(nullptr, title.c_str());
        if (existing != nullptr) {
            if (IsIconic(existing))
                ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return SingleInstanceOutcome::AlreadyRunning;
    }

    single_instance_mutex_ = mutex;
    return SingleInstanceOutcome::Acquired;
#else
    return SingleInstanceOutcome::Suppressed;
#endif
}

void ProductionBootstrap::InitializeCrashCapture() {
#if defined(Q_OS_WIN)
    crash_capture_attempted_ = true;

    // ADR 0017. In the default OFF/stub build this no-ops the Sentry path, but
    // the session sidecar and the next-launch dialog still work -- that is
    // intended, "the previous session never marked a clean exit" is detectable
    // without any upload channel.
    crash_dir_ = crash_capture::ResolveCrashDir();
    if (crash_dir_.empty()) {
        qWarning().noquote() << "Crash capture disabled: could not resolve crash dir.";
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(crash_dir_), ec);
    if (ec) {
        qWarning().noquote() << "Failed to create crash dir:" << QString::fromStdString(crash_dir_)
                             << QString::fromStdString(ec.message());
    }

    crash_capture::CrashCaptureConfig crash_cfg;
    crash_cfg.crash_dir = crash_dir_;
    crash_cfg.handler_exe_path = crash_capture::ResolveHandlerExePath();
    crash_cfg.app_version = build::kVersion;
    crash_cfg.debug_mode = false;
    crash_capture::Initialize(crash_cfg);
#endif
}

SentryTestEventOutcome ProductionBootstrap::RunSentryTestEventIfRequested() {
#if defined(Q_OS_WIN)
    if (!qEnvironmentVariableIsSet("EXOSNAP_SENTRY_TEST_EVENT"))
        return SentryTestEventOutcome::NotRequested;

    crash_capture::GiveUserConsent();
    crash_capture::SendTestEvent("It works! - ExoSnap Sentry verify");
    // Full shutdown rather than just a flush: this path exits without ever
    // reaching an event loop, so the clean-exit marker has to be written here or
    // the next launch reports a crash that never happened.
    Shutdown();
    qInfo().noquote() << "Sentry test event sent; exiting.";
    return SentryTestEventOutcome::SentExitNow;
#else
    return SentryTestEventOutcome::NotRequested;
#endif
}

void ProductionBootstrap::requestElevatedRelaunch(const QStringList& args) {
    elevated_relaunch_args_ = args;
    elevated_relaunch_requested_ = true;
}

void ProductionBootstrap::RunPendingElevatedRelaunch() {
    if (!elevated_relaunch_requested_)
        return;
    elevated_relaunch_requested_ = false;

#if defined(Q_OS_WIN)
    // ADR 0033. The successor has to be able to take the single-instance guard,
    // so hand it over first -- otherwise the elevated process comes up, finds
    // the mutex still held by the exiting one, and activates a window that is
    // already going away.
    ReleaseSingleInstance();

    const services::RelaunchResult relaunch_result =
        services::RelaunchAsAdmin(QCoreApplication::applicationFilePath(), elevated_relaunch_args_);
    if (relaunch_result == services::RelaunchResult::UserDeclined) {
        qInfo().noquote() << "Elevated relaunch cancelled by user (UAC declined).";
    } else if (relaunch_result == services::RelaunchResult::Failed) {
        qWarning().noquote() << "Elevated relaunch failed (ShellExecuteEx).";
    }
#endif
}

void ProductionBootstrap::Shutdown() {
    if (shut_down_)
        return;
    shut_down_ = true;

    // Detach the sink before AppLog is torn down: a late engine record must not
    // reach a half-destroyed sink.
    if (logging_initialized_)
        exosnap::ShutdownEngineLogging();

#if defined(Q_OS_WIN)
    if (crash_capture_attempted_) {
        // Mark the clean exit so the next launch does not show the crash dialog,
        // then flush and stop the engine.
        if (!crash_dir_.empty())
            crash_capture::MarkCleanExit(crash_dir_);
        crash_capture::Shutdown();
    }
#endif

    // Last, so the successor starts against a process that has already flushed
    // its logs and released its crash session.
    RunPendingElevatedRelaunch();
}

void ProductionBootstrap::ReleaseSingleInstance() {
#if defined(Q_OS_WIN)
    if (single_instance_mutex_ == nullptr)
        return;
    HANDLE mutex = static_cast<HANDLE>(single_instance_mutex_);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    single_instance_mutex_ = nullptr;
#endif
}

} // namespace exosnap::bootstrap
