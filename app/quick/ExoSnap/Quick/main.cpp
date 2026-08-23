// Not inside the EXOSNAP_ENABLE_AUTO_RECORD_HARNESS block below, and that is
// the whole contract: --pseudo-localize is armed by argv and by nothing else
// (see PseudoLocalization.h), so its translator has to exist in every
// configuration the product ships. Its sources are added to the target
// unconditionally for the same reason; a harness-gated include here compiled in
// Debug and broke Release, where the gate is off.
#include "PseudoLocalization.h"
#include "QuickApplication.h"
#include "QuickLiveVerifySource.h"
#include "QuickWindowGeometry.h"
#if defined(EXOSNAP_ENABLE_AUTO_RECORD_HARNESS)
#include "NotificationsAdapter.h"
#include "QuickAutoEditHarness.h"
#include "QuickAutoRecordHarness.h"
#include "auto_record/AutoRecordHarness.h"
#endif
#include "bootstrap/ProductionBootstrap.h"
#include "cli/CommandLineFlags.h"
#include "diagnostics/NativeWindowFacts.h"
#include "diagnostics/StartupClock.h"
#include "live_verify/LiveVerifyCommandPolicy.h"
#include "live_verify/LiveVerifyControlServer.h"
#include "live_verify/LiveVerifyOptions.h"
#include "services/ElevatedRelaunch.h"
#include "services/RecordingCoordinator.h"
#include "services/UpdateFeedOverride.h"
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
#include <QMetaMethod>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSize>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVariantMap>

#include <capability/audio_ui_state.h>

#include <QFile>

#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>

namespace {

// A rejected command line has to reach the person or script that typed it. This
// is a GUI-subsystem binary, so qCritical alone lands in the log and nowhere the
// caller can see -- an explicit rejection the caller cannot read is barely better
// than a silent one. stdout is where the harness already prints its result line.
void reportStartupError(const QString& message) {
    QTextStream(stdout) << message << Qt::endl;
    qCritical().noquote() << message;
}

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
    if (arguments.contains(QStringLiteral("--window-maximize-cycle")))
        return QStringLiteral("quick-window-cycle");
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

// ── --navigation-lifecycle-test (QCR-615) ───────────────────────────────────
//
// Settings, Diagnostics, Logs and About are loaded by URL rather than from an
// inline `sourceComponent`, which is the only reason their documents stay out of
// the startup compile. That mechanism removes two things a component reference
// used to give for free, and this test is where both are asserted:
//
//   * the pages are genuinely absent until they are navigated to. Nothing else
//     in the product observes it, and the whole point of the change is that they
//     are not there;
//   * `setSource()` carries initial property VALUES, not bindings and not signal
//     handlers. So every adapter a page requires has to have actually arrived,
//     and Diagnostics' two navigation signals have to still reach the shell even
//     though the shell no longer knows the page's type.
//
// Structural, never timed: no wall-clock assertion belongs in CI. The loaders are
// synchronous, so a navigation and its result are observable in the same call.
struct NavigationDestination {
    int page = 0;
    const char* object_name = nullptr;
    const char* adapter_property = nullptr;
    QObject* adapter = nullptr;
};

int failNavigationLifecycle(const char* what) {
    qCritical().noquote() << QStringLiteral("navigation-lifecycle: ") + QString::fromLatin1(what);
    // AppLog's message handler replaces Qt's default one, and Qt hands a first
    // installer no previous handler to chain to -- so nothing reaches stderr and
    // the whole diagnosis lives in a log file inside a throwaway config dir. A
    // CTest failure has to state its reason where CTest captures it, or the gate
    // reports red without saying why.
    std::fprintf(stderr, "navigation-lifecycle: %s\n", what);
    return 5;
}

QObject* findShellPage(QQuickWindow* window, const char* object_name) {
    return window != nullptr ? window->findChild<QObject*>(QString::fromLatin1(object_name)) : nullptr;
}

// Calls the shell's one navigation edge the way a tab, a shortcut or a
// notification action does. Resolved through the metaobject rather than by name
// alone, because a typed QML function (`navigateTo(page: int)`) publishes an
// int parameter and an untyped one publishes a QVariant -- and invoking with the
// wrong one fails with a warning instead of an error.
bool invokeNavigateTo(QObject* shell, int page) {
    const QMetaObject* meta = shell->metaObject();
    for (int index = 0; index < meta->methodCount(); ++index) {
        const QMetaMethod method = meta->method(index);
        if (method.name() != QByteArrayLiteral("navigateTo") || method.parameterCount() != 1)
            continue;
        if (method.parameterMetaType(0) == QMetaType::fromType<int>())
            return method.invoke(shell, Q_ARG(int, page));
        return method.invoke(shell, Q_ARG(QVariant, QVariant(page)));
    }
    return false;
}

// One nav-tab delegate. A Repeater's delegates are not QObject children of the
// window -- only the Repeater itself is -- so they are reached through itemAt().
QObject* navTabAt(QObject* repeater, int index) {
    QQuickItem* item = nullptr;
    if (!QMetaObject::invokeMethod(repeater, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, index)))
        return nullptr;
    return item;
}

