#include "QuickApplication.h"
#include "QuickLiveVerifySource.h"
#include "QuickWindowGeometry.h"
#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
#include "NotificationsAdapter.h"
#include "PseudoLocalization.h"
#include "QuickAutoEditHarness.h"
#include "QuickAutoRecordHarness.h"
#include "auto_record/AutoRecordHarness.h"
#endif
#include "bootstrap/ProductionBootstrap.h"
#include "diagnostics/NativeWindowFacts.h"
#include "diagnostics/StartupClock.h"
#include "live_verify/LiveVerifyControlServer.h"
#include "live_verify/LiveVerifyOptions.h"
#include "services/ElevatedRelaunch.h"
#include "services/RecordingCoordinator.h"
#include "services/VerifyReinstallMode.h"

// QApplication, not QGuiApplication: QSystemTrayIcon is a Qt Widgets type and
// refuses to construct without a QApplication. That single dependency is the
// documented, intentional remainder of Qt6::Widgets in the Quick frontend (Qt
// offers no Quick-native tray icon, and a hand-rolled Shell_NotifyIcon is
// post-1.0). Everything the application draws is still Qt Quick.
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include <capability/audio_ui_state.h>

#include <QFile>

#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

namespace {

QString visualOutputPath(const QStringList& arguments) {
    const int option_index = arguments.indexOf(QStringLiteral("--visual-test"));
    if (option_index < 0 || option_index + 1 >= arguments.size())
        return {};
    return arguments.at(option_index + 1);
}

QSize visualWindowSize(const QStringList& arguments) {
    const int option_index = arguments.indexOf(QStringLiteral("--visual-test-size"));
    if (option_index < 0 || option_index + 1 >= arguments.size())
        return {};
    const QStringList dimensions = arguments.at(option_index + 1).split(QLatin1Char('x'));
    if (dimensions.size() != 2)
        return {};
    bool width_ok = false;
    bool height_ok = false;
    const int width = dimensions.at(0).toInt(&width_ok);
    const int height = dimensions.at(1).toInt(&height_ok);
    return width_ok && height_ok && width > 0 && height > 0 ? QSize(width, height) : QSize{};
}

// Delay before a --visual-test capture fires. The default is enough for a page
// whose content is ready at load, but areas that hydrate asynchronously (the
// Device adapter scan, the Diagnostics self-test) would otherwise be
// photographed mid-flight on a slower machine — the capture would silently show
// a "Scanning…" state and still exit 0. Reviewers need that wait to be a stated
// number, not a coincidence, so it is an explicit option.
int visualCaptureDelayMs(const QStringList& arguments) {
    constexpr int kDefaultCaptureDelayMs = 400;
    // Scrolling to the end of Settings is not ready at 400 ms: the page is
    // loader-built, its two breakpoint compositions settle afterwards, and the
    // pinning timer only then has a content height to scroll to. Measured on
    // this tree, 400 ms photographs the TOP of the page — which is what the flag
    // did silently before, so nobody noticed the cards below the fold were never
    // in the sweep. An explicit --visual-delay-ms still wins over this.
    constexpr int kSettingsBottomCaptureDelayMs = 2000;
    const int default_delay_ms = arguments.contains(QStringLiteral("--settings-visual-bottom"))
                                     ? kSettingsBottomCaptureDelayMs
                                     : kDefaultCaptureDelayMs;
    const int option_index = arguments.indexOf(QStringLiteral("--visual-delay-ms"));
    if (option_index < 0 || option_index + 1 >= arguments.size())
        return default_delay_ms;
    bool delay_ok = false;
    const int delay_ms = arguments.at(option_index + 1).toInt(&delay_ok);
    return delay_ok && delay_ms >= 0 ? delay_ms : default_delay_ms;
}

// Scratch config directory a harness run is isolated into. Distinct per harness
// family so a visual capture and an HWND audit cannot leave each other's
// synthetic settings behind, matching the Widgets entry point's three dirs.
QString harnessConfigId(const QStringList& arguments) {
    if (arguments.contains(QStringLiteral("--visual-test")))
        return QStringLiteral("quick-visual-test");
    if (arguments.contains(QStringLiteral("--hwnd-audit")))
        return QStringLiteral("quick-hwnd-audit");
    if (arguments.contains(QStringLiteral("--auto-record")))
        return QStringLiteral("quick-auto-record");
    if (arguments.contains(QStringLiteral("--auto-edit")))
        return QStringLiteral("quick-auto-edit");
    return QStringLiteral("quick-harness");
}

QString optionValue(const QStringList& arguments, const QString& option) {
    const int index = arguments.indexOf(option);
    return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString{};
}

quint64 processCpuTime100ns() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
        return 0;
    const auto value = [](FILETIME time) {
        return (static_cast<quint64>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    };
    return value(kernel) + value(user);
}

QString defaultAdapterName() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->EnumAdapters1(0, adapter.GetAddressOf()))) {
        return QStringLiteral("Unavailable");
    }
    DXGI_ADAPTER_DESC1 description{};
    return SUCCEEDED(adapter->GetDesc1(&description)) ? QString::fromWCharArray(description.Description)
                                                      : QStringLiteral("Unavailable");
}

// Both facts the audit below reports come from diagnostics/NativeWindowFacts,
// shared with the Live Verify window/overlay snapshots. Two readers, one
// implementation: --hwnd-audit is the startup gate and the snapshot is
// observation during a live run, and the two must never disagree about what
// "no non-client area" or "no child HWNDs" means.
using exosnap::diagnostics::CountChildWindows;
using exosnap::diagnostics::NonClientInset;
using exosnap::diagnostics::QueryNonClientInset;

int childWindowCount(HWND root) {
    return CountChildWindows(root);
}

NonClientInset nonClientInset(HWND hwnd) {
    return QueryNonClientInset(hwnd);
}

LRESULT CALLBACK previewPatternWindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_TIMER) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const LONG phase = static_cast<LONG>((GetTickCount64() / 8) % 320);
        const LONG inverse_phase = 320 - phase;
        RECT red{30 + phase, 30, 250 + phase, 250};
        RECT blue{130 + inverse_phase, 30, 350 + inverse_phase, 250};
        RECT gold{30, 300 + (phase / 4), 680, 420 + (phase / 4)};
        HBRUSH red_brush = CreateSolidBrush(RGB(220, 20, 60));
        HBRUSH blue_brush = CreateSolidBrush(RGB(30, 144, 255));
        HBRUSH gold_brush = CreateSolidBrush(RGB(255, 210, 40));
        FillRect(dc, &red, red_brush);
        FillRect(dc, &blue, blue_brush);
        FillRect(dc, &gold, gold_brush);
        DeleteObject(red_brush);
        DeleteObject(blue_brush);
        DeleteObject(gold_brush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT label{30, 515, 700, 580};
        DrawTextW(dc, L"HIGH-MOTION REAL DESKTOP CONTENT", -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w_param, l_param);
}

