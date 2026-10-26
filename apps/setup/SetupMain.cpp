#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <shellapi.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QUrl>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "QuickThemeTokens.h"
#include "SetupBridge.h"
#include "SetupController.h"
#include "i18n/Language.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

using namespace exosnap::setup;

QString RuntimeDirectory(const SetupHost* host) {
    if (host && host->runtime_path)
        return QString::fromWCharArray(host->runtime_path);
    std::array<wchar_t, 32768> path{};
    const DWORD length =
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path.data(), static_cast<DWORD>(path.size()));
    return QFileInfo(QString::fromWCharArray(path.data(), static_cast<int>(length))).absolutePath();
}

bool Preview(SetupController& controller, const QString& name) {
    const QStringList names{QStringLiteral("install"),        QStringLiteral("manage"),   QStringLiteral("uninstall"),
                            QStringLiteral("uninstall-data"), QStringLiteral("progress"), QStringLiteral("protected"),
                            QStringLiteral("success"),        QStringLiteral("reboot"),   QStringLiteral("failure"),
                            QStringLiteral("removed"),        QStringLiteral("cancelled")};
    if (!names.contains(name))
        return false;
    SetupEvent event{SetupEventKind::Detected};
    event.installed = name == QStringLiteral("manage") || name.startsWith(QStringLiteral("uninstall")) ||
                      name == QStringLiteral("removed");
    event.version = event.installed ? L"0.11.0" : nullptr;
    controller.receive(event);
    if (name == QStringLiteral("manage") || name == QStringLiteral("install"))
        return true;
    if (name.startsWith(QStringLiteral("uninstall"))) {
        controller.chooseUninstall();
        controller.setRemoveUserData(name.endsWith(QStringLiteral("data")));
        return true;
    }
    if (name == QStringLiteral("removed")) {
        controller.chooseUninstall();
        controller.setRemoveUserData(true);
    }
    controller.activatePrimary();
    event = SetupEvent{SetupEventKind::Applying};
    event.can_cancel = name == QStringLiteral("progress");
    controller.receive(event);
    event = SetupEvent{SetupEventKind::Progress};
    event.progress = name == QStringLiteral("protected") ? 96 : 48;
    event.can_cancel = name == QStringLiteral("progress");
    controller.receive(event);
    if (name == QStringLiteral("progress") || name == QStringLiteral("protected"))
        return true;
    if (name == QStringLiteral("failure")) {
        event = SetupEvent{SetupEventKind::Error};
        event.text = L"Windows Installer stopped with error 1603.";
        controller.receive(event);
    }
    event = SetupEvent{SetupEventKind::Completed};
    event.restart_required = name == QStringLiteral("reboot");
    event.status = name == QStringLiteral("failure") ? 1603 : name == QStringLiteral("cancelled") ? 1602 : 0;
    controller.receive(event);
    return true;
}

void ReceiveEvent(void* context, const SetupEvent* event) {
    auto* controller = static_cast<SetupController*>(context);
    const SetupEvent copy = *event;
    const QString text = QString::fromWCharArray(event->text ? event->text : L"");
    const QString version = QString::fromWCharArray(event->version ? event->version : L"");
    QMetaObject::invokeMethod(
        controller,
        [controller, copy, text, version] {
            SetupEvent delivered = copy;
            delivered.text = reinterpret_cast<const wchar_t*>(text.utf16());
            delivered.version = reinterpret_cast<const wchar_t*>(version.utf16());
            controller->receive(delivered);
        },
        Qt::QueuedConnection);
}

} // namespace

