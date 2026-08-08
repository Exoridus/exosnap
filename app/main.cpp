#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QProcess>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QWindow>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion
#include "MainWindow.h"
#include "diagnostics/AppLog.h"
#include "diagnostics/EngineLogBridge.h"
#include "diagnostics/StartupClock.h"
#include "diagnostics/StartupTrace.h"
#include "exosnap_resource.h"
#include "services/ElevatedRelaunch.h"
#include "services/VerifyReinstallMode.h"
#include "ui/theme/ExoSnapTheme.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "visual_tests/VisualTestHarness.h"
#endif
#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
#include "auto_record/AutoRecordHarness.h"
#endif
#if defined(EXOSNAP_ENABLE_HWND_AUDIT_HARNESS)
#include "hwnd_audit/HwndAuditHarness.h"
#endif

#if defined(Q_OS_WIN)
#include <crash_capture/crash_capture.h>
#include <update/install_mode_detector.h>
#include <update/swap_engine.h>
#include <update/update_types.h>

#include <filesystem>
#include <string>

#include <windows.h>
#endif

namespace {

constexpr const wchar_t* kSingleInstanceMutexName = L"ExoSnap_SingleInstance_Mutex";

QString FormatIconSizes(const QList<QSize>& sizes) {
    QStringList out;
    out.reserve(sizes.size());
    for (const QSize& size : sizes) {
        out.push_back(QStringLiteral("%1x%2").arg(size.width()).arg(size.height()));
    }
    return out.join(QStringLiteral(", "));
}

void LogIconLoadDiagnostics(const QString& source, const QIcon& icon) {
    if (icon.isNull()) {
        qWarning().noquote() << source << "is null.";
        return;
    }

    const QList<QSize> sizes = icon.availableSizes();
    if (sizes.isEmpty()) {
        qWarning().noquote() << source << "loaded but reports no available sizes.";
    }
}

#if defined(Q_OS_WIN)
void ApplyNativeWindowIcons(QWidget& window) {
    HWND hwnd = reinterpret_cast<HWND>(window.winId());
    if (hwnd == nullptr) {
        qWarning().noquote() << "HWND unavailable while applying WM_SETICON from main.cpp.";
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        qWarning().noquote() << "GetModuleHandleW failed while applying WM_SETICON from main.cpp. error="
                             << static_cast<unsigned long>(GetLastError());
        return;
    }

    HICON small_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));

    if (small_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON failed to load ICON_SMALL from EXE resources in main.cpp. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }

    if (big_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON failed to load ICON_BIG from EXE resources in main.cpp. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#if defined(Q_OS_WIN)
    // Restrict the loader's default DLL search order before anything else can run
    // (including Qt's own initialization, which may LoadLibrary plugins). Without
    // this, the implicit search order can fall back to directories that let a
    // planted DLL sitting next to a portable-ZIP extraction get preferred over the
    // real one. LOAD_LIBRARY_SEARCH_APPLICATION_DIR keeps the app's own directory
    // (where the Qt/FFmpeg DLLs deployed by windeployqt live) searchable.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    // Self-heal an update swap interrupted between its two directory renames
    // (see swap_engine.h — a process kill in that narrow window, or between a
    // failed second rename and its own compensating restore, can leave the
    // registry-recorded install dir gone with the last-known-good tree stranded
    // in its ".old" sibling). The updater already runs this after every swap;
    // this covers the case where the updater itself never got to. No-op when
    // the install dir already carries exosnap.exe (the overwhelmingly common
    // case) or there is no registry-recorded install path (e.g. a dev build).
    if (const auto install_dir = exosnap::update::ReadInstallPath(); install_dir.has_value() && !install_dir->empty()) {
        const auto swap_plan = exosnap::update::MakeSwapPlan(*install_dir, exosnap::update::SemVer{});
        (void)exosnap::update::RepairOrphanedSwap(swap_plan);
    }
#endif

    // PERF-MEASURE: start the process-global startup clock before anything else so
    // first-paint / preview-live milestones measure true start→milestone latency.
    exosnap::diagnostics::StartupClock().start();
    // Capture elapsed time at the true moment of each early milestone; the actual
    // AppLog::info() calls happen further down (AppLog::init() needs QCoreApplication
    // to exist, so it cannot run before QApplication is constructed), but the elapsed
    // value below is read right here so the logged number still reflects the real
    // start→milestone latency, not the latency to whenever logging became possible.
    const qint64 main_start_ms = exosnap::diagnostics::StartupClock().elapsed();

    QApplication app(argc, argv);

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // The visual-test harness runs the real application, and the real application
    // persists its live configuration while it runs. Pointed at the developer's
    // config directory it silently overwrites their settings with the scenario's
    // synthetic ones. Isolate before anything can read or write that directory —
    // an opt-in environment variable is not enough, because forgetting it is
    // exactly the mistake that destroys data.
    if (exosnap::visual::HasVisualTestRequest(QCoreApplication::arguments()) &&
        !qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR")) {
        const QString isolated = QDir(QDir::tempPath()).filePath(QStringLiteral("exosnap-visual-test"));
        QDir().mkpath(isolated);
        qputenv("EXOSNAP_CONFIG_DIR", isolated.toUtf8());
        qInfo().noquote() << "visual test: isolated config dir" << isolated;
    }
#endif
#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    // Same rationale as the visual-test isolation above: an auto-record run must
    // never write into the developer's real config directory.
    if (exosnap::auto_record::HasAutoRecordRequest(QCoreApplication::arguments()) &&
        !qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR")) {
        const QString isolated = QDir(QDir::tempPath()).filePath(QStringLiteral("exosnap-auto-record"));
        QDir().mkpath(isolated);
        qputenv("EXOSNAP_CONFIG_DIR", isolated.toUtf8());
        qInfo().noquote() << "auto-record: isolated config dir" << isolated;
    }
#endif
#if defined(EXOSNAP_ENABLE_HWND_AUDIT_HARNESS)
    // Same rationale as the two blocks above: an audit run brings up the real
    // application, which persists its live configuration while it runs.
    if (exosnap::hwnd_audit::HasHwndAuditRequest(QCoreApplication::arguments()) &&
        !qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR")) {
        const QString isolated = QDir(QDir::tempPath()).filePath(QStringLiteral("exosnap-hwnd-audit"));
        QDir().mkpath(isolated);
        qputenv("EXOSNAP_CONFIG_DIR", isolated.toUtf8());
        qInfo().noquote() << "hwnd-audit: isolated config dir" << isolated;
    }
#endif
    const qint64 qapplication_created_ms = exosnap::diagnostics::StartupClock().elapsed();
    app.setApplicationName("ExoSnap");

    // PERF-MEASURE: bring the log sink up as early as possible so main-start /
    // qapplication-created / theme-applied land in the physical log file. Previously
    // AppLog::init() only ran once MainWindow was constructed (see MainWindow.cpp);
    // that call is still there and is a harmless no-op (AppLog::init() is idempotent).
    exosnap::diagnostics::AppLog::init();
    exosnap::InitializeEngineLogging();
    exosnap::diagnostics::AppLog::info(QStringLiteral("perf"), QStringLiteral("main-start %1 ms").arg(main_start_ms));
    exosnap::diagnostics::AppLog::info(QStringLiteral("perf"),
                                       QStringLiteral("qapplication-created %1 ms").arg(qapplication_created_ms));
    exosnap::diagnostics::StartupTrace::instance().record(QStringLiteral("main-start"), main_start_ms);
    exosnap::diagnostics::StartupTrace::instance().record(QStringLiteral("qapplication-created"),
                                                          qapplication_created_ms);

    static const QString kAppIconPath = QStringLiteral(":/brand/exosnap-logo-idle.ico");
    if (!QFile::exists(kAppIconPath))
        qWarning().noquote() << "Runtime app icon resource missing:" << kAppIconPath;

    QIcon app_icon(kAppIconPath);
    if (app_icon.isNull()) {
        qWarning().noquote() << "Runtime app icon failed to load from resource:" << kAppIconPath;
    } else {
        LogIconLoadDiagnostics(QStringLiteral("Runtime app icon"), app_icon);
    }

    if (!app_icon.isNull())
        QApplication::setWindowIcon(app_icon);
    exosnap::ui::theme::ApplyExoSnapTheme(app);
    {
        const qint64 theme_applied_ms = exosnap::diagnostics::StartupClock().elapsed();
        exosnap::diagnostics::AppLog::info(QStringLiteral("perf"),
                                           QStringLiteral("theme-applied %1 ms").arg(theme_applied_ms));
        exosnap::diagnostics::StartupTrace::instance().record(QStringLiteral("theme-applied"), theme_applied_ms);
    }

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    exosnap::visual::VisualTestOptions visual_options;
    QString visual_parse_error;
    const bool visual_test_requested = exosnap::visual::HasVisualTestRequest(QCoreApplication::arguments());
    if (visual_test_requested &&
        !exosnap::visual::ParseVisualTestOptions(QCoreApplication::arguments(), &visual_options, &visual_parse_error)) {
        qCritical().noquote() << visual_parse_error;
        return 2;
    }
#else
    constexpr bool visual_test_requested = false;
#endif

#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    exosnap::auto_record::AutoRecordOptions auto_record_options;
    QString auto_record_parse_error;
    const bool auto_record_requested = exosnap::auto_record::HasAutoRecordRequest(QCoreApplication::arguments());
    if (auto_record_requested && !exosnap::auto_record::ParseAutoRecordOptions(
                                     QCoreApplication::arguments(), &auto_record_options, &auto_record_parse_error)) {
        qCritical().noquote() << auto_record_parse_error;
        return 2;
    }
#else
    constexpr bool auto_record_requested = false;
#endif

#if defined(EXOSNAP_ENABLE_HWND_AUDIT_HARNESS)
    const bool hwnd_audit_requested = exosnap::hwnd_audit::HasHwndAuditRequest(QCoreApplication::arguments());
#else
    constexpr bool hwnd_audit_requested = false;
#endif

#if defined(Q_OS_WIN)
    HANDLE hMutex = nullptr;
    // In a Release build every harness macro is undefined, so visual_test_requested,
    // auto_record_requested and hwnd_audit_requested are all `constexpr false` — the compound
    // condition folds to a compile-time constant and MSVC's /W4 flags it (C4127), which /WX
    // then hard-fails on. The check is a genuine runtime branch in non-Release builds, so
    // suppress locally rather than restructure (matches this codebase's existing
    // MSVC-suppression convention).
    //
    // Every harness must stay out of this branch. Beyond the second instance exiting
    // immediately, the single-instance path calls SetForegroundWindow on the running window —
    // a harness run would yank focus off whatever the developer is doing, which CLAUDE.md
    // rules out.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127) // conditional expression is constant (Release-only fold)
#endif
    if (!visual_test_requested && !auto_record_requested && !hwnd_audit_requested) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        hMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
        if (hMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(hMutex);

            HWND existingHwnd = FindWindowW(nullptr, L"ExoSnap");
            if (existingHwnd != nullptr) {
                if (IsIconic(existingHwnd))
                    ShowWindow(existingHwnd, SW_RESTORE);
                SetForegroundWindow(existingHwnd);
            }
            return 0;
        }
    }