HWND createPreviewPatternWindow() {
    constexpr wchar_t class_name[] = L"ExoSnapQuickPreviewPattern";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = previewPatternWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);
    HWND pattern = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, class_name, L"ExoSnap preview input pattern",
                                   WS_POPUP, 1320, 180, 730, 620, nullptr, nullptr, window_class.hInstance, nullptr);
    if (pattern != nullptr) {
        SetTimer(pattern, 1, 8, nullptr);
        ShowWindow(pattern, SW_SHOWNOACTIVATE);
        UpdateWindow(pattern);
    }
    return pattern;
}

// Grabs every visible capture-excluded overlay window alongside the main
// --visual-test screenshot, one file per overlay, named after its objectName:
//
//     shell.png  ->  shell.quickOverlayRecording.png
//
// Written next to the main capture rather than into it: the overlays live on the
// recorded monitor at their own positions and sizes, and compositing them into
// the shell shot would fabricate a layout that never appears.
//
// Failure is silent on purpose. This is supplementary evidence; a scenario with
// no overlay on screen must not turn the shell capture's exit code red.
void saveOverlayWindowGrabs(const QString& screenshot_path) {
    const int dot = screenshot_path.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? screenshot_path.left(dot) : screenshot_path;
    const QString suffix = dot > 0 ? screenshot_path.mid(dot) : QStringLiteral(".png");

    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window == nullptr || !window->isVisible())
            continue;
        if (!window->objectName().startsWith(QLatin1String("quickOverlay")))
            continue;
        auto* quick_window = qobject_cast<QQuickWindow*>(window);
        if (quick_window == nullptr)
            continue;
        (void)quick_window->grabWindow().save(stem + QLatin1Char('.') + window->objectName() + suffix);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Must be the first statement: the DLL-search hardening has to precede any
    // LoadLibrary the Qt platform plugin triggers, and the interrupted-swap
    // repair has to precede anything that resolves a path inside the install
    // directory. See bootstrap/ProductionBootstrap.h.
    const exosnap::bootstrap::PreAppResult pre_app = exosnap::bootstrap::RunPreApplicationPhase();

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QApplication app(argc, argv);
    // quitOnLastWindowClosed is deliberately left at Qt's default. Close-to-tray
    // REFUSES the close (requestClose() returns false) and only then hides the
    // window, which is structurally the same thing the Widgets shell does with
    // event->ignore() + hide(): a refused close never emits lastWindowClosed, so
    // hiding to the tray cannot quit the process out from under a recording.

    // The Qt Quick Controls style, pinned before a single line of QML loads.
    //
    // Without this call the style is whatever the platform default resolves to
    // — on Windows that is the Windows style, which in turn falls back to
    // Fusion for the controls it does not implement, while the files that
    // import QtQuick.Controls.Basic explicitly stay Basic. One binary would
    // then draw its controls from three different styles, and a Qt update
    // changing the platform default would silently restyle the application.
    //
    // Basic is the choice because that is what the ExoSnap design system is
    // built on: every visual decision lives in the Exo* components and the
    // theme tokens, so the style underneath must contribute as little of its
    // own opinion as possible.
    //
    // QQuickStyle::setStyle() outranks -style, QT_QUICK_CONTROLS_STYLE and
    // qtquickcontrols2.conf, so neither the environment nor a command line can
    // take the application somewhere else.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const exosnap::bootstrap::PostAppResult post_app = exosnap::bootstrap::MarkApplicationConstructed();
    exosnap::bootstrap::ApplyApplicationMetadata();

    const QStringList arguments = QCoreApplication::arguments();

    // The Live Verify control channel. Explicit argv opt-in and nothing else: no
    // Debug default, no environment variable, no "looks like a developer
    // machine" heuristic. A malformed option is fatal rather than ignored — a
    // runner that believes it armed the channel and silently got a normal
    // application would report acceptance for a process it never drove.
    //
    // Deliberately NOT part of `diagnostic_mode` below: a verification launch
    // must be the real application, with the real single-instance guard, the
    // real tray and the real configuration directory. The runner isolates
    // configuration by setting EXOSNAP_CONFIG_DIR itself when a check calls for
    // it, so which profile is in force stays the check's decision.
    const exosnap::live_verify::ControlOptions live_verify_options =
        exosnap::live_verify::ParseControlOptions(arguments);
    if (live_verify_options.requested && !live_verify_options.error.isEmpty()) {
        qCritical().noquote() << live_verify_options.error;
        return 2;
    }

    const bool preview_mode = arguments.contains(QStringLiteral("--preview-smoke-test")) ||
                              arguments.contains(QStringLiteral("--preview-lifecycle-test")) ||
                              arguments.contains(QStringLiteral("--preview-visual-test")) ||
                              arguments.contains(QStringLiteral("--preview-benchmark")) ||
                              arguments.contains(QStringLiteral("--still-frame-validation")) ||
                              arguments.contains(QStringLiteral("--target-refresh-validation")) ||
                              arguments.contains(QStringLiteral("--hwnd-audit"));
#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    // The frontend A/B benchmark. Unlike every other harness above, the window
    // must be VISIBLE and normally composited — measuring an off-screen or
    // minimised frontend would measure nothing the user ever pays for — so it
    // only joins the config-isolation/single-instance exemptions, not the
    // off-screen placement.
    const bool auto_record_requested = exosnap::auto_record::HasAutoRecordRequest(arguments);
    exosnap::auto_record::AutoRecordOptions auto_record_options;
    if (auto_record_requested) {
        QString parse_error;
        if (!exosnap::auto_record::ParseAutoRecordOptions(arguments, &auto_record_options, &parse_error)) {
            qCritical().noquote() << parse_error;
            return 2;
        }
    }
    // The Edit -> Export half of the product flow. Parsed alongside the recording
    // options so a malformed value is rejected before anything records for a
    // minute and only then discovers it has nowhere to export to.
    const bool auto_edit_requested = exosnap::quick::AutoEditRequested(arguments);
    exosnap::quick::AutoEditOptions auto_edit_options;
    if (auto_edit_requested) {
        QString parse_error;
        if (!exosnap::quick::ParseAutoEditOptions(arguments, &auto_edit_options, &parse_error)) {
            qCritical().noquote() << parse_error;
            return 2;
        }
    }