extern "C" __declspec(dllexport) std::uint32_t RunSetupUi(const exosnap::setup::SetupHost* host) {
    using namespace exosnap::setup;
    if (!host) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#if defined(_DEBUG)
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    }
    const QString runtime = RuntimeDirectory(host);
    QCoreApplication::setLibraryPaths({runtime});
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    int argument_count = 0;
    LPWSTR* native_args = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!native_args)
        return ERROR_INVALID_PARAMETER;
    std::vector<QByteArray> arguments;
    arguments.reserve(static_cast<std::size_t>(argument_count));
    for (int index = 0; index < argument_count; ++index)
        arguments.push_back(QString::fromWCharArray(native_args[index]).toLocal8Bit());
    LocalFree(native_args);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (QByteArray& arg : arguments)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    QGuiApplication app(argument_count, argv.data());
    QCoreApplication::setApplicationName(QStringLiteral("ExoSnap Setup"));
    QCoreApplication::setOrganizationName(QStringLiteral("ExoSnap"));

    for (const QString& font : QDir(QStringLiteral(":/fonts")).entryList({QStringLiteral("*.ttf")}, QDir::Files))
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + font);

    const QStringList args = app.arguments();
    const auto option = [&args](const QString& key) {
        const int index = args.indexOf(key);
        return index >= 0 && index + 1 < args.size() ? args.at(index + 1) : QString{};
    };
    const QString preview = host ? QString{} : option(QStringLiteral("--preview-state"));
    const bool lifecycle = !host && preview == QStringLiteral("lifecycle");
    const QString screenshot = host ? QString{} : option(QStringLiteral("--screenshot"));
    exosnap::i18n::InstallLanguage(app, host ? QStringLiteral("system") : option(QStringLiteral("--ui-language")));
    if (!host && preview.isEmpty())
        return ERROR_INVALID_PARAMETER;

    SetupController controller(host ? QString::fromWCharArray(host->version) : QStringLiteral("0.11.0"),
                               QDir::toNativeSeparators(host ? QString::fromWCharArray(host->install_path)
                                                             : QStringLiteral("C:/Program Files/ExoSnap")),
                               host && host->passive, host && host->uninstall_requested,
                               host && host->repair_requested);
    QFile license(QStringLiteral(":/setup/LICENSE"));
    if (!license.open(QIODevice::ReadOnly))
        return ERROR_FILE_NOT_FOUND;
    controller.setLicenseText(QString::fromUtf8(license.readAll()));
    if (!host && !lifecycle && !Preview(controller, preview))
        return ERROR_INVALID_PARAMETER;

    QQmlApplicationEngine engine;
    engine.addImportPath(runtime + QStringLiteral("/qml"));
    engine.setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&controller)},
         {QStringLiteral("detailsPreview"), !host && args.contains(QStringLiteral("--preview-details"))}});
    engine.loadFromModule(QStringLiteral("ExoSnap.Setup"), QStringLiteral("SetupMain"));
    if (engine.rootObjects().isEmpty())
        return ERROR_INVALID_DATA;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (!window)
        return ERROR_INVALID_DATA;
    if (auto* tokens = engine.singletonInstance<exosnap::quick::QuickThemeTokens*>(QStringLiteral("ExoSnap.Setup"),
                                                                                   QStringLiteral("QuickThemeTokens")))
        tokens->setAppearance(QStringLiteral("dark"), QStringLiteral("aqua"));
    QObject::connect(&controller, &SetupController::finished, &app,
                     [&app](std::uint32_t code) { app.exit(static_cast<int>(code)); });
    if (host) {
        QObject::connect(&controller, &SetupController::applyRequested, &app,
                         [host, window](SetupAction action, bool shortcut, bool remove) {
                             host->apply(host->context, action, shortcut, remove, window->winId());
                         });
        QObject::connect(&controller, &SetupController::cancelRequested, &app, [host] { host->cancel(host->context); });
        QObject::connect(&controller, &SetupController::launchRequested, &app,
                         [host, window] { host->launch(host->context, window->winId()); });
        QObject::connect(&controller, &SetupController::restartRequested, &app, [host, &app] {
            host->restart(host->context);
            app.exit(3010);
        });
        QObject::connect(&controller, &SetupController::openLogRequested, &app, [host] {
            if (host->log_path && *host->log_path)
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromWCharArray(host->log_path)));
        });
        host->subscribe(host->context, ReceiveEvent, &controller);
        QTimer::singleShot(0, &app, [host] { host->detect(host->context); });
    }
    if (QScreen* screen = QGuiApplication::primaryScreen())
        window->setPosition(screen->availableGeometry().center() - QPoint(window->width() / 2, window->height() / 2));
    if (lifecycle) {
        window->setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
        QRect desktop;
        for (QScreen* screen : QGuiApplication::screens())
            desktop = desktop.united(screen->geometry());
        window->setPosition(desktop.topLeft() - QPoint(1000, 1000));
    }
    window->show();
    if (lifecycle) {
        const auto geometry = [window](const char* stage) {
            std::fprintf(stdout, "setup-lifecycle %s %dx%d min=%d max=%d focused=%d\n", stage, window->width(),
                         window->height(), window->minimumHeight(), window->maximumHeight(),
                         GetForegroundWindow() == reinterpret_cast<HWND>(window->winId()) ? 1 : 0);
            std::fflush(stdout);
        };
        QTimer::singleShot(100, &app, [&controller] { (void)Preview(controller, QStringLiteral("install")); });
        QTimer::singleShot(250, &app, [geometry, &controller, window, screenshot, &app] {
            geometry("decision");
            if (!screenshot.isEmpty() && !window->grabWindow().save(screenshot)) {
                app.exit(ERROR_WRITE_FAULT);
                return;
            }
            controller.activatePrimary();
        });
        QTimer::singleShot(400, &app, [geometry, &controller] {
            geometry("applying");
            controller.receive(SetupEvent{SetupEventKind::Completed});
        });
        QTimer::singleShot(550, &app, [geometry, &app] {
            geometry("success");
            app.quit();
        });
    }
    if (!screenshot.isEmpty() && !lifecycle) {
        QTimer::singleShot(300, window, [window, screenshot, &app] {
            app.exit(window->grabWindow().save(screenshot) ? 0 : ERROR_WRITE_FAULT);
        });
    }
    if (!host && args.contains(QStringLiteral("--preview-smoke")))
        QTimer::singleShot(1500, &app, &QCoreApplication::quit);
    const int result = app.exec();
    if (host)
        host->subscribe(host->context, nullptr, nullptr);
    return static_cast<std::uint32_t>(result);
}