#endif

#if defined(Q_OS_WIN)
    // ---- Crash capture init (ADR 0017) --------------------------------------
    // Resolve + create the crash dir, then Initialize. In the default OFF/stub
    // build this no-ops the Sentry path but the session sidecar + next-launch
    // dialog still work (that is intended). BeginSession is NOT called here —
    // MainWindow does it after ReadPreviousCrashContext so the previous-session
    // crash context survives the rewrite.
    std::string crash_dir = exosnap::crash_capture::ResolveCrashDir();
    if (!crash_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(crash_dir), ec);
        if (ec) {
            qWarning().noquote() << "Failed to create crash dir:" << QString::fromStdString(crash_dir)
                                 << QString::fromStdString(ec.message());
        }
        exosnap::crash_capture::CrashCaptureConfig crash_cfg;
        crash_cfg.crash_dir = crash_dir;
        crash_cfg.handler_exe_path = exosnap::crash_capture::ResolveHandlerExePath();
        crash_cfg.app_version = exosnap::build::kVersion;
        crash_cfg.debug_mode = false;
        exosnap::crash_capture::Initialize(crash_cfg);
    } else {
        qWarning().noquote() << "Crash capture disabled: could not resolve crash dir.";
    }

    // Sentry "Verify" hook: EXOSNAP_SENTRY_TEST_EVENT=1 sends one diagnostic
    // event and exits. Harmless in production (env var unset); only does
    // anything in an official ON build where a DSN is compiled in.
    if (qEnvironmentVariableIsSet("EXOSNAP_SENTRY_TEST_EVENT")) {
        exosnap::crash_capture::GiveUserConsent();
        exosnap::crash_capture::SendTestEvent("It works! - ExoSnap Sentry verify");
        if (!crash_dir.empty())
            exosnap::crash_capture::MarkCleanExit(crash_dir);
        exosnap::crash_capture::Shutdown(); // flushes pending events
        qInfo().noquote() << "Sentry test event sent; exiting.";
        return 0;
    }