#else
    constexpr bool auto_record_requested = false;
    constexpr bool auto_edit_requested = false;
#endif
    const bool diagnostic_mode = preview_mode || auto_record_requested || auto_edit_requested ||
                                 arguments.contains(QStringLiteral("--smoke-test")) ||
                                 arguments.contains(QStringLiteral("--visual-test"));

    // A harness runs the *real* application, and the real application persists
    // its live configuration while it runs. Aimed at the developer's own config
    // directory it overwrites their settings with the scenario's synthetic ones,
    // so isolation is unconditional rather than an opt-in variable — forgetting
    // the variable is exactly the mistake that destroys data. Must happen before
    // QuickApplication is constructed: its stores read on construction.
    if (diagnostic_mode)
        (void)exosnap::bootstrap::IsolateHarnessConfigDir(harnessConfigId(arguments));

    // Unconditional, and deliberately not gated on `diagnostic_mode`: the case
    // that matters most is a caller — the release packaging smoke, CI — that set
    // EXOSNAP_CONFIG_DIR itself and expects the launch to leave the real user's
    // tree alone. The QML engine's cache is the one thing EXOSNAP_CONFIG_DIR
    // does not reach on its own. Must precede QuickApplication, which owns the
    // engine.
    (void)exosnap::bootstrap::AlignQmlDiskCacheWithConfigDir();

    exosnap::bootstrap::BootstrapOptions bootstrap_options;
    // Every harness stays out of the single-instance guard: beyond a second
    // instance exiting immediately, the guard calls SetForegroundWindow on the
    // running window, which would yank focus off whatever the developer is doing.
    bootstrap_options.suppress_single_instance = diagnostic_mode;
    exosnap::bootstrap::ProductionBootstrap bootstrap(bootstrap_options);
    bootstrap.InitializeLogging(pre_app, post_app);

    if (bootstrap.AcquireSingleInstance() == exosnap::bootstrap::SingleInstanceOutcome::AlreadyRunning)
        return 0;

    bootstrap.InitializeCrashCapture();
    if (bootstrap.RunSentryTestEventIfRequested() == exosnap::bootstrap::SentryTestEventOutcome::SentExitNow)
        return 0;

    const QIcon app_icon = exosnap::bootstrap::InstallApplicationIcon();

    // Startup-geometry measurement seam. Must be armed before load(), which is
    // where the persisted rect is resolved and the window is created. Also
    // available as EXOSNAP_WINDOW_TRACE=1 so a launch that cannot take extra argv
    // (a shortcut, the updater's relaunch) can still be traced.
    if (arguments.contains(QStringLiteral("--window-trace"))) {
        exosnap::quick::SetWindowGeometryTraceEnabled(true);
        // Before the window exists: the messages that decide its first rect are
        // sent during creation, so a filter installed afterwards would miss the
        // only part of the sequence worth measuring.
        exosnap::quick::InstallStartupMessageTrace();
    }

    // Harness-only: controlled text expansion for the 860x700 layout regression
    // pass (QCR-511). Installed BEFORE the engine loads, so every qsTr() in QML
    // resolves through it on its first evaluation and no retranslate() call is
    // needed. argv-only by design — see PseudoLocalization.h.
    if (arguments.contains(QStringLiteral("--pseudo-localize"))) {
        auto* pseudo = new exosnap::quick::PseudoLocalizationTranslator(&app);
        QCoreApplication::installTranslator(pseudo);
    }

    exosnap::quick::QuickApplication quick_application;
    // ADR 0033: the handoff a prior elevated self-relaunch put in our own argv.
    // Applied before load() so the shell's landing page is decided once, rather
    // than navigating away from Record after the first frame. ADR 0055: the
    // verification-reinstall arming is argv-only and never persisted.
    const exosnap::services::RelaunchHandoff startup_handoff = exosnap::services::ParseRelaunchArgs(arguments);
    quick_application.applyStartupRelaunchHandoff(startup_handoff.page_name, startup_handoff.reenable_present_diag);
    quick_application.applyVerifyUpdateReinstallMode(exosnap::services::HasVerifyUpdateReinstallRequest(arguments));
    // A --visual-test sweep is one process per scenario and a benchmark run
    // measures the frontend, not the notification area; neither should drop an
    // ExoSnap icon into the developer's tray. --smoke-test is deliberately NOT in
    // this list: it is what proves the tray still constructs and tears down
    // inside the real application.
    quick_application.applyTraySuppression(preview_mode || auto_record_requested || auto_edit_requested ||
                                           arguments.contains(QStringLiteral("--visual-test")));

    if (!quick_application.load(diagnostic_mode))
        return -1;

    const auto roots = quick_application.engine().rootObjects();
    auto* root_window = roots.isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(roots.constFirst());

    if (root_window != nullptr) {
        // The Qt-level window icon only covers what Qt itself draws; the taskbar
        // and Alt-Tab read the native ICON_SMALL/ICON_BIG slots, which stay empty
        // unless WM_SETICON is sent against the real HWND.
        if (!app_icon.isNull())
            root_window->setIcon(app_icon);
        exosnap::bootstrap::ApplyNativeWindowIcons(reinterpret_cast<void*>(root_window->winId()));

        // frameSwapped fires on the render thread; the milestone is recorded on
        // the GUI thread via the queued connection so AppLog and StartupTrace are
        // only ever touched from one thread. The elapsed value is read in the
        // slot, so it carries a few hundred microseconds of queue latency — far
        // below the resolution this milestone is read at.
        QObject::connect(root_window, &QQuickWindow::frameSwapped, &app, [&bootstrap]() {
            static bool recorded = false;
            if (recorded)
                return;
            recorded = true;
            bootstrap.RecordStartupMilestone(QStringLiteral("first-paint"),
                                             exosnap::diagnostics::StartupClock().elapsed());
        });
    }

    // ---- Live Verify control channel ----------------------------------------
    // Declared after `quick_application` so it is destroyed BEFORE it: the pipe
    // worker hands requests to this thread and the source borrows the
    // application's adapters, so the endpoint has to be gone before any of that
    // is torn down. Both stay null on a normal launch — no pipe, no thread, no
    // log line.
    std::unique_ptr<exosnap::quick::QuickLiveVerifySource> live_verify_source;
    std::unique_ptr<exosnap::live_verify::LiveVerifyControlServer> live_verify_server;
    if (live_verify_options.requested) {
        live_verify_source = std::make_unique<exosnap::quick::QuickLiveVerifySource>(quick_application, root_window);
        live_verify_server = std::make_unique<exosnap::live_verify::LiveVerifyControlServer>(
            live_verify_source.get(), live_verify_options.run_id);
        QString control_error;
        if (!live_verify_server->Start(&control_error)) {
            qCritical().noquote() << QStringLiteral("Live Verify control endpoint unavailable: %1").arg(control_error);
            return 3;
        }

        auto* server = live_verify_server.get();
        auto* source = live_verify_source.get();

        // Events, not polling. Each one reuses an existing signal; none of them
        // introduces a second idea of the state it reports.
        if (auto* record = quick_application.recordViewModelAdapter()) {
            auto last_state = std::make_shared<int>(record->state());
            QObject::connect(record, &exosnap::quick::RecordViewModelAdapter::changed, server,
                             [server, source, record, last_state]() {
                                 if (record->state() == *last_state)
                                     return;
                                 *last_state = record->state();
                                 server->EmitEvent(QStringLiteral("record.stateChanged"), source->RecordSnapshot());
                                 // Completed/Failed are the two states in which a
                                 // result exists to read. Emitted from the same
                                 // transition rather than from a second callback,
                                 // because the coordinator's result-callback slot
                                 // is single-occupancy and already owned.
                                 const bool terminal =
                                     record->state() == static_cast<int>(exosnap::UiRecordingState::Completed) ||
                                     record->state() == static_cast<int>(exosnap::UiRecordingState::Failed);
                                 if (terminal)
                                     server->EmitEvent(QStringLiteral("record.resultReady"), source->RecordResult());
                             });
        }
        if (root_window != nullptr) {
            QObject::connect(root_window, &QQuickWindow::screenChanged, server, [server, source](QScreen*) {
                server->EmitEvent(QStringLiteral("window.screenChanged"), source->WindowSnapshot());
            });
        }
        // Queued so it lands after the event loop starts and the first frame is
        // on its way; a client connecting later simply reads the snapshots.
        QTimer::singleShot(
            0, server, [server, source]() { server->EmitEvent(QStringLiteral("app.ready"), source->AppSnapshot()); });
    }

    const HWND preview_pattern =
        arguments.contains(QStringLiteral("--desktop-pattern")) ? createPreviewPatternWindow() : nullptr;
    if (preview_pattern != nullptr) {
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                         [preview_pattern]() { DestroyWindow(preview_pattern); });
    }
    const QSize requested_size = visualWindowSize(arguments);
    if (requested_size.isValid())
        quick_application.applyHarnessWindowSize(requested_size);

    // Harness-only: selects which navigation destination a --visual-test capture
    // renders. Nav indices follow the canonical product order (Record, Settings,
    // Diagnostics, Logs, About).
    const QString visual_page = optionValue(arguments, QStringLiteral("--visual-page"));
    if (!visual_page.isEmpty()) {
        bool page_ok = false;
        const int page_index = visual_page.toInt(&page_ok);
        if (page_ok) {
            if (auto* shell = root_window != nullptr ? root_window->findChild<QObject*>(QStringLiteral("quickAppShell"))
                                                     : nullptr) {
                shell->setProperty("currentPage", page_index);
            }
        }
    }

    // Harness-only: opens one of the popups that is now built on first use
    // instead of at page load (QCR-601, QCR-610), so a --visual-test capture can
    // photograph it. Without this the two surfaces would only be reachable by
    // driving the running application, which is exactly what the harness exists
    // to avoid. Queued, so the shell has finished its first layout — the picker
    // measures itself against the overlay it centres in.
    const QString visual_popup = optionValue(arguments, QStringLiteral("--visual-popup"));
    if (!visual_popup.isEmpty()) {
        QTimer::singleShot(0, &app, [root_window, &quick_application, visual_popup]() {
            if (visual_popup == QLatin1String("source-picker")) {
                if (auto* page = root_window != nullptr
                                     ? root_window->findChild<QObject*>(QStringLiteral("quickRecordPage"))
                                     : nullptr) {
                    QMetaObject::invokeMethod(page, "openSourcePicker");
                }
            } else if (visual_popup == QLatin1String("notification-hub")) {
                if (auto* notifications = quick_application.notificationsAdapter())
                    notifications->openHub();
            }
        });
    }

    // Harness-only: renders a --visual-test capture in the named appearance and
    // accent.
    //
    // Driven through the settings adapter rather than straight into the token
    // singleton, so the Appearance card's own controls agree with the colours
    // the capture shows — a screenshot in Light whose segmented control reads
    // "Dark" is worse evidence than no screenshot.
    //
    // A capture WITHOUT these options is pinned to the product default rather
    // than left at whatever is in the scratch config dir. The harness config dir
    // is scratch, but it is not fresh: it survives between runs and the running
    // application persists its appearance into it, so a capture taken after a
    // `--visual-appearance light` run silently came out Light while claiming to
    // be the default. A screenshot whose colours depend on which capture ran
    // before it is not evidence of anything.
    const QString visual_appearance = optionValue(arguments, QStringLiteral("--visual-appearance"));
    const QString visual_accent = optionValue(arguments, QStringLiteral("--visual-accent"));
    if (!visualOutputPath(arguments).isEmpty()) {
        const QString appearance = visual_appearance.isEmpty() ? QStringLiteral("dark") : visual_appearance;
        const QString accent = visual_accent.isEmpty() ? QStringLiteral("aqua") : visual_accent;
        if (auto* settings = quick_application.settingsAdapter()) {
            settings->setAppearanceId(appearance);
            settings->setAccentId(accent);
        } else if (auto* tokens = quick_application.engine().singletonInstance<exosnap::quick::QuickThemeTokens*>(
                       QStringLiteral("ExoSnap.Quick"), QStringLiteral("QuickThemeTokens"))) {
            tokens->setAppearance(appearance, accent);
        }
    }

    // Harness-only: scrolls the Settings page to its end so a capture can reach
    // the cards below the fold. Appearance is the last of them, and no window
    // height on a real display reaches it — the window is clamped to the screen
    // work area long before the content ends.
    // Queued, not immediate: the Settings page is built by the shell's loader in
    // response to --visual-page above, so at this point in startup it does not
    // exist yet. findChild() then returned nullptr and this did nothing at all,
    // silently -- every capture taken with the flag showed the top of the page
    // while claiming to show its end, and the cards below the fold (Appearance
    // among them) had never actually been photographed.
    if (arguments.contains(QStringLiteral("--settings-visual-bottom"))) {
        QTimer::singleShot(0, &app, [root_window]() {
            auto* page = root_window != nullptr ? root_window->findChild<QObject*>(QStringLiteral("quickSettingsPage"))
                                                : nullptr;
            if (page == nullptr) {
                qWarning("--settings-visual-bottom: no quickSettingsPage; is --visual-page set to Settings?");
                return;
            }
            QMetaObject::invokeMethod(page, "scrollToBottom");
        });
    }

    const QString record_visual_state = optionValue(arguments, QStringLiteral("--record-visual-state"));
    if (!record_visual_state.isEmpty()) {
        if (auto* shell =
                root_window != nullptr ? root_window->findChild<QObject*>(QStringLiteral("quickAppShell")) : nullptr) {
            shell->setProperty("currentPage", 0);
        }
        QTimer::singleShot(0, &app, [&quick_application, record_visual_state]() {
            (void)quick_application.applyRecordVisualScenario(record_visual_state);
        });
    }

    // Harness-only: seeds one of the runtime overlay surfaces (recovery,
    // recording error, crash report) so a --visual-test capture can photograph
    // it without a real crash or a real failed recording on this machine.
    const QString overlay_visual_state = optionValue(arguments, QStringLiteral("--overlay-visual-state"));
    if (!overlay_visual_state.isEmpty()) {
        QTimer::singleShot(0, &app, [&quick_application, overlay_visual_state]() {
            (void)quick_application.applyOverlayVisualScenario(overlay_visual_state);
        });
    }

    if (arguments.contains(QStringLiteral("--smoke-test")))
        QTimer::singleShot(150, &app, &QCoreApplication::quit);

    if (arguments.contains(QStringLiteral("--preview-smoke-test"))) {
        auto* deadline = new QTimer(&app);
        deadline->setInterval(50);
        int* attempts = new int(0);
        QObject::connect(deadline, &QTimer::timeout, &app, [&app, &quick_application, deadline, attempts]() {
            ++*attempts;
            if (quick_application.recordPreviewAdapter()->frameReady() || *attempts >= 120) {
                const bool ready = quick_application.recordPreviewAdapter()->frameReady();
                deadline->stop();
                delete attempts;
                app.exit(ready ? 0 : 3);
            }
        });
        deadline->start();
    }

    if (arguments.contains(QStringLiteral("--preview-lifecycle-test"))) {
        auto* shell =
            root_window != nullptr ? root_window->findChild<QObject*>(QStringLiteral("quickAppShell")) : nullptr;
        auto* lifecycle = new QTimer(&app);
        lifecycle->setInterval(50);
        int* state = new int(0);
        int* attempts = new int(0);
        QObject::connect(lifecycle, &QTimer::timeout, &app,
                         [&app, &quick_application, root_window, shell, lifecycle, state, attempts]() {
                             auto fail = [&app, lifecycle, state, attempts]() {
                                 lifecycle->stop();
                                 delete state;
                                 delete attempts;
                                 app.exit(5);
                             };
                             ++*attempts;
                             if (shell == nullptr || root_window == nullptr || *attempts > 300) {
                                 fail();
                                 return;
                             }
                             auto* adapter = quick_application.recordPreviewAdapter();
                             switch (*state) {
                             case 0:
                                 if (!adapter->frameReady())
                                     return;
                                 shell->setProperty("currentPage", 5);
                                 *state = 1;
                                 *attempts = 0;
                                 return;
                             case 1:
                                 if (adapter->active() || adapter->frameReady())
                                     return;
                                 shell->setProperty("currentPage", 0);
                                 *state = 2;
                                 *attempts = 0;
                                 return;
                             case 2:
                                 if (!adapter->frameReady())
                                     return;
                                 root_window->resize(root_window->width() + 96, root_window->height() + 54);
                                 root_window->showMinimized();
                                 *state = 3;
                                 *attempts = 0;
                                 return;
                             case 3:
                                 if (*attempts < 8)
                                     return;
                                 // Force destruction of the scene-graph-owned
                                 // D3D11 wrappers while the producer remains
                                 // alive. Restore must reopen the retained NT
                                 // handle instead of waiting for a new source.
                                 root_window->releaseResources();
                                 root_window->showNormal();
                                 root_window->requestActivate();
                                 *state = 4;
                                 *attempts = 0;
                                 return;
                             case 4:
                                 if (!adapter->frameReady())
                                     return;
                                 lifecycle->stop();
                                 delete state;
                                 delete attempts;
                                 app.exit(0);
                                 return;
                             default:
                                 fail();
                             }
                         });
        lifecycle->start();
    }

    const QString preview_visual_path = optionValue(arguments, QStringLiteral("--preview-visual-test"));
    if (!preview_visual_path.isEmpty()) {
        auto* wait_for_visual = new QTimer(&app);
        wait_for_visual->setInterval(50);
        int* attempts = new int(0);
        QObject::connect(
            wait_for_visual, &QTimer::timeout, &app,
            [&app, &quick_application, root_window, preview_visual_path, wait_for_visual, attempts]() {
                ++*attempts;
                if (!quick_application.recordPreviewAdapter()->frameReady()) {
                    if (*attempts < 200)
                        return;
                    wait_for_visual->stop();
                    delete attempts;
                    app.exit(3);
                    return;
                }
                wait_for_visual->stop();
                delete attempts;
                if (root_window != nullptr) {
                    if (QObject* page = root_window->findChild<QObject*>(QStringLiteral("quickRecordPage")))
                        page->setProperty("showMetricsOverlay", true);
                    root_window->setProperty("benchmarkInteractionActive", true);
                }
                QTimer::singleShot(1500, &app, [&app, root_window, preview_visual_path]() {
                    const bool saved = root_window != nullptr && root_window->grabWindow().save(preview_visual_path);
                    app.exit(saved ? 0 : 2);
                });
            });
        wait_for_visual->start();
    }

    if (arguments.contains(QStringLiteral("--hwnd-audit"))) {
        QTimer::singleShot(800, &app, [&app, root_window]() {
            const HWND hwnd = root_window != nullptr ? reinterpret_cast<HWND>(root_window->winId()) : nullptr;
            const int children = hwnd != nullptr ? childWindowCount(hwnd) : -1;
            qInfo("quick-hwnd-audit: child_hwnds=%d", children);

            // Second signal, same instrument: the frameless shell is only really
            // frameless if Windows reserves no non-client area. Without this the
            // audit could pass on a build that draws a native caption above our
            // own 40 px band — two title bars, zero child HWNDs.
            const LONG_PTR style = hwnd != nullptr ? GetWindowLongPtrW(hwnd, GWL_STYLE) : 0;
            const LONG_PTR ex_style = hwnd != nullptr ? GetWindowLongPtrW(hwnd, GWL_EXSTYLE) : 0;
            const NonClientInset inset = hwnd != nullptr ? nonClientInset(hwnd) : NonClientInset{};
            qInfo("quick-hwnd-audit: style=0x%08llx exstyle=0x%08llx caption=%d thickframe=%d border=%d",
                  static_cast<unsigned long long>(style), static_cast<unsigned long long>(ex_style),
                  (style & WS_CAPTION) == WS_CAPTION ? 1 : 0, (style & WS_THICKFRAME) != 0 ? 1 : 0,
                  (style & WS_BORDER) != 0 ? 1 : 0);
            qInfo("quick-hwnd-audit: nonclient_inset=%d,%d,%d,%d native_titlebar=%d", inset.left, inset.top,
                  inset.right, inset.bottom, inset.top > 0 ? 1 : 0);

            // What Qt BELIEVES the frame to be. Windows reserves nothing (the
            // inset above is all zeroes), so any difference here is a frame that
            // exists only in Qt's own bookkeeping — and that difference is what
            // gets added to the window every time a persisted geometry is
            // restored, then persisted again.
            if (root_window != nullptr) {
                const QRect client = root_window->geometry();
                const QRect frame = root_window->frameGeometry();
                qInfo("quick-hwnd-audit: qt_geometry=%d,%d %dx%d qt_frame=%d,%d %dx%d qt_frame_margins=%d,%d,%d,%d",
                      client.x(), client.y(), client.width(), client.height(), frame.x(), frame.y(), frame.width(),
                      frame.height(), client.left() - frame.left(), client.top() - frame.top(),
                      frame.right() - client.right(), frame.bottom() - client.bottom());
            }

            // Two assertions, both of them invisible in a screenshot:
            //
            // The non-client area must be empty — a window whose title band is
            // its own must have Windows reserve nothing outside its client rect,
            // or a native caption is being drawn above the product's.
            //
            // WS_THICKFRAME must be present. It is now asserted rather than only
            // reported: it is the single bit keeping the native resize drag, Aero
            // Snap and Win+Arrow alive on a frameless window, and it went missing
            // silently once before — Qt rewrites the whole style when it applies
            // Qt::FramelessWindowHint, and the bit was being set before that
            // rather than after. The old objection to asserting it (that Qt then
            // believes the window has an 8/31 px frame and misplaces it) does not
            // survive measurement: with the frameless hint applied Qt reports
            // frame margins of 0,0,0,0 with the bit set, which is what
            // qt_frame_margins above exists to keep honest.
            exosnap::quick::TraceWindowGeometry("settled", root_window);

            const bool chrome_clean = hwnd != nullptr && inset.isEmpty() && (style & WS_THICKFRAME) != 0;
            app.exit(children == 0 && chrome_clean ? 0 : 1);
        });
    }

    const QString benchmark_path = optionValue(arguments, QStringLiteral("--preview-benchmark"));
    if (!benchmark_path.isEmpty()) {
        bool duration_ok = false;
        const int requested_duration =
            optionValue(arguments, QStringLiteral("--benchmark-seconds")).toInt(&duration_ok);
        const int duration_seconds = duration_ok ? std::clamp(requested_duration, 2, 120) : 10;
        auto* wait_for_frame = new QTimer(&app);
        wait_for_frame->setInterval(50);
        int* attempts = new int(0);
        QObject::connect(
            wait_for_frame, &QTimer::timeout, &app,
            [&app, &quick_application, root_window, benchmark_path, duration_seconds, wait_for_frame, attempts]() {
                ++*attempts;
                if (!quick_application.recordPreviewAdapter()->frameReady()) {
                    if (*attempts < 200)
                        return;
                    wait_for_frame->stop();
                    delete attempts;
                    app.exit(3);
                    return;
                }

                wait_for_frame->stop();
                delete attempts;
                quick_application.recordPreviewAdapter()->resetMetrics();
                if (root_window != nullptr)
                    root_window->setProperty("benchmarkInteractionActive", true);
                const quint64 cpu_start = processCpuTime100ns();
                QTimer::singleShot(
                    duration_seconds * 1000, &app,
                    [&app, &quick_application, root_window, benchmark_path, duration_seconds, cpu_start]() {
                        QVariantMap values = quick_application.recordPreviewAdapter()->benchmarkSnapshot();
                        QJsonObject report = QJsonObject::fromVariantMap(values);
                        report.insert(QStringLiteral("duration_seconds"), duration_seconds);
                        report.insert(QStringLiteral("gpu"), defaultAdapterName());
                        if (QScreen* screen = QGuiApplication::primaryScreen()) {
                            report.insert(QStringLiteral("display_width"), screen->size().width());
                            report.insert(QStringLiteral("display_height"), screen->size().height());
                            report.insert(QStringLiteral("display_refresh_hz"), screen->refreshRate());
                            report.insert(QStringLiteral("device_pixel_ratio"), screen->devicePixelRatio());
                        }
                        SYSTEM_INFO system_info{};
                        GetSystemInfo(&system_info);
                        const quint64 cpu_delta = processCpuTime100ns() - cpu_start;
                        const double cpu_percent = system_info.dwNumberOfProcessors > 0
                                                       ? static_cast<double>(cpu_delta) /
                                                             (static_cast<double>(duration_seconds) * 10'000'000.0 *
                                                              system_info.dwNumberOfProcessors) *
                                                             100.0
                                                       : 0.0;
                        report.insert(QStringLiteral("process_cpu_percent"), cpu_percent);
                        report.insert(QStringLiteral("gpu_usage_percent"),
                                      QStringLiteral("unavailable: no in-process GPU counter"));
                        report.insert(QStringLiteral("qml_overlay_animation"), true);
                        report.insert(QStringLiteral("scene_graph_api"), QStringLiteral("Direct3D 11"));
                        report.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
                        report.insert(QStringLiteral("child_hwnd_count"),
                                      root_window != nullptr
                                          ? childWindowCount(reinterpret_cast<HWND>(root_window->winId()))
                                          : -1);

                        QFile output(benchmark_path);
                        const bool saved = output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                                           output.write(QJsonDocument(report).toJson()) >= 0;
                        output.close();
                        app.exit(saved ? 0 : 4);
                    });
            });
        wait_for_frame->start();
    }