// The clip the QCR-001 assertions below run against. No master path, so nothing
// is decoded, no keyframe scan starts and no export can run -- exactly the
// fixture shape the visual harness uses. A duration is all `editOverlayOpen`
// needs.
exosnap::EditContext navigationTestEditContext() {
    exosnap::EditContext context;
    context.output_path = QStringLiteral("qcr001-navigation-lifecycle.mkv");
    context.duration = QStringLiteral("2:34");
    context.duration_seconds = 154.0;
    return context;
}

int runNavigationLifecycleTest(QQuickWindow* window, exosnap::quick::QuickApplication& application) {
    if (window == nullptr)
        return failNavigationLifecycle("no root window");
    QObject* shell = window->findChild<QObject*>(QStringLiteral("quickAppShell"));
    if (shell == nullptr)
        return failNavigationLifecycle("no quickAppShell");

    const std::array<NavigationDestination, 4> destinations{{
        {1, "quickSettingsPage", "settings", application.settingsAdapter()},
        {2, "quickDiagnosticsPage", "diagnostics", application.diagnosticsAdapter()},
        {3, "quickLogsPage", "logs", application.logsAdapter()},
        {4, "quickAboutPage", "aboutViewModel", application.aboutViewModel()},
    }};

    // Startup built the page the user is looking at, and nothing else.
    if (findShellPage(window, "quickRecordPage") == nullptr)
        return failNavigationLifecycle("Record page missing after startup");
    for (const NavigationDestination& destination : destinations) {
        if (findShellPage(window, destination.object_name) != nullptr)
            return failNavigationLifecycle(destination.object_name);
    }

    // First visit: the page exists and holds the adapter it was handed.
    std::array<QObject*, 4> first_visit{};
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const NavigationDestination& destination = destinations.at(index);
        shell->setProperty("currentPage", destination.page);
        QObject* page = findShellPage(window, destination.object_name);
        if (page == nullptr)
            return failNavigationLifecycle(destination.object_name);
        if (page->property(destination.adapter_property).value<QObject*>() != destination.adapter)
            return failNavigationLifecycle(destination.adapter_property);
        first_visit.at(index) = page;
    }

    // Diagnostics is the one page with a second required property, and the one
    // whose initial-property list could silently lose a member.
    QObject* diagnostics = first_visit.at(1);
    if (diagnostics->property("device").value<QObject*>() != application.deviceAdapter())
        return failNavigationLifecycle("device");

    // Second visit: the same instance, so a page still remembers where the user
    // was in it (the resident contract from QCR-602).
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        shell->setProperty("currentPage", 0);
        shell->setProperty("currentPage", destinations.at(index).page);
        if (findShellPage(window, destinations.at(index).object_name) != first_visit.at(index))
            return failNavigationLifecycle("page rebuilt on second visit");
    }

    // The two signals the shell used to handle inline on its sourceComponent.
    shell->setProperty("currentPage", 2);
    if (!QMetaObject::invokeMethod(diagnostics, "navigateToLogsRequested"))
        return failNavigationLifecycle("navigateToLogsRequested not invokable");
    if (shell->property("currentPage").toInt() != 3)
        return failNavigationLifecycle("navigateToLogsRequested did not navigate");
    shell->setProperty("currentPage", 2);
    if (!QMetaObject::invokeMethod(diagnostics, "navigateToSettingsRequested"))
        return failNavigationLifecycle("navigateToSettingsRequested not invokable");
    if (shell->property("currentPage").toInt() != 1)
        return failNavigationLifecycle("navigateToSettingsRequested did not navigate");

    // ── QCR-001: an open edit session is state of Record, not a modality ────
    //
    // This block used to assert the opposite ("navigation not locked during an
    // edit session"). That contract was never a product decision: the Widgets
    // shell that shipped until the cutover let the nav tabs navigate for the
    // whole edit session, the Quick port lost it in `enabled: !editOverlayOpen`,
    // and Wave 0 canonised the regression on the false premise that the code had
    // always disabled them. What follows is the intended contract instead.
    shell->setProperty("currentPage", 0);
    exosnap::quick::EditSessionAdapter* session = application.editSessionAdapter();
    exosnap::quick::EditPlayerAdapter* player = application.editPlayerAdapter();
    exosnap::quick::EditExportAdapter* exporter = application.editExportAdapter();
    if (session == nullptr || player == nullptr || exporter == nullptr)
        return failNavigationLifecycle("no edit adapters");

    session->setEditContext(navigationTestEditContext());
    if (!shell->property("editOverlayOpen").toBool())
        return failNavigationLifecycle("a clip did not open the edit workspace");
    QObject* workspace = findShellPage(window, "quickEditOverlay");
    if (workspace == nullptr)
        return failNavigationLifecycle("no quickEditOverlay after a clip opened");
    // `visible` on a QQuickItem is EFFECTIVE visibility, so this is read off the
    // workspace itself rather than off the shell's derived property: the point of
    // the contract is what reaches the screen, not what the shell believes.
    if (!shell->property("editOverlayVisible").toBool() || !workspace->property("visible").toBool())
        return failNavigationLifecycle("the edit workspace is not visible on Record");

    // The affordance itself, not only the edge behind it: this exact `enabled`
    // binding is where the Quick port lost the Widgets shell's contract.
    QObject* nav_tabs = findShellPage(window, "quickNavTabs");
    if (nav_tabs == nullptr)
        return failNavigationLifecycle("no quickNavTabs repeater");
    for (int tab = 0; tab <= 4; ++tab) {
        QObject* delegate = navTabAt(nav_tabs, tab);
        if (delegate == nullptr)
            return failNavigationLifecycle("a navigation tab is missing");
        if (!delegate->property("enabled").toBool())
            return failNavigationLifecycle("a navigation tab is disabled during an edit session");
    }

    // State the user would notice losing, set before leaving and compared after
    // returning. Product state, not QML internals.
    session->requestTrim(22000, 118000);
    session->requestSeek(41000);
    const QString clip_path = session->clipPath();
    const qint64 trim_start = session->trimStartMs();
    const qint64 trim_end = session->trimEndMs();
    const qint64 position = session->positionMs();
    if (trim_start != 22000 || trim_end != 118000 || position != 41000)
        return failNavigationLifecycle("the fixture clip did not take the trim and the position");

    // Every destination stays reachable, and reaching it neither closes the
    // session nor leaves the workspace lying over the page that replaced it.
    for (int page = 1; page <= 4; ++page) {
        if (!invokeNavigateTo(shell, page))
            return failNavigationLifecycle("navigateTo is not invokable");
        if (shell->property("currentPage").toInt() != page)
            return failNavigationLifecycle("a destination was refused during an edit session");
        if (!shell->property("editOverlayOpen").toBool())
            return failNavigationLifecycle("navigation closed the edit session");
        if (shell->property("editOverlayVisible").toBool() || workspace->property("visible").toBool())
            return failNavigationLifecycle("the edit workspace stayed visible off Record");
        if (findShellPage(window, "quickEditOverlay") != workspace)
            return failNavigationLifecycle("the edit workspace was rebuilt by a page change");
    }

    // Back on Record: the same workspace, the same session, the same numbers.
    if (!invokeNavigateTo(shell, 0))
        return failNavigationLifecycle("navigateTo is not invokable");
    if (!shell->property("editOverlayVisible").toBool() || !workspace->property("visible").toBool())
        return failNavigationLifecycle("returning to Record did not show the edit workspace");
    if (findShellPage(window, "quickEditOverlay") != workspace)
        return failNavigationLifecycle("returning to Record built a second edit workspace");
    if (session->clipPath() != clip_path || session->trimStartMs() != trim_start || session->trimEndMs() != trim_end ||
        session->positionMs() != position)
        return failNavigationLifecycle("the edit session lost state across a page change");

    // Playback: leaving Record pauses, keeps the position, and does not resume
    // on the way back.
    player->setClipStateForTest(/*clip_open=*/true, session->durationMs());
    player->setPlaying(true);
    if (!player->playing())
        return failNavigationLifecycle("the fixture player refused to play");
    if (!invokeNavigateTo(shell, 1))
        return failNavigationLifecycle("navigateTo is not invokable");
    if (player->playing())
        return failNavigationLifecycle("playback kept running off Record");
    if (session->positionMs() != position)
        return failNavigationLifecycle("pausing on a page change moved the playhead");
    if (!invokeNavigateTo(shell, 0))
        return failNavigationLifecycle("navigateTo is not invokable");
    if (player->playing())
        return failNavigationLifecycle("returning to Record resumed playback on its own");
    if (session->positionMs() != position)
        return failNavigationLifecycle("returning to Record moved the playhead");
    player->setClipStateForTest(/*clip_open=*/false, 0);

    // Export: a page change is not a cancel. The run lives on a thread the
    // adapter owns, so it is unaffected by which QML item is on screen -- this
    // pins that, using the harness state seam rather than a fake export.
    exporter->applyVisualState(exosnap::quick::EditExportAdapter::Running, 42, QString(), QString());
    if (!invokeNavigateTo(shell, 2))
        return failNavigationLifecycle("navigateTo is not invokable");
    if (!exporter->running() || exporter->progressPercent() != 42)
        return failNavigationLifecycle("navigating away disturbed a running export");
    if (!invokeNavigateTo(shell, 0))
        return failNavigationLifecycle("navigateTo is not invokable");
    if (!exporter->running() || exporter->progressPercent() != 42)
        return failNavigationLifecycle("returning to Record disturbed a running export");
    exporter->applyVisualState(exosnap::quick::EditExportAdapter::Options, 0, QString(), QString());

    // The alternative navigation path. ShellAdapter::navigateToPageRequested is
    // what a notification action, the Diagnostics blocker jump and the recording
    // error's log jump all emit; it used to write `currentPage` directly and so
    // obeyed a different contract than the tabs did.
    exosnap::quick::ShellAdapter* shell_adapter = application.shellAdapter();
    if (shell_adapter == nullptr)
        return failNavigationLifecycle("no shell adapter");
    emit shell_adapter->navigateToPageRequested(exosnap::quick::ShellAdapter::LogsPage);
    if (shell->property("currentPage").toInt() != 3)
        return failNavigationLifecycle("the adapter navigation path did not reach the shell");
    if (!shell->property("editOverlayOpen").toBool() || shell->property("editOverlayVisible").toBool() ||
        workspace->property("visible").toBool())
        return failNavigationLifecycle("the adapter navigation path used a different edit contract");

    // A blocking surface still blocks — QCR-415 must not regress. Edit is not
    // one of them, which is the whole point of the block above.
    exosnap::quick::RecordingErrorAdapter* recording_error = application.recordingErrorAdapter();
    if (recording_error == nullptr)
        return failNavigationLifecycle("no recording error adapter");
    exosnap::models::RecordingFailureReport report;
    report.title = QStringLiteral("Recording stopped unexpectedly");
    report.summary = QStringLiteral("navigation-lifecycle fixture");
    recording_error->present(report, /*can_send_report=*/false);
    if (shell->property("navigationAllowed").toBool())
        return failNavigationLifecycle("a blocking surface left navigation allowed");
    if (QObject* delegate = navTabAt(nav_tabs, 1); delegate == nullptr || delegate->property("enabled").toBool())
        return failNavigationLifecycle("a blocking surface left the navigation tabs enabled");
    (void)invokeNavigateTo(shell, 4);
    if (shell->property("currentPage").toInt() != 3)
        return failNavigationLifecycle("navigation went through behind a blocking surface");
    recording_error->dismiss();
    if (!shell->property("navigationAllowed").toBool())
        return failNavigationLifecycle("dismissing the blocking surface did not restore navigation");

    session->close();
    if (shell->property("editOverlayOpen").toBool())
        return failNavigationLifecycle("closing the session left the edit workspace open");

    shell->setProperty("currentPage", 0);
    return 0;
}