#endif

    // ELEVATION-FOUNDATION-R1 (ADR 0033): parse the elevated-relaunch handoff
    // from our own argv (set by a prior `runas` self-relaunch). Pure parse; the
    // window applies it (navigate + re-enable opt-in) once constructed.
    const exosnap::services::RelaunchHandoff startup_handoff =
        exosnap::services::ParseRelaunchArgs(QCoreApplication::arguments());

#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    if (auto_record_requested && !auto_record_options.enable_preview) {
        const int rc = exosnap::auto_record::RunAutoRecord(app, auto_record_options);
#if defined(Q_OS_WIN)
        if (!crash_dir.empty())
            exosnap::crash_capture::MarkCleanExit(crash_dir);
        exosnap::crash_capture::Shutdown();
#endif
        return rc;
    }
#endif

    exosnap::MainWindow win;
    win.applyStartupRelaunchHandoff(startup_handoff.page_name, startup_handoff.reenable_present_diag);
    // ADR 0055: verification reinstall, armed from argv for this run only. Nothing
    // is written to settings, so a plain restart drops back to normal behavior.
    win.applyVerifyUpdateReinstallMode(
        exosnap::services::HasVerifyUpdateReinstallRequest(QCoreApplication::arguments()));
    if (!app_icon.isNull()) {
        win.setWindowIcon(app_icon);
        const QList<QSize> sizes = win.windowIcon().availableSizes();
        if (sizes.isEmpty()) {
            qWarning().noquote() << "MainWindow icon set, but availableSizes() is empty.";
        } else {
            qDebug().noquote() << "MainWindow icon sizes:" << FormatIconSizes(sizes);
        }
    }
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_test_requested) {
        const int visual_rc = exosnap::visual::RunVisualTest(app, win, visual_options);
#if defined(Q_OS_WIN)
        if (!crash_dir.empty())
            exosnap::crash_capture::MarkCleanExit(crash_dir);
        exosnap::crash_capture::Shutdown();
#endif
        return visual_rc;
    }