#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
    // Replaces the former --preview-recording-benchmark path, which built its own
    // recording configuration, its own timing and its own report shape and
    // therefore could not be compared with a Widgets run. Everything meaningful
    // now comes from the shared drive loop.
    if (auto_record_requested) {
        // `bootstrap` is a stack object: its destructor performs the same clean-exit
        // marking and crash-capture shutdown every other early return here relies on.
        exosnap::benchmark::RunOutcome record_outcome;
        const int record_exit = exosnap::quick::RunQuickAutoRecord(app, quick_application, root_window,
                                                                   auto_record_options, &record_outcome);
        if (!auto_edit_requested || record_exit != 0)
            return record_exit;
        // Chained Record -> Edit -> Export, in one process: a second process would
        // edit a file this one had already forgotten about.
        //
        // The clip is handed over explicitly rather than through the application's
        // own Record -> Editor handoff. The drive loop takes the coordinator's
        // single result-callback slot for itself, so the view model never learns a
        // recording completed and openEditorForCurrentRecording() has nothing to
        // open. What the outcome carries -- the real output path and the measured
        // media duration -- is the same information that handoff would have used.
        if (auto_edit_options.media_path.isEmpty()) {
            auto_edit_options.media_path = record_outcome.output_path;
            auto_edit_options.media_duration_seconds = record_outcome.media_duration_seconds;
        }
        return exosnap::quick::RunQuickAutoEdit(app, quick_application, root_window, auto_edit_options);
    }

    if (auto_edit_requested)
        return exosnap::quick::RunQuickAutoEdit(app, quick_application, root_window, auto_edit_options);