// installTranslator does not take ownership, and the translator has to outlive
// every qsTr() that resolves through it -- so it is parented to the application
// and destroyed with it. The static analyser does not model Qt's parent
// ownership and reads the allocation as unowned; the suppression is scoped to
// this function so it can never cover an unrelated allocation.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
void installPseudoLocalization(QApplication& app) {
    QCoreApplication::installTranslator(new exosnap::quick::PseudoLocalizationTranslator(&app));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

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

    // Before any parser looks at argv. Five parsers read the full argument list
    // and each skips what it does not own, which means a misspelled harness
    // option was ignored by all five: the run then succeeded without performing
    // the check it was asked for. Fail closed here instead, once, against the
    // registry of every option this binary understands.
    {
        QString flag_error;
        if (!exosnap::cli::ValidateCommandLine(arguments, &flag_error)) {
            reportStartupError(flag_error);
            return 2;
        }
    }

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
        reportStartupError(live_verify_options.error);
        return 2;
    }

    // The dev feed override. Refused outright in an official build and refused
    // on a malformed value, because a test that believes it is pointed at a
    // fixture and is actually talking to the production feed reports the wrong
    // thing -- and could act on a real release.
    const exosnap::services::UpdateFeedOverride update_feed_override =
        exosnap::services::ParseUpdateFeedOverride(arguments);
    if (update_feed_override.requested && !update_feed_override.error.isEmpty()) {
        reportStartupError(update_feed_override.error);
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
            reportStartupError(parse_error);
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
            reportStartupError(parse_error);
            return 2;
        }
    }
