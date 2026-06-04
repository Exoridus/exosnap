#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QWindow>

#include "MainWindow.h"
#include "exosnap_resource.h"
#include "ui/theme/ExoSnapTheme.h"

#if defined(Q_OS_WIN)
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

#if defined(Q_OS_WIN)
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName);
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
#endif

    exosnap::MainWindow win;
    if (!app_icon.isNull()) {
        win.setWindowIcon(app_icon);
        const QList<QSize> sizes = win.windowIcon().availableSizes();
        if (sizes.isEmpty()) {
            qWarning().noquote() << "MainWindow icon set, but availableSizes() is empty.";
        } else {
            qDebug().noquote() << "MainWindow icon sizes:" << FormatIconSizes(sizes);
        }
    }
    win.resize(1100, 700);
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

    return app.exec();
}