#endif

    const QString still_validation_path = optionValue(arguments, QStringLiteral("--still-frame-validation"));
    if (!still_validation_path.isEmpty()) {
        struct StillValidationState {
            enum class Phase {
                WaitingForReadyFrame,
                CapturingReady,
                WaitingForRecording,
                CapturingRecording,
                WaitingForPaused,
                CapturingPaused,
                WaitingForResult,
            };
            Phase phase = Phase::WaitingForReadyFrame;
            recorder_core::CaptureTarget target;
            QJsonArray frames;
            int attempts = 0;
        };
        auto state = std::make_shared<StillValidationState>();
        auto* coordinator = quick_application.recordingCoordinator();
        QString preparation_error;
        bool target_found = false;
        if (coordinator != nullptr && quick_application.prepareRecordingBenchmark(60, preparation_error)) {
            for (const auto& target : coordinator->EnumerateTargets()) {
                if (target.kind == recorder_core::CaptureTarget::Kind::Monitor) {
                    state->target = target;
                    target_found = true;
                    break;
                }
            }
        }

        const auto write_report = [still_validation_path](QJsonObject report) {
            QFile output(still_validation_path);
            const bool saved = output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                               output.write(QJsonDocument(report).toJson()) >= 0;
            output.close();
            return saved;
        };
        if (!target_found) {
            QJsonObject report{{QStringLiteral("success"), false},
                               {QStringLiteral("error"), preparation_error.isEmpty()
                                                             ? QStringLiteral("No monitor target is available")
                                                             : preparation_error}};
            const bool saved = write_report(report);
            QTimer::singleShot(0, &app, [&app, saved]() { app.exit(saved ? 6 : 4); });
        } else {
            const auto fail = [&app, state, write_report](QString error) {
                QJsonObject report{{QStringLiteral("success"), false},
                                   {QStringLiteral("error"), std::move(error)},
                                   {QStringLiteral("frames"), state->frames}};
                const bool saved = write_report(report);
                app.exit(saved ? 7 : 4);
            };
            coordinator->SetFrameCapturedCallback(
                [&app, coordinator, state, fail](bool success, const QString& path, const QString& error) {
                    QMetaObject::invokeMethod(
                        &app,
                        [coordinator, state, fail, success, path, error]() {
                            if (!success) {
                                fail(error.isEmpty() ? QStringLiteral("Still-frame capture failed") : error);
                                return;
                            }
                            const QImage image(path);
                            if (image.isNull()) {
                                fail(QStringLiteral("Captured PNG could not be decoded: %1").arg(path));
                                return;
                            }
                            QString phase;
                            switch (state->phase) {
                            case StillValidationState::Phase::CapturingReady:
                                phase = QStringLiteral("ready");
                                state->phase = StillValidationState::Phase::WaitingForRecording;
                                {
                                    exosnap::capability::AudioUiState no_audio;
                                    no_audio.target_kind = exosnap::capability::CaptureTargetKind::Display;
                                    coordinator->StartRecording(state->target, no_audio);
                                }
                                break;
                            case StillValidationState::Phase::CapturingRecording:
                                phase = QStringLiteral("recording");
                                state->phase = StillValidationState::Phase::WaitingForPaused;
                                coordinator->PauseRecording();
                                break;
                            case StillValidationState::Phase::CapturingPaused:
                                phase = QStringLiteral("paused");
                                state->phase = StillValidationState::Phase::WaitingForResult;
                                coordinator->StopRecording();
                                break;
                            default:
                                fail(QStringLiteral("Unexpected still-frame callback phase"));
                                return;
                            }
                            state->attempts = 0;
                            state->frames.append(QJsonObject{{QStringLiteral("phase"), phase},
                                                             {QStringLiteral("path"), path},
                                                             {QStringLiteral("width"), image.width()},
                                                             {QStringLiteral("height"), image.height()}});
                        },
                        Qt::QueuedConnection);
                });
            coordinator->SetResultReadyCallback([&app, &quick_application, root_window, state,
                                                 write_report](const exosnap::UiRecordingResult& result) {
                QMetaObject::invokeMethod(
                    &app,
                    [&app, &quick_application, root_window, state, write_report, result]() {
                        const bool three_frames = state->frames.size() == 3;
                        QJsonObject report{
                            {QStringLiteral("success"), result.succeeded && three_frames},
                            {QStringLiteral("frames"), state->frames},
                            {QStringLiteral("recording_succeeded"), result.succeeded},
                            {QStringLiteral("recording_output"), QString::fromStdWString(result.output_path)},
                            {QStringLiteral("recording_dropped_frames"),
                             static_cast<qint64>(quick_application.recordPreviewAdapter()->recordingDroppedFrames())},
                            {QStringLiteral("child_hwnd_count"),
                             root_window != nullptr ? childWindowCount(reinterpret_cast<HWND>(root_window->winId()))
                                                    : -1}};
                        if (!result.succeeded)
                            report.insert(QStringLiteral("error"), QString::fromStdWString(result.error_detail));
                        const bool saved = write_report(report);
                        app.exit(saved && result.succeeded && three_frames ? 0 : 8);
                    },
                    Qt::QueuedConnection);
            });

            auto* still_timer = new QTimer(&app);
            still_timer->setInterval(50);
            QObject::connect(still_timer, &QTimer::timeout, &app,
                             [&app, &quick_application, coordinator, state, still_timer, fail]() {
                                 if (++state->attempts > 1200) {
                                     still_timer->stop();
                                     fail(QStringLiteral("Still-frame validation timed out"));
                                     return;
                                 }
                                 switch (state->phase) {
                                 case StillValidationState::Phase::WaitingForReadyFrame:
                                     if (!quick_application.recordPreviewAdapter()->frameReady())
                                         return;
                                     state->phase = StillValidationState::Phase::CapturingReady;
                                     state->attempts = 0;
                                     coordinator->CaptureFrame();
                                     break;
                                 case StillValidationState::Phase::WaitingForRecording:
                                     if (coordinator->State() != exosnap::UiRecordingState::Recording)
                                         return;
                                     state->phase = StillValidationState::Phase::CapturingRecording;
                                     state->attempts = 0;
                                     coordinator->CaptureFrame();
                                     break;
                                 case StillValidationState::Phase::WaitingForPaused:
                                     if (coordinator->State() != exosnap::UiRecordingState::Paused)
                                         return;
                                     state->phase = StillValidationState::Phase::CapturingPaused;
                                     state->attempts = 0;
                                     coordinator->CaptureFrame();
                                     break;
                                 case StillValidationState::Phase::WaitingForResult:
                                 case StillValidationState::Phase::CapturingReady:
                                 case StillValidationState::Phase::CapturingRecording:
                                 case StillValidationState::Phase::CapturingPaused:
                                     break;
                                 }
                             });
            still_timer->start();
        }
    }

    const QString target_validation_path = optionValue(arguments, QStringLiteral("--target-refresh-validation"));
    if (!target_validation_path.isEmpty()) {
        struct TargetValidationState {
            bool armed = false;
            bool probe_seen = false;
            int initial_displays = 0;
            int initial_windows = 0;
            int maximum_windows = 0;
            QJsonArray events;
        };
        auto state = std::make_shared<TargetValidationState>();
        auto* adapter = quick_application.recordViewModelAdapter();
        const auto write_target_report = [target_validation_path, state](bool success, QString error = {}) {
            QJsonObject report{{QStringLiteral("success"), success},
                               {QStringLiteral("initial_display_count"), state->initial_displays},
                               {QStringLiteral("initial_window_count"), state->initial_windows},
                               {QStringLiteral("maximum_window_count"), state->maximum_windows},
                               {QStringLiteral("probe_seen"), state->probe_seen},
                               {QStringLiteral("events"), state->events}};
            if (!error.isEmpty())
                report.insert(QStringLiteral("error"), std::move(error));
            QFile output(target_validation_path);
            const bool saved = output.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                               output.write(QJsonDocument(report).toJson()) >= 0;
            output.close();
            return saved;
        };
        QObject::connect(adapter, &exosnap::quick::RecordViewModelAdapter::targetOptionsChanged, &app,
                         [&app, adapter, state, write_target_report]() {
                             if (!state->armed)
                                 return;
                             const int displays = adapter->displayTargetOptions().size();
                             const int windows = adapter->windowTargetOptions().size();
                             const bool probe_present =
                                 std::ranges::any_of(adapter->windowTargetOptions(), [](const QVariant& option) {
                                     return option.toMap()
                                         .value(QStringLiteral("label"))
                                         .toString()
                                         .contains(QStringLiteral("ExoSnap Target Refresh Probe"), Qt::CaseInsensitive);
                                 });
                             state->maximum_windows = std::max(state->maximum_windows, windows);
                             state->events.append(QJsonObject{{QStringLiteral("display_count"), displays},
                                                              {QStringLiteral("window_count"), windows},
                                                              {QStringLiteral("probe_present"), probe_present}});
                             if (probe_present)
                                 state->probe_seen = true;
                             if (state->probe_seen && !probe_present) {
                                 const bool saved = write_target_report(true);
                                 app.exit(saved ? 0 : 4);
                             }
                         });
        QTimer::singleShot(1000, &app, [adapter, state]() {
            state->initial_displays = adapter->displayTargetOptions().size();
            state->initial_windows = adapter->windowTargetOptions().size();
            state->maximum_windows = state->initial_windows;
            state->armed = true;
        });
        QTimer::singleShot(30000, &app, [&app, state, write_target_report]() {
            const bool saved = write_target_report(false, QStringLiteral("Target refresh validation timed out"));
            app.exit(saved ? 9 : 4);
        });
    }

    const QString screenshot_path = visualOutputPath(arguments);
    if (!screenshot_path.isEmpty()) {
        // Harness-only, same class of hook as --preview-visual-test's metrics
        // overlay: opens the Record button's countdown menu so the capture can
        // photograph it. A menu is otherwise only reachable by hovering or
        // clicking the chevron, and this harness synthesises no input.
        const bool open_countdown_menu = arguments.contains(QStringLiteral("--record-visual-menu"));
        const auto capture = [&app, root_window, screenshot_path]() {
            const bool saved = root_window != nullptr && root_window->grabWindow().save(screenshot_path);
            // The capture-excluded overlays are separate top-level windows, so
            // the root grab above cannot contain them -- and a desktop capture
            // cannot either, by design. QQuickWindow::grabWindow renders the
            // scene graph directly, which is not what SetWindowDisplayAffinity
            // suppresses, so it is the one way to photograph these at all.
            saveOverlayWindowGrabs(screenshot_path);
            app.exit(saved ? 0 : 2);
        };
        QTimer::singleShot(visualCaptureDelayMs(arguments), &app, [&app, root_window, open_countdown_menu, capture]() {
            if (!open_countdown_menu) {
                capture();
                return;
            }
            if (root_window != nullptr) {
                if (QObject* split = root_window->findChild<QObject*>(QStringLiteral("quickRecordSplitButton")))
                    QMetaObject::invokeMethod(split, "openCountdownMenu");
            }
            // The popup has an enter transition; grabbing in
            // the same tick photographs it mid-fade.
            QTimer::singleShot(400, &app, capture);
        });
    }

    return app.exec();
}
