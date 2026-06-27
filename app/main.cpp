#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QProcess>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QWindow>

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion
#include "MainWindow.h"
#include "exosnap_resource.h"
#include "services/ElevatedRelaunch.h"
#include "ui/theme/ExoSnapTheme.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "visual_tests/VisualTestHarness.h"
#endif

#if defined(Q_OS_WIN)
#include <crash_capture/crash_capture.h>

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
    QApplication app(argc, argv);
    app.setApplicationName("ExoSnap");

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

#if defined(Q_OS_WIN)
    HANDLE hMutex = nullptr;
    if (!visual_test_requested) {
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

    exosnap::MainWindow win;
    win.applyStartupRelaunchHandoff(startup_handoff.page_name, startup_handoff.reenable_present_diag);
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

#if defined(Q_OS_WIN)
    // Normal shutdown: mark a clean exit so the next launch does not show the
    // crash dialog, then flush + shut down the crash engine.
    if (!crash_dir.empty())
        exosnap::crash_capture::MarkCleanExit(crash_dir);
    exosnap::crash_capture::Shutdown();

    // Relaunch path (dialog "Restart ExoSnap"): MarkCleanExit + Shutdown already
    // ran above, so the relaunched instance is quiet. Release the single-instance
    // mutex so the new process can acquire it, then spawn a detached copy.
    if (win.relaunchRequested()) {
        if (hMutex != nullptr) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
            hMutex = nullptr;
        }
        QProcess::startDetached(QApplication::applicationFilePath(), {});
    } else if (win.elevatedRelaunchRequested()) {
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