#endif

#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    if (auto_record_requested && auto_record_options.enable_preview) {
        const int rc = exosnap::auto_record::RunAutoRecord(app, win, auto_record_options);
#if defined(Q_OS_WIN)
        if (!crash_dir.empty())
            exosnap::crash_capture::MarkCleanExit(crash_dir);
        exosnap::crash_capture::Shutdown();
#endif
        return rc;
    }
#endif

#if defined(EXOSNAP_ENABLE_HWND_AUDIT_HARNESS)
    if (hwnd_audit_requested) {
        const int rc = exosnap::hwnd_audit::RunHwndAudit(app, win);
#if defined(Q_OS_WIN)
        if (!crash_dir.empty())
            exosnap::crash_capture::MarkCleanExit(crash_dir);
        exosnap::crash_capture::Shutdown();
#endif
        return rc;
    }
#endif

    win.resize(1120, 700); // matches MainWindow's minimum size (1120×700)
    win.show();
    QTimer::singleShot(0, &win, [&win, app_icon]() {
        if (!app_icon.isNull()) {
            win.setWindowIcon(app_icon);
            if (win.windowHandle() != nullptr) {
                win.windowHandle()->setIcon(app_icon);
            } else {
                qWarning().noquote() << "MainWindow windowHandle unavailable while applying runtime icon post-show.";
            }
        } else {
            qWarning().noquote() << "Post-show runtime icon apply skipped because app icon is null.";
        }

#if defined(Q_OS_WIN)
        ApplyNativeWindowIcons(win);
#endif
    });

    const int rc = app.exec();

    // Detach the sink before AppLog is torn down: a late engine record must not reach it.
    exosnap::ShutdownEngineLogging();

#if defined(Q_OS_WIN)
    // Normal shutdown: mark a clean exit so the next launch does not show the
    // crash dialog, then flush + shut down the crash engine.
    if (!crash_dir.empty())
        exosnap::crash_capture::MarkCleanExit(crash_dir);
    exosnap::crash_capture::Shutdown();

    if (win.elevatedRelaunchRequested()) {
        // ELEVATION-FOUNDATION-R1 (ADR 0033): relaunch elevated via ShellExecuteEx
        // ("runas", UAC). Reuse the same single-instance mutex release so the new
        // elevated process can acquire it. A UAC decline (UserDeclined) is a
        // normal, graceful outcome — stay non-elevated, no retry loop.
        if (hMutex != nullptr) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            hMutex = nullptr;
        }
        const exosnap::services::RelaunchResult relaunch_result =
            exosnap::services::RelaunchAsAdmin(QApplication::applicationFilePath(), win.elevatedRelaunchArgs());
        if (relaunch_result == exosnap::services::RelaunchResult::UserDeclined) {
            qInfo().noquote() << "Elevated relaunch cancelled by user (UAC declined).";
        } else if (relaunch_result == exosnap::services::RelaunchResult::Failed) {
            qWarning().noquote() << "Elevated relaunch failed (ShellExecuteEx).";
        }
    }
#endif

    return rc;
}