#else
    constexpr bool auto_record_requested = false;
    constexpr bool auto_edit_requested = false;
#endif
    const bool navigation_lifecycle_test = arguments.contains(QStringLiteral("--navigation-lifecycle-test"));
    // Isolated and exempt from the single-instance guard like every other
    // harness, but NOT started no-activate: the state it drives is the window
    // state, and a window Windows was told not to activate is not the window the
    // user maximizes.
    const bool maximize_cycle = arguments.contains(QStringLiteral("--window-maximize-cycle"));
    const bool diagnostic_mode =
        preview_mode || auto_record_requested || auto_edit_requested || navigation_lifecycle_test || maximize_cycle ||
        arguments.contains(QStringLiteral("--smoke-test")) || arguments.contains(QStringLiteral("--visual-test"));

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
    if (arguments.contains(QStringLiteral("--window-trace")) || maximize_cycle) {
        exosnap::quick::SetWindowGeometryTraceEnabled(true);
        // Before the window exists: the messages that decide its first rect are
        // sent during creation, so a filter installed afterwards would miss the
        // only part of the sequence worth measuring.
        exosnap::quick::InstallStartupMessageTrace();
        // The cycle happens after the first frame, which is where the trace
        // normally stops itself.
        if (maximize_cycle)
            exosnap::quick::SetStartupMessageTracePersistent(true);
    }

    // Harness-only: controlled text expansion for the 860x700 layout regression
    // pass (QCR-511). Installed BEFORE the engine loads, so every qsTr() in QML
    // resolves through it on its first evaluation and no retranslate() call is
    // needed. argv-only by design — see PseudoLocalization.h.
    if (arguments.contains(QStringLiteral("--pseudo-localize")))
        installPseudoLocalization(app);

    exosnap::quick::QuickApplication quick_application;
    // ADR 0033: the handoff a prior elevated self-relaunch put in our own argv.
    // Applied before load() so the shell's landing page is decided once, rather
    // than navigating away from Record after the first frame. ADR 0055: the
    // verification-reinstall arming is argv-only and never persisted.
    const exosnap::services::RelaunchHandoff startup_handoff = exosnap::services::ParseRelaunchArgs(arguments);
    quick_application.applyStartupRelaunchHandoff(startup_handoff.page_name, startup_handoff.reenable_present_diag);
    quick_application.applyVerifyUpdateReinstallMode(exosnap::services::HasVerifyUpdateReinstallRequest(arguments));
    quick_application.applyUpdateFeedOverride(update_feed_override.base_url);
    // The child updater gets an automation endpoint ONLY when this process has
    // one. Same run id, different role in the pipe name (ADR 0067), so a runner
    // that is already driving this process can reach the updater it starts
    // without discovering anything.
    quick_application.applyUpdaterAutomationRunId(live_verify_options.requested ? live_verify_options.run_id
                                                                                : QString());
    // A --visual-test sweep is one process per scenario and a benchmark run
    // measures the frontend, not the notification area; neither should drop an
    // ExoSnap icon into the developer's tray. --smoke-test is deliberately NOT in
    // this list: it is what proves the tray still constructs and tears down
    // inside the real application.
    quick_application.applyTraySuppression(preview_mode || auto_record_requested || auto_edit_requested ||
                                           navigation_lifecycle_test ||
                                           arguments.contains(QStringLiteral("--visual-test")));

    if (!quick_application.load(diagnostic_mode && !maximize_cycle))
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
        //
        // The update child is the one event that is not a state change of THIS
        // process: it carries the pid, the staged binary and the endpoint of the
        // updater that was just started, so a runner attaches to a process it
        // caused instead of watching for a pipe to appear.
        if (auto* updates = quick_application.updateService()) {
            QObject::connect(updates, &exosnap::UpdateService::updaterLaunched, server, [server, source]() {
                server->EmitEvent(QStringLiteral("update.updaterLaunched"), source->UpdaterLaunchSnapshot());
            });
        }
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
        // Protocol 2's general settle signal. The source advances its revision
        // only when the observable state actually differs, so this fires on real
        // changes and not on every elapsed-time tick — which is what makes it
        // something a runner can wait on. It also gives the three blocking
        // surfaces their first observable transition: they had no event at all,
        // so a check could not even see the state that made record.start refuse.
        QObject::connect(
            source, &exosnap::quick::QuickLiveVerifySource::observableStateChanged, server, [server, source]() {
                server->EmitEvent(QStringLiteral("ui.stateChanged"),
                                  exosnap::live_verify::StateToJson(source->State(), source->StateRevision()));
            });
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

    // Queued rather than immediate: the shell's own Component.onCompleted is what
    // loads the landing destination, and the assertions below start from "nothing
    // but Record exists" — running before that completes would test a half-built
    // shell and pass for the wrong reason.
    if (navigation_lifecycle_test) {
        QTimer::singleShot(0, &app, [&app, root_window, &quick_application]() {
            app.exit(runNavigationLifecycleTest(root_window, quick_application));
        });
    }

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
            qInfo("quick-hwnd-audit: style=0x%08llx exstyle=0x%08llx caption=%d thickframe=%d border=%d "
                  "maximizebox=%d minimizebox=%d sysmenu=%d",
                  static_cast<unsigned long long>(style), static_cast<unsigned long long>(ex_style),
                  (style & WS_CAPTION) == WS_CAPTION ? 1 : 0, (style & WS_THICKFRAME) != 0 ? 1 : 0,
                  (style & WS_BORDER) != 0 ? 1 : 0, (style & WS_MAXIMIZEBOX) != 0 ? 1 : 0,
                  (style & WS_MINIMIZEBOX) != 0 ? 1 : 0, (style & WS_SYSMENU) != 0 ? 1 : 0);
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
            // The gesture style bits must be present. They are asserted rather
            // than only reported: WS_THICKFRAME carries the native resize drag,
            // Aero Snap and Win+Arrow; WS_MAXIMIZEBOX carries
            // double-click-to-maximize, drag-to-top and the Snap Layouts flyout;
            // WS_MINIMIZEBOX and WS_SYSMENU carry Win+Down and the window menu.
            // None of them look wrong in a screenshot, and WS_THICKFRAME went
            // missing silently once before -- Qt rewrites the whole style when it
            // applies Qt::FramelessWindowHint, and the bit was being set before
            // that rather than after. The old objection to asserting it (that Qt
            // then believes the window has an 8/31 px frame and misplaces it) does
            // not survive measurement: with the frameless hint applied Qt reports
            // frame margins of 0,0,0,0 with the bits set, which is what
            // qt_frame_margins above exists to keep honest.
            exosnap::quick::TraceWindowGeometry("settled", root_window);

            constexpr LONG_PTR kGestureStyles = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;
            const bool chrome_clean = hwnd != nullptr && inset.isEmpty() && (style & kGestureStyles) == kGestureStyles;
            app.exit(children == 0 && chrome_clean ? 0 : 1);
        });
    }

    // Harness-only: drives the product's own maximize/restore path and checks the
    // result against Windows rather than against Qt.
    //
    // The window state is the one part of the chrome no fixture can reach.
    // QT_QPA_PLATFORM=offscreen has no HWND, so IsZoomed, WINDOWPLACEMENT and the
    // WS_MAXIMIZE bit -- the only authoritative answers to "is this window
    // maximized" -- do not exist there. The alternative is a developer performing
    // the gesture by hand, which is not a regression check.
    //
    // argv-configured, never input synthesis: the toggle is the same QML function
    // the Maximize button calls, so a run exercises the shipped path.
    if (arguments.contains(QStringLiteral("--window-maximize-cycle"))) {
        struct CycleState {
            int step = 0;
            int windowed_edges = 0;
            RECT baseline{};
        };
        auto state = std::make_shared<CycleState>();
        auto* cycle = new QTimer(&app);
        cycle->setInterval(700);
        QObject::connect(cycle, &QTimer::timeout, &app, [&app, root_window, cycle, state]() {
            const HWND hwnd = root_window != nullptr ? reinterpret_cast<HWND>(root_window->winId()) : nullptr;
            if (hwnd == nullptr) {
                cycle->stop();
                app.exit(3);
                return;
            }
            const auto sameRect = [](const RECT& a, const RECT& b) {
                return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
            };
            // WM_NCHITTEST sent straight to the window, which is how the resize
            // gating becomes observable without a mouse: the message reaches the
            // same filter a real drag would, moves no cursor and takes no focus.
            const auto hitTest = [hwnd](LONG x, LONG y) {
                return static_cast<LRESULT>(
                    SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y))));
            };
            const auto resizeEdgeCount = [&hitTest](const RECT& rect) {
                const LONG mid_x = (rect.left + rect.right) / 2;
                const LONG mid_y = (rect.top + rect.bottom) / 2;
                const POINT probes[] = {{mid_x, rect.top + 1},         {mid_x, rect.bottom - 2},
                                        {rect.left + 1, mid_y},        {rect.right - 2, mid_y},
                                        {rect.left + 1, rect.top + 1}, {rect.right - 2, rect.bottom - 2}};
                int edges = 0;
                for (const POINT& probe : probes) {
                    const LRESULT code = hitTest(probe.x, probe.y);
                    if (code >= HTLEFT && code <= HTBOTTOMRIGHT)
                        ++edges;
                }
                return edges;
            };
            const auto report = [hwnd](const char* stage, RECT* normal_out) {
                RECT rect{};
                GetWindowRect(hwnd, &rect);
                WINDOWPLACEMENT placement{};
                placement.length = sizeof(placement);
                const bool have_placement = GetWindowPlacement(hwnd, &placement) != FALSE;
                const RECT& normal = placement.rcNormalPosition;
                qInfo("window-cycle: %s zoomed=%d iconic=%d show_cmd=%u ws_maximize=%d restore_to_max=%d "
                      "window=%ld,%ld %ldx%ld restore=%ld,%ld %ldx%ld",
                      stage, IsZoomed(hwnd) != FALSE ? 1 : 0, IsIconic(hwnd) != FALSE ? 1 : 0,
                      have_placement ? placement.showCmd : 0u,
                      (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZE) != 0 ? 1 : 0,
                      have_placement && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0 ? 1 : 0, rect.left, rect.top,
                      rect.right - rect.left, rect.bottom - rect.top, normal.left, normal.top,
                      normal.right - normal.left, normal.bottom - normal.top);
                if (normal_out != nullptr)
                    *normal_out = normal;
                return rect;
            };

            switch (state->step) {
            case 0: {
                // Normalisation, not the path under test: the previous run may
                // have persisted a maximized window, and the cycle has to start
                // from a known windowed state either way.
                ShowWindow(hwnd, SW_RESTORE);
                // Placed on a rect that is deliberately NOT the work area. A
                // persisted window that already fills it makes the round-trip
                // assertion below true whatever the restore does.
                MONITORINFO monitor_info{};
                monitor_info.cbSize = sizeof(monitor_info);
                const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                    const RECT& work = monitor_info.rcWork;
                    SetWindowPos(hwnd, nullptr, work.left + 120, work.top + 80, (work.right - work.left) / 2,
                                 (work.bottom - work.top) / 2, SWP_NOZORDER | SWP_NOACTIVATE);
                }
                break;
            }
            case 1:
                state->baseline = report("baseline", nullptr);
                if (IsZoomed(hwnd) != FALSE) {
                    qWarning("window-cycle: FAIL the window is still zoomed after SW_RESTORE");
                    cycle->stop();
                    app.exit(4);
                    return;
                }
                // Asserted in BOTH states, because "no resize edge while maximized"
                // is worthless on its own: a hit test that answered HTCLIENT
                // everywhere would satisfy it and leave the window unresizable.
                state->windowed_edges = resizeEdgeCount(state->baseline);
                qInfo("window-cycle: windowed resize edges=%d/6", state->windowed_edges);
                if (state->windowed_edges != 6) {
                    qWarning("window-cycle: FAIL a windowed window must answer all six resize probes");
                    cycle->stop();
                    app.exit(5);
                    return;
                }
                QMetaObject::invokeMethod(root_window, "toggleMaximized");
                break;
            case 2: {
                RECT normal{};
                const RECT maximized = report("maximized", &normal);
                // Checked against Windows, not against Window.visibility: a QML
                // state that no Win32 fact agrees with is exactly the defect this
                // harness exists to catch, and reading it back from QML would
                // report success on it.
                if (IsZoomed(hwnd) == FALSE) {
                    qWarning("window-cycle: FAIL the window is not zoomed after toggleMaximized() "
                             "(%ldx%ld, still SW_SHOWNORMAL)",
                             maximized.right - maximized.left, maximized.bottom - maximized.top);
                    cycle->stop();
                    app.exit(1);
                    return;
                }
                // The restore rect has to survive the maximize untouched. It is a
                // separate assertion from the round trip below because it fails
                // earlier and says why: a window that resizes into the maximized
                // state instead of entering it overwrites this rect on the way in.
                if (!sameRect(normal, state->baseline)) {
                    qWarning("window-cycle: FAIL the restore rect became %ld,%ld %ldx%ld while maximizing "
                             "(it was %ld,%ld %ldx%ld)",
                             normal.left, normal.top, normal.right - normal.left, normal.bottom - normal.top,
                             state->baseline.left, state->baseline.top, state->baseline.right - state->baseline.left,
                             state->baseline.bottom - state->baseline.top);
                    cycle->stop();
                    app.exit(2);
                    return;
                }
                const int maximized_edges = resizeEdgeCount(maximized);
                qInfo("window-cycle: maximized resize edges=%d/6", maximized_edges);
                if (maximized_edges != 0) {
                    qWarning("window-cycle: FAIL a maximized window answered %d resize probes", maximized_edges);
                    cycle->stop();
                    app.exit(5);
                    return;
                }
                // Minimize and un-minimize WHILE STILL MAXIMIZED. The taskbar
                // button sends exactly the SC_RESTORE used below, so the sequence
                // is the real one without any input: Windows must bring a window
                // that was maximized back MAXIMIZED, which it decides from
                // WPF_RESTORETOMAXIMIZED in the placement.
                // The shell's own function, not QWindow::showMinimized(): that is
                // the path the Minimize button takes.
                QMetaObject::invokeMethod(root_window, "minimizeWindow");
                break;
            }
            case 3: {
                report("minimized", nullptr);
                if (IsIconic(hwnd) == FALSE) {
                    qWarning("window-cycle: FAIL the window is not iconic after showMinimized()");
                    cycle->stop();
                    app.exit(6);
                    return;
                }
                SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
                break;
            }
            case 4: {
                report("un-minimized", nullptr);
                if (IsZoomed(hwnd) == FALSE) {
                    qWarning("window-cycle: FAIL a window minimized while maximized came back windowed");
                    cycle->stop();
                    app.exit(6);
                    return;
                }
                QMetaObject::invokeMethod(root_window, "toggleMaximized");
                break;
            }
            case 5: {
                const RECT restored = report("restored", nullptr);
                cycle->stop();
                if (!sameRect(restored, state->baseline)) {
                    qWarning("window-cycle: FAIL restored to %ld,%ld %ldx%ld but the window started at %ld,%ld %ldx%ld",
                             restored.left, restored.top, restored.right - restored.left,
                             restored.bottom - restored.top, state->baseline.left, state->baseline.top,
                             state->baseline.right - state->baseline.left,
                             state->baseline.bottom - state->baseline.top);
                    app.exit(2);
                    return;
                }
                if (IsZoomed(hwnd) != FALSE) {
                    qWarning("window-cycle: FAIL the window is still zoomed after the second toggleMaximized()");
                    app.exit(1);
                    return;
                }
                qInfo("window-cycle: PASS maximize and restore round-tripped");
                app.exit(0);
                return;
            }
            default:
                break;
            }
            ++state->step;
        });
        cycle->start();
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
