#include "MainWindow.h"

#include "MainWindowPages.h"
#include "VisualGpuFixture.h"

#include "diagnostics/AppLog.h"
#include "diagnostics/ConfigSummary.h"
#include "diagnostics/StartupClock.h"
#include "diagnostics/StartupTrace.h"
#include "diagnostics/SupportBundle.h"
#include "models/RecordingPreset.h"
#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "pages/AboutPage.h"
#include "pages/ConfigPage.h"
#include "pages/DevicePage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/EditExportPage.h"
#include "pages/LogsPage.h"
#include "pages/RecordPage.h"
#include "services/ElevatedRelaunch.h"
#include "services/GlobalHotkeyService.h"
#include "services/UpdateService.h"
#include "ui/WindowGeometryPolicy.h"
#include "ui/chrome/NotificationHubPanel.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/dialogs/CrashReportOverlay.h"
#include "ui/dialogs/EditExportOverlay.h"
#include "ui/dialogs/FinalizingOverlay.h"
#include "ui/dialogs/RecordingErrorOverlay.h"
#include "ui/dialogs/RecoveryOverlay.h"
#include "ui/dialogs/SourcePickerOverlay.h"
#include "ui/dialogs/WhatsNewOverlay.h"
#include "ui/overlay/CountdownOverlayWindow.h"
#include "ui/overlay/DiagnosticsOverlayWindow.h"
#include "ui/overlay/NotificationToastWindow.h"
#include "ui/overlay/QuickControlPillWindow.h"
#include "ui/overlay/RecordingOverlayWindow.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "ui/theme/ExoSnapPalette.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/tray/TrayPresence.h"
#include "ui/widgets/NotificationBell.h"
#include "ui/widgets/WebcamSetupPanel.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "ui/widgets/EditPlayerSurface.h"
#include "ui/widgets/ExportPanel.h"
#include "visual_tests/VisualScenario.h"

#include <QToolButton>
#endif

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion / kGitCommit

#include <capability/adapter_enum.h>

#include <QFileDialog>
#include <QProcess>
#include <QStandardPaths>

#include <filesystem>

#include <capability/capability_builder.h>
#include <capability/capability_cache_key.h>
#include <capability/codec_selection.h>
#include <capability/config_types.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <crash_capture/crash_capture.h>
#include <crash_capture/crash_scrubber.h>
#include <update/update_handoff.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

#if defined(Q_OS_WIN)
#include "exosnap_resource.h"

#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>

#if defined(_MSC_VER)
#pragma comment(lib, "dwmapi.lib")
#endif
#endif

namespace exosnap {

// The page table and its derived indices live in MainWindowPages.h so the
// visual-scenario translation unit indexes the same stack from the same source.
using namespace pages;

namespace {

constexpr bool kTraceFrameActivation = false;

#if defined(Q_OS_WIN)
// Win32 registrar — wraps a live HWND; created in showEvent once the handle is valid.
class Win32HotkeyRegistrar : public IHotkeyRegistrar {
  public:
    explicit Win32HotkeyRegistrar(HWND hwnd) : hwnd_(hwnd) {
    }
    bool Register(int id, unsigned int modifiers, unsigned int vk) override {
        return ::RegisterHotKey(hwnd_, id, static_cast<UINT>(modifiers), static_cast<UINT>(vk)) != FALSE;
    }
    void Unregister(int id) override {
        ::UnregisterHotKey(hwnd_, id);
    }

  private:
    HWND hwnd_ = nullptr;
};
#endif

void appendFrameTrace(const QString& line) {
    if (!kTraceFrameActivation)
        return;

    static const QString kLogPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/exosnap_frame_activation.log");
    QFile file(kLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;

    QTextStream stream(&file);
    stream << line << '\n';
}

enum class ResizeZone {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// ELEVATION-FOUNDATION-R1: map a nav label (kPageDescriptors nav_label) to its
// page index for the elevated-relaunch handoff. Returns -1 when unknown.
int pageIndexForNavLabel(const QString& label) {
    for (std::size_t i = 0; i < kPageDescriptors.size(); ++i) {
        if (label.compare(QString::fromUtf8(kPageDescriptors[i].nav_label), Qt::CaseInsensitive) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

// UPDATE-WIRE-R1: map between the persisted/UI channel string ("Stable"|"Preview")
// and the engine enum. Unknown values fall back to Stable.
update::UpdateChannel UpdateChannelFromString(const QString& channel) {
    return channel.compare(QStringLiteral("Preview"), Qt::CaseInsensitive) == 0 ? update::UpdateChannel::Preview
                                                                                : update::UpdateChannel::Stable;
}

QString UpdateChannelToString(update::UpdateChannel channel) {
    return channel == update::UpdateChannel::Preview ? QStringLiteral("Preview") : QStringLiteral("Stable");
}

// SETTINGS-HONESTY-R1: map the persisted/UI developer log-level string ("Off" |
// "Error" | "Warning" | "Info" | "Debug") to AppLog's filter (nullopt = "Off",
// i.e. record nothing). Unknown/legacy values fall back to Debug (record
// everything, review F1) so a corrupt or stale key can never silently narrow
// support diagnostics below what main always recorded.
std::optional<diagnostics::LogSeverity> DeveloperLogLevelFromString(const QString& level) {
    if (level.compare(QStringLiteral("Off"), Qt::CaseInsensitive) == 0)
        return std::nullopt;
    if (level.compare(QStringLiteral("Error"), Qt::CaseInsensitive) == 0)
        return diagnostics::LogSeverity::Error;
    if (level.compare(QStringLiteral("Warning"), Qt::CaseInsensitive) == 0)
        return diagnostics::LogSeverity::Warning;
    if (level.compare(QStringLiteral("Info"), Qt::CaseInsensitive) == 0)
        return diagnostics::LogSeverity::Info;
    return diagnostics::LogSeverity::Debug;
}

ResizeZone resizeZoneFromLocalPoint(const QPoint& local, const QSize& size, bool maximized) {
    if (maximized)
        return ResizeZone::None;

    constexpr int resize_border = 8;
    const bool left = local.x() >= -resize_border && local.x() < resize_border;
    const bool right = local.x() <= size.width() + resize_border && local.x() > size.width() - resize_border;
    const bool top = local.y() >= -resize_border && local.y() < resize_border;
    const bool bottom = local.y() <= size.height() + resize_border && local.y() > size.height() - resize_border;

    if (top && left)
        return ResizeZone::TopLeft;
    if (top && right)
        return ResizeZone::TopRight;
    if (bottom && left)
        return ResizeZone::BottomLeft;
    if (bottom && right)
        return ResizeZone::BottomRight;
    if (left)
        return ResizeZone::Left;
    if (right)
        return ResizeZone::Right;
    if (top)
        return ResizeZone::Top;
    if (bottom)
        return ResizeZone::Bottom;
    return ResizeZone::None;
}

LRESULT hitTestFromResizeZone(ResizeZone zone) {
    switch (zone) {
    case ResizeZone::Left:
        return HTLEFT;
    case ResizeZone::Right:
        return HTRIGHT;
    case ResizeZone::Top:
        return HTTOP;
    case ResizeZone::Bottom:
        return HTBOTTOM;
    case ResizeZone::TopLeft:
        return HTTOPLEFT;
    case ResizeZone::TopRight:
        return HTTOPRIGHT;
    case ResizeZone::BottomLeft:
        return HTBOTTOMLEFT;
    case ResizeZone::BottomRight:
        return HTBOTTOMRIGHT;
    case ResizeZone::None:
    default:
        return HTCLIENT;
    }
}

HCURSOR cursorFromHitTestCode(LRESULT hit_test) {
    switch (hit_test) {
    case HTLEFT:
    case HTRIGHT:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case HTTOP:
    case HTBOTTOM:
        return LoadCursorW(nullptr, IDC_SIZENS);
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
        return LoadCursorW(nullptr, IDC_SIZENESW);
    default:
        return nullptr;
    }
}

void ensureWin32ResizableStyle(HWND hwnd) {
    if (hwnd == nullptr)
        return;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_THICKFRAME) != 0)
        return;

    style |= WS_THICKFRAME;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

// Resolve the active theme's subtle line colour to an opaque COLORREF for the
// native DWM window border. The theme's line tokens are semi-transparent white
// (dark) / ink (light) overlays, so composite them over the theme background —
// DWMWA_BORDER_COLOR takes no alpha.
COLORREF themedDwmBorderColorref() {
    const auto& theme = exosnap::ui::theme::ActiveTheme();
    QColor bg = exosnap::ui::theme::ParseThemeColor(theme.bg);
    if (!bg.isValid())
        bg = QColor(0x0E, 0x0E, 0x10);
    const QColor line = exosnap::ui::theme::ParseThemeColor(theme.line2);
    QColor resolved = bg;
    if (line.isValid()) {
        const double a = line.alphaF();
        resolved =
            QColor(qRound(line.red() * a + bg.red() * (1.0 - a)), qRound(line.green() * a + bg.green() * (1.0 - a)),
                   qRound(line.blue() * a + bg.blue() * (1.0 - a)));
    }
    return RGB(resolved.red(), resolved.green(), resolved.blue());
}

// Paint the frameless window's native 1px contour in a subtle themed line colour
// (it follows the Win11 rounded corners, which no QSS border can). Failures stay
// silent apart from a one-time log line: DWMWA_BORDER_COLOR does not exist before
// Windows 11 build 22000, and those systems simply keep the default frame.
void applyDwmThemedBorder(HWND hwnd, const char* reason) {
    if (hwnd == nullptr)
        return;

#if !defined(DWMWA_BORDER_COLOR)
#define DWMWA_BORDER_COLOR 34
#endif

    const COLORREF border_color = themedDwmBorderColorref();
    const HRESULT hr =
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, static_cast<DWORD>(sizeof(border_color)));

    if (kTraceFrameActivation) {
        const QString line = QStringLiteral("%1 [FrameDbg] DwmSetWindowAttribute(DWMWA_BORDER_COLOR=0x%2) reason=%3 "
                                            "hwnd=0x%4 hr=0x%5")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
                                 .arg(QString::number(static_cast<quint32>(border_color), 16))
                                 .arg(QString::fromLatin1(reason != nullptr ? reason : "null"))
                                 .arg(QString::number(reinterpret_cast<quintptr>(hwnd), 16))
                                 .arg(QString::number(static_cast<quint32>(hr), 16));
        qDebug().noquote() << line;
        appendFrameTrace(line);
    }

    if (FAILED(hr)) {
        static bool warned_once = false;
        if (!warned_once) {
            warned_once = true;
            qWarning().nospace() << "DwmSetWindowAttribute(DWMWA_BORDER_COLOR) failed, hr=0x"
                                 << QString::number(static_cast<quint32>(hr), 16);
        }
    }
}

void applyDwmThemedBorder(HWND hwnd) {
    applyDwmThemedBorder(hwnd, "unspecified");
}

void traceFrameMessage(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    if (!kTraceFrameActivation)
        return;

    const char* name = nullptr;
    switch (message) {
    case WM_ACTIVATE:
        name = "WM_ACTIVATE";
        break;
    case WM_NCACTIVATE:
        name = "WM_NCACTIVATE";
        break;
    case WM_SETFOCUS:
        name = "WM_SETFOCUS";
        break;
    case WM_KILLFOCUS:
        name = "WM_KILLFOCUS";
        break;
    case WM_NCPAINT:
        name = "WM_NCPAINT";
        break;
    case WM_STYLECHANGED:
        name = "WM_STYLECHANGED";
        break;
    case WM_WINDOWPOSCHANGED:
        name = "WM_WINDOWPOSCHANGED";
        break;
    case WM_SIZE:
        name = "WM_SIZE";
        break;
    default:
        return;
    }

    const QString line = QStringLiteral("%1 [FrameDbg] %2 hwnd=0x%3 wParam=0x%4 lParam=0x%5")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
                             .arg(QString::fromLatin1(name))
                             .arg(QString::number(reinterpret_cast<quintptr>(hwnd), 16))
                             .arg(QString::number(static_cast<quintptr>(w_param), 16))
                             .arg(QString::number(static_cast<quintptr>(l_param), 16));
    qDebug().noquote() << line;
    appendFrameTrace(line);
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), recovery_service_(recovery_manifest_store_) {
    diagnostics::AppLog::init();
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("mainwindow-ctor-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("MainWindow constructing"));

    setWindowTitle("ExoSnap");
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint | Qt::WindowSystemMenuHint);

#if defined(Q_OS_WIN)
    // Keep the native 1px DWM contour in the active theme's line colour across
    // theme switches. The initial application happens with the rest of the Win32
    // frame setup (resizable_style_applied_) once the native window exists; this
    // subscriber only refreshes an already-applied border, so it must not force
    // native-window creation during construction.
    ui::theme::OnThemeChanged(this, [this]() {
        if (!resizable_style_applied_)
            return;
        if (HWND hwnd = reinterpret_cast<HWND>(effectiveWinId()))
            applyDwmThemedBorder(hwnd, "theme-change");
    });
#endif

    if (!QApplication::windowIcon().isNull()) {
        setWindowIcon(QApplication::windowIcon());
    } else {
        static const QString kAppIconPath = QStringLiteral(":/brand/exosnap-logo-idle.ico");
        if (!QFile::exists(kAppIconPath))
            qWarning().noquote() << "MainWindow icon resource missing:" << kAppIconPath;
        QIcon fallback_icon(kAppIconPath);
        if (fallback_icon.isNull())
            qWarning().noquote() << "MainWindow failed to load icon from resource:" << kAppIconPath;
        setWindowIcon(fallback_icon);
    }
    if (!windowIcon().isNull() && windowIcon().availableSizes().isEmpty())
        qWarning().noquote() << "MainWindow icon is set but reports no available sizes.";
    // D6 wave-2 responsive: lowered from 1120 so a single Settings card column
    // is fully visible at the minimum width.  RecordPage preview scales down
    // gracefully to ~200 px; rail column is fixed at 320 px.
    setMinimumSize(860, 700);

    // ---- Load reduced AppSettingsStore (hotkeys + window geometry only) ----
    persisted_settings_ = settings_store_.Load();
    if (!persisted_settings_.load_ok) {
        app_settings_corrupted_ = true;
        diagnostics::AppLog::warning(
            QStringLiteral("settings"),
            QStringLiteral("Settings file could not be read cleanly and was reset to defaults: %1")
                .arg(settings_store_.SettingsFilePath()));
    }
    // SETTINGS-HONESTY-R1: narrow AppLog's recording filter to the persisted developer
    // log-level now that it is known. AppLog::init() (above) already ran with the
    // "record everything" default, so early-startup entries are unaffected.
    diagnostics::AppLog::setMinSeverity(DeveloperLogLevelFromString(persisted_settings_.developer_log_level));
    // Reconcile the SDK-wide persisted consent with the explicit app policy
    // before any report UI is considered in this process.
    switch (persisted_settings_.crash_report_policy) {
    case CrashReportPolicy::AskEveryTime:
        applyCrashConsentAction(CrashConsentAction::ResetToAsk);
        break;
    case CrashReportPolicy::AlwaysSend:
        applyCrashConsentAction(CrashConsentAction::GrantPersistent);
        break;
    case CrashReportPolicy::NeverSend:
        applyCrashConsentAction(CrashConsentAction::Revoke);
        break;
    }
    // ADR 0033: sync the present provider opt-in from the persisted setting now
    // that the settings store has been loaded. The provider was constructed with
    // opt_in=false; SetOptIn kicks off the ETW session when elevation allows it.
    present_provider_.SetOptIn(persisted_settings_.present_diagnostics_optin);
    // ADR 0033: the kernel DPC/ISR provider shares the present opt-in gate but has no
    // internal elevation check, so apply (opt-in && elevation) here — mirroring
    // PresentMonProvider::GateOpen(). Graceful: Start() returns false when ETW can't open.
    if (persisted_settings_.present_diagnostics_optin && elevation_provider_.IsElevated()) {
        [[maybe_unused]] const bool dpc_started = dpc_provider_.Start();
    }
    initHotkeyService();

    // ---- Update engine bridge (UPDATE-WIRE-R1 · ADR 0012) ----
    // Constructed with nullptr here because record_page_ (and its RecordingCoordinator)
    // does not exist yet at this point in the constructor -- RecordPage is built
    // further down, and its coordinator is itself only built later still, asynchronously,
    // once runtime capability probing completes. UpdateService::SetRecordingCoordinator()
    // wires the real coordinator in once RecordPage::coordinatorInitialized() fires (see
    // below), so the engine-layer guard (LaunchUpdater's multi-state check) is live; the
    // app-layer guard (recording_active_ || remuxing_active_) stays as belt-and-suspenders.
    update_service_ = new UpdateService(nullptr, this);
    update_service_->SetChannel(UpdateChannelFromString(persisted_settings_.update_channel));
    connect(update_service_, &UpdateService::updateCheckComplete, this, &MainWindow::onUpdateCheckComplete);

    // Process launch and close handoff are separate states. Download/verification
    // can still fail or be cancelled while the old app remains open, so spawning
    // the detached updater must never persist the applied-version stamp or claim
    // a restart is pending.
    connect(update_service_, &UpdateService::updaterLaunched, this, [this]() {
        update_handoff_phase_ = UpdateHandoffPhase::UpdaterRunning;
        if (config_page_)
            config_page_->setUpdateStatus(QStringLiteral("updater-running"), last_available_version_, QString());
        diagnostics::AppLog::info(
            QStringLiteral("update"),
            verify_update_reinstall_
                ? QStringLiteral("Updater launched to verify-reinstall %1; waiting for close handoff")
                      .arg(last_available_version_)
                : QStringLiteral("Updater launched for %1; waiting for close handoff").arg(last_available_version_));
    });
    connect(update_service_, &UpdateService::updaterExited, this, [this](qint64 process_id, quint32 exit_code) {
        if (update_handoff_phase_ != UpdateHandoffPhase::UpdaterRunning)
            return;

        update_handoff_phase_ = UpdateHandoffPhase::Idle;
        const QString state =
            verify_update_reinstall_ ? QStringLiteral("verify-reinstall") : QStringLiteral("available");
        if (config_page_)
            config_page_->setUpdateStatus(state, last_available_version_, QString());
        diagnostics::AppLog::warning(
            QStringLiteral("update"),
            QStringLiteral("Updater process %1 exited before close handoff (code %2); update card re-armed")
                .arg(process_id)
                .arg(exit_code));
    });
    // Any staging/launch failure surfaces on the Settings card as an error.
    connect(update_service_, &UpdateService::updateError, this,
            [this](exosnap::update::VerifyResult /*result*/, const QString& detail) {
                update_handoff_phase_ = UpdateHandoffPhase::Idle;
                if (config_page_)
                    config_page_->setUpdateStatus(QStringLiteral("error"), QString(), QString(), detail);
                diagnostics::AppLog::warning(QStringLiteral("update"),
                                             QStringLiteral("Updater launch failed: %1").arg(detail));
            });

    // Handoff truth belongs to the process that accepted the updater's marked
    // close request. A fresh process (whether the new version or a restored old
    // one) must never reconstruct "Restart pending" from that stale UI stamp.
    const QString reconciled_applied = ReconcileAppliedVersionOnStartup(persisted_settings_.applied_version);
    if (persisted_settings_.applied_version != reconciled_applied) {
        persisted_settings_.applied_version = reconciled_applied;
        settings_store_.Save(persisted_settings_);
    }

    // ---- Load preset store (live config is the truth; presets are snapshots) ----
    PersistedPresetState loaded_presets = preset_store_.Load();
    preset_registry_.LoadState(std::move(loaded_presets.user_presets), loaded_presets.selected_id);
    if (loaded_presets.live.has_value()) {
        boot_live_config_ = SanitizePresetConfig(*loaded_presets.live);
    } else {
        // No readable live config: start on Default, not (changed).
        preset_registry_.SetSelected(std::string(kDefaultPresetId));
        boot_live_config_ = preset_registry_.SelectedSavedConfig();
    }
    if (loaded_presets.repaired) {
        preset_store_repaired_ = true;
        diagnostics::AppLog::warning(QStringLiteral("presets"),
                                     QStringLiteral("Preset store repaired field-wise on load"));
        preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(), boot_live_config_);
    }

    // Initialize live mirrors from the restored live config.
    output_settings_ = boot_live_config_.output;
    video_settings_ = boot_live_config_.video;
    live_audio_ = boot_live_config_.audio;
    live_webcam_ = boot_live_config_.webcam;

    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("settings loaded"));
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("settings-loaded %1 ms").arg(diagnostics::StartupClock().elapsed()));

    // ---- Crash-capture session lifecycle (CRASH-WIRE-R1 · ADR 0017) ----
    // ORDER IS CRITICAL: read the previous session's crash context (if any)
    // BEFORE BeginSession overwrites the sidecar with the new session marker.
    // crash_capture::Initialize() already ran in main(); we only manage the
    // session sidecar here. Honest crash detection = "previous session did not
    // mark a clean exit" — works even in the OFF/stub build (no Crashpad).
    crash_dir_ = crash_capture::ResolveCrashDir();
    if (!crash_dir_.empty()) {
        pending_crash_ = crash_capture::ReadPreviousCrashContext(crash_dir_);
        if (pending_crash_) {
            diagnostics::AppLog::warning(
                QStringLiteral("crash"),
                QStringLiteral("Previous session did not exit cleanly — crash dialog pending"));
        }
        crash_capture::BeginSession(crash_dir_, currentSessionContext());
        // Mirror the context into the live Sentry scope (no-op w/o DSN).
        refreshCrashSessionContext();
    } else {
        diagnostics::AppLog::warning(QStringLiteral("crash"),
                                     QStringLiteral("Crash dir unavailable — session tracking disabled"));
    }

    auto* central = new QWidget(this);
    central->setObjectName("mainCentral");
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    title_bar_ = new ui::chrome::OperationalTitleBar(central);
    main_layout->addWidget(title_bar_);

    // Top navigation (in the title bar) drives the page stack directly — no sidebar, no
    // secondary page header. Pages own their internal padding and fill the area below the bar.
    stack_ = new QStackedWidget(central);
    stack_->setObjectName("mainStack");
    record_page_ = new RecordPage(stack_);
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("record-page-built %1 ms").arg(diagnostics::StartupClock().elapsed()));
    // Deferred: device_page_ is built by buildDevicePage() after show().
    // A cheap placeholder holds index kDevicePageIndex so config_placeholder_ and
    // all subsequent pages get the correct indices without any re-numbering.
    device_placeholder_ = new QWidget(stack_);
    // Deferred: config_page_ is built by buildConfigPage() after show().
    // A cheap placeholder holds index kSettingsPageIndex so the diagnostics slot and
    // all subsequent pages get the correct indices without any re-numbering.
    config_placeholder_ = new QWidget(stack_);
    // Deferred: diagnostics_page_ is built by buildDiagnosticsPage() after show().
    // A cheap placeholder holds index kDiagnosticsPageIndex so logs_page_ etc. get
    // the correct subsequent indices without any re-numbering.
    diagnostics_placeholder_ = new QWidget(stack_);
    stack_->addWidget(record_page_);
    stack_->addWidget(device_placeholder_); // device page is deferred
    stack_->addWidget(config_placeholder_); // config page is deferred
    stack_->addWidget(diagnostics_placeholder_);
    // Deferred: logs_page_ is built by buildLogsPage() after show().
    // A cheap placeholder holds index kLogsPageIndex so about_page_ etc. get
    // the correct subsequent indices without any re-numbering.
    logs_placeholder_ = new QWidget(stack_);
    stack_->addWidget(logs_placeholder_);
    // Deferred: about_page_ is built by buildAboutPage() after show().
    // A cheap placeholder holds index kAboutPageIndex (the last stack slot —
    // EDIT-OVERLAY-R1 removed the former EditExportPage tail slot).
    about_placeholder_ = new QWidget(stack_);
    stack_->addWidget(about_placeholder_);
    // EDIT-OVERLAY-R1 (ADR 0022 update): EditExportPage is no longer a stack page —
    // it is hosted by edit_export_overlay_, an in-window overlay over Record
    // (navigateToEditExportPage). Deferred: built by buildEditExportOverlay() after
    // show() — no stack slot/placeholder needed at all now.
    // Inject the recovery manifest store before the coordinator is initialized.
    record_page_->setRecoveryManifestStore(&recovery_manifest_store_);
    record_page_->setOutputSettings(output_settings_);
    record_page_->setVideoSettings(video_settings_);
    record_page_->setWebcamSettings(live_webcam_);
    record_page_->applyPersistedAudioSettings(live_audio_);
    record_page_->setCountdownSeconds(boot_live_config_.countdown_seconds);
    record_page_->restoreRecordingHistory();
    // NOTE: config_page_ initial setters and signal connects are wired in buildConfigPage().

    main_layout->addWidget(stack_, 1);

    // Source picker overlay — in-window, same accessibility-first parenting as About.
    source_picker_overlay_ = new ui::dialogs::SourcePickerOverlay(central);
    source_picker_overlay_->hide();
    record_page_->setSourcePickerOverlay(source_picker_overlay_);

    // EDIT-OVERLAY-R1: edit_export_overlay_ itself is lightweight, but the
    // EditExportPage it hosts is not (recorder_core-coupled, builds a full UI) — so
    // its construction stays deferred to buildEditExportOverlay() via
    // hydrateSecondaryPages()/navigateToEditExportPage(), exactly like the former
    // buildEditExportOverlay(). Only the (cheap) member pointer starts null here.

    // PS-PHASE-B: Notification hub panel — top-level popup, no Qt parent.
    notification_hub_ = new ui::chrome::NotificationHubPanel(nullptr);
    notification_hub_->hide();

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    notification_hub_->setDemoAdvisories(true);
    // The demo advisories seeded above include a caution entry, so the harness
    // renders the dot in the colour a real mixed-severity inbox would produce.
    title_bar_->setBellUnreadStatus(QStringLiteral("caution"));
#endif

    // PS-PHASE-E: Deep-link target contract — route by target string.
    connect(notification_hub_, &ui::chrome::NotificationHubPanel::deepLinkRequested, this,
            [this](const QString& target) {
                notification_hub_->hide();
                if (target == QStringLiteral("present-needs-admin")) {
                    // ADR 0033: the present-diagnostics advisory action relaunches as
                    // administrator (recording guard lives in dispatchNotificationAction).
                    dispatchNotificationAction(notifications::NotificationEvent{},
                                               notifications::NotificationAction::RelaunchElevated);
                    return;
                }
                if (target == QStringLiteral("update-view") || target == QStringLiteral("about")) {
                    // About is now a nav page; Settings hosts the update panel.
                    navigateToPage(kAboutPageIndex);
                } else if (target == QStringLiteral("recovery-view")) {
                    // Recovery overlay (if still open) or just navigate to Record.
                    navigateToPage(kRecordPageIndex);
                } else if (target == QStringLiteral("diagnostics")) {
                    navigateToPage(kDiagnosticsPageIndex);
                } else if (target == QStringLiteral("preset-undo")) {
                    // The hub keeps the record of a preset switch; Undo restores the
                    // previous live config via the same handler the toast used to.
                    dispatchNotificationAction(notifications::NotificationEvent{},
                                               notifications::NotificationAction::UndoPresetSwitch);
                } else if (target.startsWith(QStringLiteral("reveal:"))) {
                    // Re-surface a saved file from the hub record.
                    notifications::NotificationEvent ev;
                    ev.action_payload = target.mid(QStringLiteral("reveal:").size());
                    dispatchNotificationAction(ev, notifications::NotificationAction::OpenFolder);
                } else if (target.startsWith(QStringLiteral("settings/"))) {
                    navigateToPage(kSettingsPageIndex);
                    if (config_page_)
                        config_page_->scrollToSection(target);
                } else {
                    navigateToPage(kSettingsPageIndex);
                }
            });

    connect(title_bar_, &ui::chrome::OperationalTitleBar::bellClicked, this, &MainWindow::toggleNotificationHub);

    // PS-PHASE-B: Hotkeys live as an embedded card inside Settings (no standalone page).
    // About is now a real embedded nav page (no overlay).
    title_bar_->setNavItems({
        {QStringLiteral("Record"), kRecordPageIndex},
        {QStringLiteral("Device"), kDevicePageIndex},
        {QStringLiteral("Settings"), kSettingsPageIndex},
        {QStringLiteral("Diagnostics"), kDiagnosticsPageIndex},
        {QStringLiteral("Logs"), kLogsPageIndex},
        {QStringLiteral("About"), kAboutPageIndex},
    });

    connect(title_bar_, &ui::chrome::OperationalTitleBar::navPageRequested, this, &MainWindow::navigateToPage);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::maximizeRestoreRequested, this, [this]() {
        if (isFullScreen()) {
            pre_fullscreen_maximized_ ? showMaximized() : showNormal();
            return;
        }
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(effectiveWinId());
        const bool zoomed = (hwnd != nullptr) && (IsZoomed(hwnd) != FALSE);
        zoomed ? showNormal() : showMaximized();
#else
        isMaximized() ? showNormal() : showMaximized();
#endif
    });
    connect(title_bar_, &ui::chrome::OperationalTitleBar::closeRequested, this, &QWidget::close);
    connect(record_page_, &RecordPage::chromeStateChanged, this, &MainWindow::onRecordChromeStateChanged);
    connect(record_page_, &RecordPage::chromeRuntimeMetricsChanged, this,
            [this](const QString& elapsed, const QString& /*bitrate*/, const QString& /*drop_text*/,
                   const QString& /*size*/, double /*av_drift_ms*/) {
                // RECORDING-OVERLAY-R1: keep the overlay elapsed text in sync. (The live
                // drop count is intentionally NOT shown in the title pill — it is dominated
                // by benign CFR downsampling; real drops are surfaced post-recording.)
                if (recording_overlay_ && recording_overlay_->isVisible())
                    recording_overlay_->updateElapsed(elapsed);
            });
    connect(record_page_, &RecordPage::navigateToDiagnosticsPage, this,
            [this]() { navigateToPage(kDiagnosticsPageIndex); });
    connect(record_page_, &RecordPage::editExportRequested, this,
            [this](const exosnap::EditContext& ctx) { navigateToEditExportPage(ctx); });
    // NOTE: config_page_ format/preset/video/audio/webcam signal connects are wired in buildConfigPage().

    // ---- FixAction routing (ADR 0033 / v0.8.0) ----
    // NOTE: diagnostics_page_ is deferred — these connects are wired in buildDiagnosticsPage().

    // ---- Audio settings changed (from RecordPage) ----
    connect(record_page_, &RecordPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        if (applying_preset_)
            return;
        live_audio_ = state;
        if (config_page_)
            config_page_->setAudioUiState(state);
        onLiveConfigChanged();
        refreshDiagnosticsData();
    });

    // ---- Recording config changed (target/region/countdown user action) ----
    connect(record_page_, &RecordPage::recordingConfigChanged, this, [this]() {
        if (applying_preset_)
            return;
        onLiveConfigChanged();
        // Capture-target resolution (saved-display-not-found notice) can change on
        // a manual re-selection or a topology-driven re-resolve.
        refreshDiagnosticsData();
    });

    // NOTE: config_page_ webcamSettingsChanged connect is wired in buildConfigPage().

    // ---- PiP placement confirmed in the Record preview ----
    connect(record_page_, &RecordPage::webcamSettingsChanged, this, [this](const WebcamSettings& settings) {
        if (applying_preset_)
            return;
        live_webcam_ = settings;
        if (config_page_)
            config_page_->setWebcamSettings(settings);
        onLiveConfigChanged();
    });

    // NOTE: config_page_ preset-management connects are wired in buildConfigPage().

    // ---- Hotkeys ----
    connect(this, &MainWindow::recordToggleRequested, record_page_, &RecordPage::onHotkeyToggle);
    connect(this, &MainWindow::pauseToggleRequested, record_page_, &RecordPage::onHotkeyPauseToggle);
    connect(this, &MainWindow::captureFrameRequested, record_page_, &RecordPage::onHotkeyCaptureFrame);
    connect(this, &MainWindow::addMarkerRequested, record_page_, &RecordPage::onHotkeyAddMarker);
    connect(this, &MainWindow::splitRecordingRequested, record_page_, &RecordPage::onHotkeySplitRecording);
    connect(hotkey_service_, &GlobalHotkeyService::bindingChanged, this, &MainWindow::onHotkeyServiceBindingChanged);
    // NOTE: record_page_→config_page_ audioMeterLevelsUpdated direct connect is wired in buildConfigPage()
    // after config_page_ is built, so the receiver pointer is always valid when the connect is made.

    // Re-apply the selected preset once the deferred coordinator init completes.
    // initCoordinator() resets audio rows and enumerates targets, clobbering the
    // preset applied in the constructor.  This connection restores the exact audio
    // rows and capture target from the preset after all init work is done.
    connect(record_page_, &RecordPage::coordinatorInitialized, this,
            [this]() { applyPresetConfig(boot_live_config_); });

    // F1 hardening (feat/updater-swap): wire the real RecordingCoordinator into
    // UpdateService now that RecordPage has built it, so LaunchUpdater's
    // recording/paused/preparing/countdown/armed-from-recovery/saving/stopping
    // guard actually enforces (it was dead code while nullptr was passed above).
    connect(record_page_, &RecordPage::coordinatorInitialized, this, [this]() {
        if (update_service_ && record_page_)
            update_service_->SetRecordingCoordinator(record_page_->recordingCoordinator());
    });

    // NOTE: config_page_ diagnosticsRequested connect wired in buildConfigPage().
    // NOTE: diagnostics_page_ navigateToLogsRequested and diagnosticsUpdated (direct connect)
    // are wired in buildDiagnosticsPage() after deferred construction.

    // ---- Countdown overlay (COUNTDOWN-OVERLAY-R1) ----
    // Top-level (no Qt parent) like the other overlays; centered on the recorded monitor.
    countdown_overlay_ = new ui::overlay::CountdownOverlayWindow(nullptr);
    connect(record_page_, &RecordPage::countdownStateChanged, this, &MainWindow::onCountdownStateChanged);

    // ---- Recording overlay (RECORDING-OVERLAY-R1) ----
    // Overlay window is top-level (no Qt parent) so it is not clipped by MainWindow.
    recording_overlay_ = new ui::overlay::RecordingOverlayWindow(nullptr);
    // NOTE: config_page_->setShowOverlay + showOverlayChanged connect are wired in buildConfigPage().
    // When the recorded monitor geometry changes (target switch / recording start), update position.
    connect(record_page_, &RecordPage::recordingMonitorGeometryChanged, this, [this](const QRect& rect) {
        recording_monitor_rect_ = rect;
        if (countdown_overlay_)
            countdown_overlay_->setMonitorGeometry(rect);
        if (recording_overlay_)
            recording_overlay_->setMonitorGeometry(rect);
        if (diagnostics_overlay_)
            diagnostics_overlay_->setMonitorGeometry(rect);
    });

    // ---- Diagnostics overlay (DIAGNOSTICS-OVERLAY-R1) ----
    // Top-level (no Qt parent) like RecordingOverlayWindow; bottom-right corner.
    diagnostics_overlay_ = new ui::overlay::DiagnosticsOverlayWindow(nullptr);
    // NOTE: config_page_->setShowDiagnosticsOverlay + showDiagnosticsOverlayChanged connect
    // are wired in buildConfigPage().
    // Feed the diagnostics overlay from chromeRuntimeMetricsChanged (~4–30 Hz stats cadence).
    connect(record_page_, &RecordPage::chromeRuntimeMetricsChanged, this,
            [this](const QString& /*elapsed*/, const QString& bitrate_text, const QString& drop_text,
                   const QString& size_text, double av_drift_ms) {
                if (diagnostics_overlay_ && diagnostics_overlay_->isVisible()) {
                    // Format A/V drift: "+12 ms" / "-8 ms"; "—" when zero/unknown.
                    QString drift_text;
                    if (av_drift_ms == 0.0) {
                        drift_text = QStringLiteral("—");
                    } else {
                        const int drift_rounded = static_cast<int>(std::round(av_drift_ms));
                        drift_text = drift_rounded >= 0 ? QStringLiteral("+%1 ms").arg(drift_rounded)
                                                        : QStringLiteral("%1 ms").arg(drift_rounded);
                    }
                    // fps is embedded in bitrate_text as "fps / bitrate"; reuse directly.
                    diagnostics_overlay_->updateMetrics(bitrate_text, // fps / bitrate line
                                                        drift_text,   // A/V drift
                                                        drop_text,    // dropped frames count
                                                        size_text,    // output file size
                                                        false, // mic_muted: derived from audioMeterLevelsUpdated below
                                                        false  // sys_muted: derived from audioMeterLevelsUpdated below
                    );
                }
            });
    // Feed muted-source glyphs from the audio meter signal.
    connect(
        record_page_, &RecordPage::audioMeterLevelsUpdated, this,
        [this](float /*sys01*/, float /*app01*/, float /*mic01*/, bool sys_active, bool app_active, bool mic_active) {
            if (diagnostics_overlay_ && diagnostics_overlay_->isVisible()) {
                // "muted" means the source is not active during recording.
                const bool mic_muted = !mic_active;
                const bool sys_muted = !sys_active;
                diagnostics_overlay_->updateMetrics(diagnostics_overlay_->fpsBitrateText(),
                                                    diagnostics_overlay_->avDriftText(),
                                                    diagnostics_overlay_->droppedFramesText(),
                                                    diagnostics_overlay_->outputSizeText(), mic_muted, sys_muted);
            }
            Q_UNUSED(app_active);
        });

    // ---- Notification toasts (NOTIFY-TOASTS-R1) ----
    initNotificationToasts();

    // ---- Close-to-tray toggle (TRAY-CLOSE-TO-TRAY-R1) ----
    // NOTE: config_page_ setKeepRunningInTray + keepRunningInTrayChanged connect wired in buildConfigPage().

    // ---- Quick-control pill (QUICK-PILL-R1) ----
    // Top-level window (no Qt parent) so it is not clipped by MainWindow.
    // Interactive + capture-excluded: does NOT carry Qt::WindowTransparentForInput.
    quick_control_pill_ = new ui::overlay::QuickControlPillWindow(nullptr);
    // NOTE: config_page_ setShowQuickControls + showQuickControlsChanged connect wired in buildConfigPage().
    // Propagate persisted setting to the pill immediately.
    quick_control_pill_->setShowQuickControls(persisted_settings_.show_quick_controls);
    // ---- Theme picker (THEME-SLICE-1) ----
    // NOTE: config_page_ setThemeId + themeIdChanged connect wired in buildConfigPage().
    // Apply the persisted theme on startup (no-op if dark-default since
    // ApplyExoSnapTheme already used it; called unconditionally for non-default).
    if (persisted_settings_.theme_id != QStringLiteral("dark-default")) {
        ui::theme::ReapplyTheme(*qApp, persisted_settings_.theme_id);
        // Refresh wordmarks that baked colours at construction with the default theme.
        // about_page_ is deferred: buildAboutPage() applies refreshBrand() when the
        // non-default theme is active, so no call is needed here.
        if (title_bar_)
            title_bar_->refreshBrand();
    }
    // NOTE: config_page_ themeIdChanged connect is wired in buildConfigPage().

    // Wire pill buttons to the existing recording actions on RecordPage.
    connect(quick_control_pill_, &ui::overlay::QuickControlPillWindow::pauseResumeRequested, record_page_,
            &RecordPage::onHotkeyPauseToggle);
    connect(quick_control_pill_, &ui::overlay::QuickControlPillWindow::stopRequested, record_page_, [this]() {
        // QUICK-PILL-R1: stop button — same path as TransportDock::stopClicked.
        // RecordPage::onStop() is private; use the hotkey toggle signal which routes
        // to onHotkeyToggle → onStop() when recording is active.
        // Actually, emit recordToggleRequested which calls onHotkeyToggle.
        // onHotkeyToggle in turn calls onStop() when CanStop().
        emit recordToggleRequested();
    });
    connect(quick_control_pill_, &ui::overlay::QuickControlPillWindow::captureFrameRequested, record_page_,
            &RecordPage::onHotkeyCaptureFrame);

    // ---- Reactive device discovery wiring ----
    // Audio: forward to both ConfigPage and RecordPage (under no-emit contract).
    connect(&audio_notifier_, &AudioDeviceNotifier::snapshotChanged, this, &MainWindow::onAudioDevicesChanged);
    // Webcam: forward to ConfigPage (which forwards to WebcamSetupPanel) and RecordPage.
    connect(&webcam_notifier_, &WebcamDeviceNotifier::snapshotChanged, this, &MainWindow::onWebcamDevicesChanged);
    // Display: replaces the old QGuiApplication::screenAdded/Removed lambdas.
    // DisplayDeviceNotifier covers add/remove AND geometry/DPI changes.
    connect(&display_notifier_, &DisplayDeviceNotifier::snapshotChanged, this, &MainWindow::onDisplaysChanged);

    // NOTE: config_page_ audioRescanRequested, update-card setters/connects, presentDiagnosticsOptIn,
    // and WebcamSetupPanel rescanRequested are all wired in buildConfigPage().

    // ---- Tray icon (TRAY-PRESENCE-R1) ----
    // TrayPresence is parented to this so it is torn down with MainWindow.
    // It must be created before rebroadcastChromeState() so the initial state
    // signal reaches it.
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_presence_ = new ui::tray::TrayPresence(this);
        // Route tray activate → show/raise the window.
        connect(tray_presence_, &ui::tray::TrayPresence::activateWindowRequested, this,
                &MainWindow::onTrayActivateWindow);
        // Route tray record toggle → same slot as the global hotkey.
        connect(tray_presence_, &ui::tray::TrayPresence::recordToggleRequested, this,
                &MainWindow::recordToggleRequested);
        // Route tray quit → force-close (bypass close-to-tray; closeEvent handles recording guard).
        // TRAY-CLOSE-TO-TRAY-R1: set force_quit_ so closeEvent knows this is a real quit, not a hide.
        connect(tray_presence_, &ui::tray::TrayPresence::quitRequested, this, [this]() {
            force_quit_ = true;
            close();
        });
        // Wire elapsed text updates into the tray tooltip at the existing cadence
        // (chromeRuntimeMetricsChanged fires ~30 Hz via MeterCallback when recording).
        connect(record_page_, &RecordPage::chromeRuntimeMetricsChanged, this,
                [this](const QString& elapsed, const QString& /*bitrate*/, const QString& /*drop_text*/,
                       const QString& /*size*/, double /*av_drift_ms*/) {
                    if (tray_presence_)
                        tray_presence_->updateElapsedText(elapsed);
                });
        tray_presence_->setWindowVisible(isVisible());
        tray_presence_->show();

        // NOTIFY-SKIN-R1: wire unread badge — incremented when an actionable toast is shown.
        // Cleared when the user activates the window (focuses it) or opens the tray menu
        // (handled in onTrayActivateWindow and via the Notifications action in TrayPresence).
        // The notification_manager_ is created later in initNotificationToasts(); wire is
        // deferred there so the manager pointer is valid.
    }

    record_page_->rebroadcastChromeState();
    // Apply the boot live config to all pages.
    applyPresetConfig(boot_live_config_);

    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("MainWindow constructed"));
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("mainwindow-ctor-end %1 ms").arg(diagnostics::StartupClock().elapsed()));

    navigateToPage(kRecordPageIndex);

    auto* fullscreen_shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreen_shortcut->setContext(Qt::ApplicationShortcut);
    connect(fullscreen_shortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);

    // Application-level filter to catch mouse-press events in the resize border
    // regardless of which child widget lies under the cursor.  Resize zones are
    // HTCLIENT, so WM_NCLBUTTONDOWN never fires for them; we handle them here.
    qApp->installEventFilter(this);

    QTimer::singleShot(0, this, [this]() {
        // Warm-start: hydrate Diagnostics/Device from the last known-good capability
        // snapshot before the real, off-thread probe below even starts, so those pages
        // are not blank during its cold window. The adapter-identity read is cheap
        // (DXGI only — no NVENC session, no Media Foundation), so this stays synchronous.
        // A cache hit is NEVER delivered to record_page_ / the coordinator: it is rebuilt
        // via BuildEffectiveCapabilities() directly, which leaves CapabilitySet::probed
        // false, and only onRuntimeCapsReady()'s freshly probed set (below) is ever handed
        // to RecordPage::setRuntimeCapabilities() — the sole path that can unlock a
        // recording-start decision. See CapabilityCacheStore's doc comment.
        capability_cache_key_ = capability::BuildCapabilityCacheKey(
            capability::CapabilityBuilder::QueryAdapterIdentity(), exosnap::build::kVersion);
        if (auto warm_snapshot = capability_cache_.LoadMatching(capability_cache_key_)) {
            runtime_caps_ = capability::CapabilityBuilder::BuildEffectiveCapabilities(*warm_snapshot);
            runtime_caps_ready_ = true;
            diagnostics::AppLog::info(
                QStringLiteral("perf"),
                QStringLiteral("caps-warm-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
            if (device_page_)
                device_page_->setCapabilitySet(runtime_caps_);
            refreshDiagnosticsData();
        }

        // Run the hardware capability probe on a worker thread so tick-0 does not
        // stall the UI. onRuntimeCapsReady() is invoked on the main thread via
        // QueuedConnection when the probe completes; it also starts the device
        // notifiers (which must follow caps so the first snapshot has context).
        diagnostics::AppLog::info(QStringLiteral("perf"),
                                  QStringLiteral("caps-probe-start %1 ms").arg(diagnostics::StartupClock().elapsed()));
        QThread* worker = QThread::create([this]() {
            // Exception barrier: QueryRuntimeFacts() allocates and has no top-level
            // noexcept guarantee, so an escaped throw here would abort the QThread and
            // std::terminate the app. Catch it and post the failure to the UI thread so
            // coordinator init resolves (to a failure state) instead of hanging armed.
            try {
                capability::CapabilitySet caps = capability::CapabilityBuilder::BuildFromHardwareQuery();
                QMetaObject::invokeMethod(this, [this, caps]() { onRuntimeCapsReady(caps); }, Qt::QueuedConnection);
            } catch (const std::exception& ex) {
                const QString reason = QString::fromUtf8(ex.what());
                QMetaObject::invokeMethod(
                    this, [this, reason]() { onRuntimeCapsFailed(reason); }, Qt::QueuedConnection);
            } catch (...) {
                QMetaObject::invokeMethod(
                    this, [this]() { onRuntimeCapsFailed(QStringLiteral("Unknown error")); }, Qt::QueuedConnection);
            }
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();

        // Startup crash-recovery: show the overlay if interrupted recordings exist.
        checkAndShowRecoveryOverlay();
        // CRASH-WIRE-R1: next-launch crash dialog (deferred behind recovery).
        checkAndShowCrashReportOverlay();
        // WHATS-NEW: one-time post-update overlay (deferred behind recovery/crash).
        checkAndShowWhatsNewOverlay();

        // UPDATE-WIRE-R1 (ADR 0012): auto-check for updates on startup, guarded so we
        // never contact the update server while a recording/finalize is in flight.
        // ADR 0045: check_updates_on_start defaults to false, so this line performs
        // no network contact on a first launch — the user must opt in from the
        // Settings update card first. IsUpdateCheckEnabled() (a separate, compile-time
        // gate in libs/update) additionally keeps self-built binaries from ever
        // phoning home even if this setting were somehow turned on.
        if (persisted_settings_.check_updates_on_start && !recording_active_ && !remuxing_active_)
            triggerUpdateCheck();
    });

    // PERF-B1: the deferred pages are hydrated AFTER the first paint (scheduled from
    // MainWindow::paintEvent), not on the first event-loop tick. The first hydrate tick
    // builds ConfigPage (~1.5 s under the global QSS) synchronously; a ctor-time
    // singleShot(0) would run that BEFORE the first paint, blocking the window from
    // appearing. Triggering hydrate post-paint lets the window appear first (~1.25 s).
}

void MainWindow::onRuntimeCapsReady(capability::CapabilitySet caps) {
    runtime_caps_ = std::move(caps);
    runtime_caps_ready_ = true;
    // Warm-start: overwrite the disk cache with THIS run's freshly probed answer so the
    // next launch can hydrate from it (see CapabilityCacheStore's doc comment). Cheap
    // JSON write; done here on the UI thread rather than the probe's worker thread so it
    // never races MainWindow teardown.
    capability_cache_.Save(runtime_caps_.runtime, capability_cache_key_);
    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("capabilities probed (async)"));
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("caps-probe-end %1 ms").arg(diagnostics::StartupClock().elapsed()));
    if (record_page_)
        record_page_->setRuntimeCapabilities(runtime_caps_); // delivers caps to coordinator (A1 gate)
    if (device_page_)
        device_page_->setCapabilitySet(runtime_caps_); // static bit-depth/rate-control facts for the matrix
    if (config_page_)
        config_page_->setRuntimeCapabilities(runtime_caps_); // per-GPU 4:4:4 chroma gate
    refreshPresetUi();
    refreshDiagnosticsData();
    startDeviceNotifiers();

    // S4: Gate webcam UI when MF is absent (Windows-N without the Media Feature Pack).
    // The embedded WebcamSetupPanel (Settings) applies the gate in buildConfigPage().
    if (!runtime_caps_.mf_webcam_available) {
        diagnostics::AppLog::warning(QStringLiteral("window"),
                                     QStringLiteral("mfplat.dll absent — webcam UI disabled (Windows-N)"));
        // config_page_ / WebcamSetupPanel: gate is applied in buildConfigPage().
    }
}

void MainWindow::onRuntimeCapsFailed(const QString& reason) {
    // The async HW probe threw. Mark the probe as resolved (it completed, just failed)
    // so nothing waits on it forever, drive the Record page into its capability-failure
    // state, and still bring up the rest of the UI (device notifiers) so navigation works.
    runtime_caps_ready_ = true;
    diagnostics::AppLog::error(QStringLiteral("window"),
                               QStringLiteral("capability probe failed (async): %1").arg(reason));
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("caps-probe-end %1 ms").arg(diagnostics::StartupClock().elapsed()));
    if (record_page_)
        record_page_->setRuntimeCapabilitiesFailed(reason);
    refreshPresetUi();
    refreshDiagnosticsData();
    startDeviceNotifiers();
}

void MainWindow::startDeviceNotifiers() {
    // Start the device notifiers after the capability probe resolves so the first
    // snapshotChanged emission has the correct runtime context (preserving the original
    // ordering). rescan() seeds the initial availability state synchronously so pages
    // know the device state without waiting for a native event.
    audio_notifier_.start();
    audio_notifier_.rescan();
    webcam_notifier_.start();
    webcam_notifier_.rescan();
    display_notifier_.start();
    display_notifier_.rescan();
    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("device notifiers started"));
}

void MainWindow::checkAndShowRecoveryOverlay() {
    const auto candidates = recovery_service_.Scan();
    if (candidates.isEmpty())
        return;

    diagnostics::AppLog::info(
        QStringLiteral("recovery"),
        QStringLiteral("Found %1 interrupted recording(s) — showing recovery overlay").arg(candidates.size()));

    // Trigger 4: RecoveryAvailable — "Recover last session?" (standing) with
    // "Recover" (primary) + "Discard". Enqueue() records it in the hub and, when
    // toasts are enabled, raises the standing toast.
    if (notification_manager_) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::RecoveryAvailable;
        event.title = QStringLiteral("Recover last session?");
        event.body =
            (candidates.size() == 1)
                ? QStringLiteral("A recording from the last session wasn’t finalized.")
                : QStringLiteral("%1 recordings from the last session weren’t finalized.").arg(candidates.size());
        event.action = notifications::NotificationAction::OpenRecovery;
        event.secondary_action = notifications::NotificationAction::Discard;
        notification_manager_->Enqueue(std::move(event));
    }

    // Parent to the central widget (same pattern as source_picker_overlay_).
    // The overlay should survive page navigation (not closed by navigateToPage); it is
    // deliberately excluded from the navigateToPage close-list because recovery must
    // remain visible regardless of which settings page the user switches to.
    auto* central = centralWidget();
    recovery_overlay_ = new ui::dialogs::RecoveryOverlay(recovery_service_, candidates, central);
    recovery_overlay_->hide();
    connect(recovery_overlay_, &ui::dialogs::RecoveryOverlay::closed, this, [this]() {
        // Dismiss = "decide later". Entries stay in the manifest for next startup.
        recovery_overlay_->deleteLater();
        recovery_overlay_ = nullptr;
    });
    // ADR-0015: wire "Continue" to the coordinator via RecordPage.
    connect(recovery_overlay_, &ui::dialogs::RecoveryOverlay::continueRequested, record_page_,
            &RecordPage::armFromRecovery);
    recovery_overlay_->openOverlay();
}

void MainWindow::openWhatsNewOverlay(const QVector<WhatsNewNote>& notes, bool post_update_mode) {
    if (notes.isEmpty())
        return;

    // One overlay at a time; replace any existing instance. closeOverlay() emits
    // closed() synchronously, and the closed-handler connected below reads the
    // whats_new_overlay_ *member* (not a captured pointer) to null it out. So:
    // take a local pointer first, null the member, then disconnect the old
    // overlay's signals (so its own closed-handler can't fire against the
    // now-null member) before closing + deleting it through the local pointer.
    if (whats_new_overlay_ != nullptr) {
        auto* old_overlay = whats_new_overlay_;
        whats_new_overlay_ = nullptr;
        old_overlay->disconnect(this);
        old_overlay->closeOverlay();
        old_overlay->deleteLater();
    }

    const QString releases_url = last_update_releases_url_.isEmpty()
                                     ? QStringLiteral("https://github.com/Exoridus/exosnap/releases")
                                     : last_update_releases_url_;

    auto* central = centralWidget();
    whats_new_overlay_ = new ui::dialogs::WhatsNewOverlay(notes, post_update_mode, releases_url, central);
    whats_new_overlay_->hide();
    connect(whats_new_overlay_, &ui::dialogs::WhatsNewOverlay::closed, this, [this]() {
        whats_new_overlay_->deleteLater();
        whats_new_overlay_ = nullptr;
    });
    // Post-update mode: the suppress checkbox persists whats_new_suppressed.
    connect(whats_new_overlay_, &ui::dialogs::WhatsNewOverlay::suppressToggled, this, [this](bool suppressed) {
        persisted_settings_.whats_new_suppressed = suppressed;
        settings_store_.Save(persisted_settings_);
    });
    whats_new_overlay_->openOverlay();
}

void MainWindow::checkAndShowWhatsNewOverlay() {
    // Consume the pending payload written by LaunchUpdater. It only shows when the
    // running build equals the payload's target and notices aren't suppressed —
    // first install / downgrade / manual-ZIP update leave no matching payload, so
    // in every one of those cases the stale payload is simply cleared without a UI.
    const QString payload_path = WhatsNewPayloadPath();
    const auto payload = ReadWhatsNewPayload(payload_path);
    if (!payload.has_value()) {
        // File exists but doesn't parse (or is absent). Best-effort delete is a
        // no-op when there's nothing there, and clears a corrupt payload so it
        // isn't re-read (and re-fail) on every subsequent launch.
        DeleteWhatsNewPayload(payload_path);
        return;
    }

    const QString running_version = QString::fromLatin1(exosnap::build::kVersion);
    const bool show = ShouldShowWhatsNew(payload, running_version, persisted_settings_.whats_new_suppressed);

    // One-time: always clear the payload once we've decided (shown or stale, e.g.
    // target-version mismatch).
    DeleteWhatsNewPayload(payload_path);
    if (!show)
        return;

    QVector<WhatsNewNote> notes = payload->notes;
    // Defer behind any recovery/crash overlay so nothing double-stacks: if one is
    // open, show once it closes; otherwise show now.
    //
    // Startup call order (see the ctor): checkAndShowRecoveryOverlay(), then
    // checkAndShowCrashReportOverlay(), then this function. When recovery is NOT
    // open, checkAndShowCrashReportOverlay() has already run synchronously by the
    // time we get here, so crash_overlay_ below already reflects its final state.
    //
    // When recovery IS open, checkAndShowCrashReportOverlay() couldn't make its
    // own decision yet either — it deferred to recovery_overlay_::closed too, and
    // its connection was made before ours (it runs first). So on recovery.closed,
    // Qt invokes the crash continuation first, then ours. If our continuation
    // opened the What's New overlay directly at that point, it would race the
    // crash continuation: both overlays could end up open at once (crash_overlay_
    // may not be fully constructed as the whats-new slot begins running, or a
    // future refactor could reorder the connects). To make the ordering
    // guarantee explicit rather than implicit-via-connect-order, hop one more
    // event-loop tick with QTimer::singleShot(0) before re-checking crash_overlay_
    // in showWhatsNewAfterStartupOverlays() — that guarantees the crash
    // continuation has fully run (overlay constructed and shown, or decided not
    // to run at all) before we decide whether to stack behind it.
    if (recovery_overlay_ != nullptr && recovery_overlay_->isOpen()) {
        connect(
            recovery_overlay_, &ui::dialogs::RecoveryOverlay::closed, this,
            [this, notes]() {
                QTimer::singleShot(0, this, [this, notes]() { showWhatsNewAfterStartupOverlays(notes); });
            },
            Qt::SingleShotConnection);
        return;
    }
    showWhatsNewAfterStartupOverlays(notes);
}

void MainWindow::showWhatsNewAfterStartupOverlays(const QVector<WhatsNewNote>& notes) {
    if (crash_overlay_ != nullptr) {
        connect(
            crash_overlay_, &ui::dialogs::CrashReportOverlay::closed, this,
            [this, notes]() { openWhatsNewOverlay(notes, /*post_update_mode=*/true); }, Qt::SingleShotConnection);
        return;
    }
    openWhatsNewOverlay(notes, /*post_update_mode=*/true);
}

namespace {

// Compact container/codec tokens for the session sidecar + crash report.
// The capability ToString() helpers are verbose ("Matroska", "AV1 NVENC",
// "AAC"); the crash facts want short, allowlisted tokens.
// (Prefixed Crash* to avoid colliding with the std::wstring ContainerToken in
// RecordingPreset.h used for filename building.)
std::string CrashContainerToken(capability::Container c) {
    switch (c) {
    case capability::Container::Matroska:
        return "MKV";
    case capability::Container::Mp4:
        return "MP4";
    case capability::Container::WebM:
        return "WebM";
    }
    return "MKV";
}

std::string CrashVideoCodecToken(capability::VideoCodec v) {
    switch (v) {
    case capability::VideoCodec::Av1:
        return "AV1";
    case capability::VideoCodec::Hevc:
        return "HEVC";
    case capability::VideoCodec::H264:
        return "H.264";
    }
    return "AV1";
}

std::string CrashAudioCodecToken(capability::AudioCodec a) {
    switch (a) {
    case capability::AudioCodec::Opus:
        return "Opus";
    case capability::AudioCodec::Aac:
        return "AAC";
    case capability::AudioCodec::Pcm:
        return "PCM";
    case capability::AudioCodec::Flac:
        return "FLAC";
    }
    return "Opus";
}

} // namespace

crash_capture::SessionContext MainWindow::currentSessionContext() const {
    crash_capture::SessionContext ctx;
    ctx.app_version = exosnap::build::kVersion;
    // All NVENC video codecs ship today; the encoder backend baseline is nvenc.
    ctx.encoder_backend = "nvenc";
    ctx.container = CrashContainerToken(output_settings_.container);
    ctx.video_codec = CrashVideoCodecToken(output_settings_.video_codec);
    ctx.audio_codec = CrashAudioCodecToken(output_settings_.audio_codec);
    return ctx;
}

void MainWindow::refreshCrashSessionContext() {
    if (crash_dir_.empty())
        return;
    const crash_capture::SessionContext ctx = currentSessionContext();
    crash_capture::UpdateSessionContext(crash_dir_, ctx);
    crash_capture::SetEncoderContext(ctx.encoder_backend, ctx.container, ctx.video_codec, ctx.audio_codec);
}

void MainWindow::checkAndShowCrashReportOverlay() {
    if (!pending_crash_)
        return;

    const CrashPromptDisposition disposition = ResolveCrashPromptDisposition(persisted_settings_.crash_report_policy);
    if (disposition == CrashPromptDisposition::SuppressAndSend) {
        diagnostics::AppLog::info(
            QStringLiteral("crash"),
            QStringLiteral("Auto-send enabled — consent granted silently; crash dialog suppressed"));
        return;
    }
    if (disposition == CrashPromptDisposition::SuppressWithoutSend) {
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Crash-report policy is Never send; consent prompt suppressed"));
        return;
    }

    // If the recovery overlay is currently open, defer behind it (no double-prompt).
    if (recovery_overlay_ != nullptr && recovery_overlay_->isOpen()) {
        connect(
            recovery_overlay_, &ui::dialogs::RecoveryOverlay::closed, this, [this]() { openCrashReportOverlay(); },
            Qt::SingleShotConnection);
        return;
    }

    openCrashReportOverlay();
}

void MainWindow::openCrashReportOverlay() {
    if (!pending_crash_ || crash_overlay_ != nullptr)
        return;

    // A crash mid-recording leaves a recovery candidate behind.
    const bool recording_was_active = !recovery_service_.Scan().isEmpty();

    ui::dialogs::CrashReportModel model;
    model.recording_was_active = recording_was_active;

    // The sidecar identifies the previous session version. Do not substitute
    // the currently running build or current-machine probe results as though
    // they were facts captured at the abnormal shutdown.
    model.version = QString::fromStdString(pending_crash_->app_version);

    // Encoder: "<BACKEND> <video> → <container>" e.g. "NVENC AV1 → MKV".
    const QString backend = QString::fromStdString(pending_crash_->encoder_backend).toUpper();
    const QString vcodec = QString::fromStdString(pending_crash_->video_codec);
    const QString container = QString::fromStdString(pending_crash_->container);
    QStringList encoder_parts;
    if (!backend.isEmpty())
        encoder_parts << backend;
    if (!vcodec.isEmpty())
        encoder_parts << vcodec;
    QString encoder = encoder_parts.join(QStringLiteral(" "));
    if (!container.isEmpty())
        encoder += QStringLiteral(" → ") + container;
    model.encoder = encoder.trimmed();

    // exception/module/thread/stack are intentionally empty: the client does not
    // symbolicate — the .dmp holds the rest and Sentry resolves stacks via PDB.
    model.crash_dir = QString::fromStdString(crash_dir_);

    // Best-effort newest *.dmp under the crash dir (may be empty in stub builds).
    QDir dir(QString::fromStdString(crash_dir_));
    if (dir.exists()) {
        const QFileInfoList dumps = dir.entryInfoList({QStringLiteral("*.dmp")}, QDir::Files, QDir::Time);
        if (!dumps.isEmpty())
            model.dmp_path = dumps.first().absoluteFilePath();
    }

    crash_overlay_ = new ui::dialogs::CrashReportOverlay(model, centralWidget());
    crash_overlay_->hide();

    connect(crash_overlay_, &ui::dialogs::CrashReportOverlay::sendReportRequested, this, [this]() {
        const bool remember = crash_overlay_ && crash_overlay_->rememberChoiceChecked();
        const CrashReportDecision decision = ResolveCrashReportDecision(CrashReportAction::SendReport, remember);
        if (decision.persisted_policy.has_value()) {
            persisted_settings_.crash_report_policy = *decision.persisted_policy;
            settings_store_.Save(persisted_settings_);
        }
        const bool delivered = applyCrashConsentAction(decision.consent_action);
        diagnostics::AppLog::info(
            QStringLiteral("crash"),
            delivered ? QStringLiteral("User granted crash-report consent; pending report released")
                      : QStringLiteral("User granted one-shot crash-report consent, but the transport did not flush"));
        if (crash_overlay_)
            crash_overlay_->closeOverlay();
    });

    connect(crash_overlay_, &ui::dialogs::CrashReportOverlay::openCrashFolderRequested, this,
            [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(crash_dir_))); });

    connect(crash_overlay_, &ui::dialogs::CrashReportOverlay::dontSendRequested, this, [this]() {
        const bool remember = crash_overlay_ && crash_overlay_->rememberChoiceChecked();
        const CrashReportDecision decision = ResolveCrashReportDecision(CrashReportAction::DontSend, remember);
        if (decision.persisted_policy.has_value()) {
            persisted_settings_.crash_report_policy = *decision.persisted_policy;
            settings_store_.Save(persisted_settings_);
        }
        applyCrashConsentAction(decision.consent_action);
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  remember ? QStringLiteral("User declined and disabled future crash-report prompts")
                                           : QStringLiteral("User declined this crash report"));
    });

    connect(crash_overlay_, &ui::dialogs::CrashReportOverlay::closed, this, [this]() {
        if (crash_overlay_) {
            crash_overlay_->deleteLater();
            crash_overlay_ = nullptr;
        }
    });

    crash_overlay_->openOverlay();
}

void MainWindow::openRecordingErrorOverlay(ui::dialogs::RecordingErrorModel model) {
    // Replace any overlay still on screen from a previous failed attempt.
    if (recording_error_overlay_ != nullptr) {
        recording_error_overlay_->closeOverlay();
        recording_error_overlay_->deleteLater();
        recording_error_overlay_ = nullptr;
    }

    // The "Send report" action is only meaningful in an official build with a
    // compiled-in DSN and an active engine; self-builds never phone home.
    model.can_send_report = crash_capture::IsActive();

    recording_error_overlay_ = new ui::dialogs::RecordingErrorOverlay(model, centralWidget());
    recording_error_overlay_->hide();

    connect(recording_error_overlay_, &ui::dialogs::RecordingErrorOverlay::sendReportRequested, this, [this, model]() {
        // Clicking Send is the explicit opt-in: grant consent (dormant w/o
        // DSN), attach allow-listed codec context, then forward a scrubbed,
        // non-fatal report. Paths in `detail` are stripped inside crash_capture.
        crash_capture::GiveUserConsent();
        crash_capture::SetEncoderContext("nvenc", model.container.toStdString(), model.video_codec.toStdString(),
                                         model.audio_codec.toStdString());
        crash_capture::ReportNonFatalError(model.phase.toStdString(), model.detail.toStdString());
        diagnostics::AppLog::info(QStringLiteral("record.failure"),
                                  QStringLiteral("user sent error report phase=%1").arg(model.phase));
        if (recording_error_overlay_)
            recording_error_overlay_->closeOverlay();
    });

    connect(recording_error_overlay_, &ui::dialogs::RecordingErrorOverlay::openLogsRequested, this, [this]() {
        if (recording_error_overlay_)
            recording_error_overlay_->closeOverlay();
        navigateToPage(kLogsPageIndex);
    });

    connect(recording_error_overlay_, &ui::dialogs::RecordingErrorOverlay::closed, this, [this]() {
        if (recording_error_overlay_) {
            recording_error_overlay_->deleteLater();
            recording_error_overlay_ = nullptr;
        }
    });

    recording_error_overlay_->openOverlay();
}

MainWindow::~MainWindow() {
    // Stop notifiers FIRST, before any pages are torn down, so no late callback
    // fires into a partially-destroyed page.  The notifiers also stop in their own
    // destructors, but explicit ordering here prevents a race with the Qt object
    // tree teardown.
    audio_notifier_.stop();
    webcam_notifier_.stop();
    display_notifier_.stop();

    // The overlays are top-level (no Qt parent); destroy them explicitly so they
    // don't outlive the application shutdown.
    delete countdown_overlay_;
    countdown_overlay_ = nullptr;
    delete recording_overlay_;
    recording_overlay_ = nullptr;
    delete diagnostics_overlay_;
    diagnostics_overlay_ = nullptr;

    // NOTIFY-TOASTS-R1: toast window is also top-level; destroy explicitly.
    delete notification_toast_window_;
    notification_toast_window_ = nullptr;

    // PS-PHASE-B: hub panel is also top-level (no Qt parent); destroy explicitly.
    delete notification_hub_;
    notification_hub_ = nullptr;
}

void MainWindow::toggleNotificationHub() {
    if (!notification_hub_)
        return;
    if (hub_just_dismissed_) {
        // This very click already closed the hub via the popup auto-dismiss;
        // swallow the toggle so a single click just closes it.
        hub_just_dismissed_ = false;
        return;
    }
    if (notification_hub_->isVisible()) {
        notification_hub_->hide();
        return;
    }
    if (!title_bar_->bellWidget())
        return;
    // Anchor: bottom-right corner of the bell widget, mapped to global coordinates.
    const QWidget* bell = title_bar_->bellWidget();
    const QPoint bell_bottom_right = bell->mapToGlobal(QPoint(bell->width(), bell->height()));
    notification_hub_->anchorToPoint(bell_bottom_right + QPoint(0, 4));
    notification_hub_->show();
    notification_hub_->raise();
    title_bar_->bellWidget()->setHubOpen(true);
}

void MainWindow::updateRecordingOverlay() {
    if (!recording_overlay_)
        return;

    // Overlay is disabled by user setting.
    if (!persisted_settings_.show_recording_overlay) {
        recording_overlay_->hideOverlay();
        return;
    }

    const bool is_recording = (record_status_label_ == QStringLiteral("REC"));
    const bool is_paused = (record_status_label_ == QStringLiteral("PAUSED"));

    if (is_recording) {
        // Show recording state; elapsed text is provided via the timer label in
        // TransportDock — for the overlay we synthesise a placeholder on first show
        // and update via chromeRuntimeMetricsChanged.
        recording_overlay_->showRecording(QStringLiteral("00:00:00"));
    } else if (is_paused) {
        recording_overlay_->showPaused(recording_overlay_->elapsedText());
    } else {
        recording_overlay_->hideOverlay();
    }
}

void MainWindow::updateDiagnosticsOverlay() {
    if (!diagnostics_overlay_)
        return;

    // Overlay disabled by user setting.
    if (!persisted_settings_.show_diagnostics_overlay) {
        diagnostics_overlay_->hideOverlay();
        return;
    }

    const bool is_recording = (record_status_label_ == QStringLiteral("REC"));
    const bool is_paused = (record_status_label_ == QStringLiteral("PAUSED"));

    if (is_recording || is_paused) {
        diagnostics_overlay_->showOverlay();
    } else {
        diagnostics_overlay_->hideOverlay();
    }
}

void MainWindow::updateQuickControlPill() {
    if (!quick_control_pill_)
        return;

    const bool is_recording = (record_status_label_ == QStringLiteral("REC"));
    const bool is_paused = (record_status_label_ == QStringLiteral("PAUSED"));
    const bool active = is_recording || is_paused;

    quick_control_pill_->updateState(active, is_paused);
}

void MainWindow::onCountdownStateChanged(bool active, int remaining_seconds, int duration_seconds) {
    if (!countdown_overlay_)
        return;

    // Gate on show_recording_overlay — same setting as the REC status pill.
    if (!persisted_settings_.show_recording_overlay) {
        countdown_overlay_->hideOverlay();
        return;
    }

    if (active) {
        // Feed the updated monitor geometry in case it changed since construction.
        countdown_overlay_->setMonitorGeometry(recording_monitor_rect_);
        countdown_overlay_->showCountdown(remaining_seconds, duration_seconds);
    } else {
        countdown_overlay_->hideOverlay();
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);

    if (!geometry_restored_) {
        geometry_restored_ = true;
        applyRestoredGeometry();
    }

#if defined(Q_OS_WIN)
    if (!resizable_style_applied_) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        ensureWin32ResizableStyle(hwnd);
        applyDwmThemedBorder(hwnd);
        resizable_style_applied_ = true;
    }
    if (!hotkeys_registered_) {
        hotkeys_registered_ = true;
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd && hotkey_service_) {
            win32_hotkey_registrar_ = std::make_unique<Win32HotkeyRegistrar>(hwnd);
            const std::vector<HotkeyAction> failed_hotkeys =
                hotkey_service_->SetRegistrar(win32_hotkey_registrar_.get());
            if (!failed_hotkeys.empty()) {
                // Split by provenance BEFORE unsetting (unsetting clears the binding,
                // which would make everything read as "at default" afterwards).
                // Default-vs-default collisions are common environmental noise (another
                // app's own default hotkey, e.g. NVIDIA's Alt+F9 Instant Replay, claimed
                // the combo first) and happen on every launch that app is running —
                // worth logging, not worth interrupting the user about every time. A
                // combo the user deliberately chose is different: it worked when they
                // set it, so losing it now is worth telling them.
                std::vector<HotkeyAction> default_failed;
                std::vector<HotkeyAction> custom_failed;
                for (const HotkeyAction action : failed_hotkeys) {
                    if (hotkey_service_->IsAtDefault(action))
                        default_failed.push_back(action);
                    else
                        custom_failed.push_back(action);
                }

                QStringList names;
                for (const HotkeyAction action : failed_hotkeys)
                    names << GlobalHotkeyService::ActionDisplayName(action);
                diagnostics::AppLog::warning(QStringLiteral("hotkeys"),
                                             QStringLiteral("Hotkey registration failed at startup (in use "
                                                            "elsewhere): %1 (default: %2, user-set: %3)")
                                                 .arg(names.join(QStringLiteral(", ")))
                                                 .arg(default_failed.size())
                                                 .arg(custom_failed.size()));

                // Windows has no API to reveal which process holds the combo, and we
                // cannot steal it — a binding we could not register is dead weight that
                // would silently swallow the key and re-warn every launch. Drop it
                // (UnsetBinding persists the cleared binding via bindingChanged) so the
                // action is cleanly unbound until the user picks a working shortcut,
                // regardless of whether it was a default or a custom choice.
                for (const HotkeyAction action : failed_hotkeys)
                    hotkey_service_->UnsetBinding(action);

                // Only a lost CUSTOM binding gets a notification. A lost default is
                // silently dropped -- the notification routes to Settings → Hotkeys,
                // which isn't useful noise for "something else on this machine also
                // defaults to this combo".
                if (notification_manager_ && !custom_failed.empty()) {
                    QStringList custom_names;
                    for (const HotkeyAction action : custom_failed)
                        custom_names << GlobalHotkeyService::ActionDisplayName(action);
                    const QString joined = custom_names.join(QStringLiteral(", "));
                    const bool plural = custom_failed.size() > 1;
                    notifications::NotificationEvent hotkey_conflict_event;
                    hotkey_conflict_event.type = notifications::NotificationType::HotkeyConflict;
                    hotkey_conflict_event.title = QStringLiteral("Hotkey unavailable");
                    hotkey_conflict_event.body =
                        (plural ? QStringLiteral("%1 were already in use by Windows or another app, so ExoSnap "
                                                 "removed them. Pick new shortcuts to re-enable them.")
                                : QStringLiteral("%1 was already in use by Windows or another app, so ExoSnap "
                                                 "removed it. Pick a new shortcut to re-enable it."))
                            .arg(joined);
                    hotkey_conflict_event.action = notifications::NotificationAction::OpenHotkeys;
                    notification_manager_->Enqueue(std::move(hotkey_conflict_event));
                }
            }
        }
#endif
    }
#endif

    if (!runtime_window_icon_bound_)
        applyRuntimeWindowIcon();

    // Sync the tray "Show/Hide window" label after the window becomes visible.
    if (tray_presence_)
        tray_presence_->setWindowVisible(true);
}

void MainWindow::paintEvent(QPaintEvent* event) {
    QMainWindow::paintEvent(event);
    // PERF-MEASURE: the first real paint = "window interactive". Log start→first-paint
    // once; the DXGI "preview-live" marker is the second data point (preview ~150 ms
    // after showEvent). Benchmark with EXOSNAP_CONFIG_DIR isolated to an empty temp.
    if (!first_paint_logged_) {
        first_paint_logged_ = true;
        const qint64 first_paint_ms = diagnostics::StartupClock().elapsed();
        diagnostics::AppLog::info(QStringLiteral("perf"), QStringLiteral("first-paint %1 ms").arg(first_paint_ms));
        diagnostics::StartupTrace::instance().record(QStringLiteral("first-paint"), first_paint_ms);
        // PERF: hydrate the deferred secondary pages only AFTER the first paint, so a
        // heavy page build (ConfigPage ~1.6 s under the global QSS) never blocks the
        // window from first appearing.
        QTimer::singleShot(0, this, [this]() { hydrateSecondaryPages(); });
    }
}

void MainWindow::applyRuntimeWindowIcon() {
    QIcon runtime_icon = windowIcon();
    if (runtime_icon.isNull())
        runtime_icon = QApplication::windowIcon();

    if (runtime_icon.isNull()) {
        static const QString kAppIconPath = QStringLiteral(":/brand/exosnap-logo-idle.ico");
        if (!QFile::exists(kAppIconPath))
            qWarning().noquote() << "Runtime icon resource missing during showEvent:" << kAppIconPath;
        runtime_icon = QIcon(kAppIconPath);
        if (runtime_icon.isNull())
            qWarning().noquote() << "Runtime icon failed to load during showEvent from:" << kAppIconPath;
    }

    if (runtime_icon.isNull())
        return;

    if (runtime_icon.availableSizes().isEmpty())
        qWarning().noquote() << "Runtime icon loaded, but availableSizes() is empty.";

    setWindowIcon(runtime_icon);

    if (windowHandle() != nullptr) {
        windowHandle()->setIcon(runtime_icon);
    } else {
        qWarning().noquote() << "MainWindow windowHandle() unavailable while applying runtime icon.";
    }

#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr) {
        qWarning().noquote() << "HWND unavailable while applying WM_SETICON fallback.";
        return;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr) {
        qWarning().noquote() << "GetModuleHandleW failed while applying WM_SETICON fallback. error="
                             << static_cast<unsigned long>(GetLastError());
        return;
    }

    HICON small_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));

    if (small_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON fallback failed to load ICON_SMALL from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }

    if (big_icon == nullptr) {
        qWarning().noquote() << "WM_SETICON fallback failed to load ICON_BIG from EXE resources. error="
                             << static_cast<unsigned long>(GetLastError());
    } else {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
#endif

    runtime_window_icon_bound_ = true;
}

void MainWindow::switchRecordingIcon(bool recording, bool paused) {
    // Switch the window/taskbar icon between the idle aperture mark, the coral
    // recording variant, and the amber paused variant. Qt's setWindowIcon updates
    // the title-bar frame; WM_SETICON ensures the taskbar button also updates on
    // Windows. Paused takes precedence over recording.
    //
    // Note: Windows may cache the EXE icon (shown before the app launches) even
    // after WM_SETICON succeeds for the running window. The taskbar *button* icon
    // does update live on Windows 10/11; the taskbar pinned icon and the EXE file
    // icon shown in Explorer do not change at runtime — this is expected behavior.
    static const QString kIdlePath = QStringLiteral(":/brand/exosnap-logo-idle.ico");
    static const QString kRecordingPath = QStringLiteral(":/brand/exosnap-logo-recording.ico");
    static const QString kPausedPath = QStringLiteral(":/brand/exosnap-logo-paused.ico");
    const QString& icon_path = paused ? kPausedPath : (recording ? kRecordingPath : kIdlePath);

    QIcon icon(icon_path);
    if (icon.isNull()) {
        qWarning().noquote() << "switchRecordingIcon: icon load failed from" << icon_path;
        return;
    }

    setWindowIcon(icon);
    if (windowHandle())
        windowHandle()->setIcon(icon);

#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (inst == nullptr)
        return;

    const WORD icon_id =
        paused ? IDI_EXOSNAP_APP_ICON_PAUSED : (recording ? IDI_EXOSNAP_APP_ICON_RECORDING : IDI_EXOSNAP_APP_ICON);
    // LR_DEFAULTCOLOR | LR_SHARED: OS caches per (instance, id, size) tuple — safe for distinct IDs.
    HICON small_icon = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(icon_id), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(inst, MAKEINTRESOURCEW(icon_id), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    if (small_icon)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (big_icon)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
#endif
}

bool MainWindow::effectiveMaximizedState() const {
    return isMaximized() || win32_maximized_ || isFullScreen();
}

void MainWindow::onRecordChromeStateChanged(bool recording, const QString& status_label, const QString& context_text) {
    Q_UNUSED(context_text);
    const bool was_recording = recording_active_;
    recording_active_ = recording;
    record_status_label_ = status_label.trimmed().toUpper();

    // CRASH-WIRE-R1: refresh crash context on the recording-start edge so a crash
    // mid-recording carries the live encoder/output context.
    if (recording && !was_recording) {
        refreshCrashSessionContext();
        // DROP-NOTIFY: start a fresh per-recording real-drop accounting so a prior
        // recording's drops can never leak into this one's "frames dropped" toast.
        last_real_drops_ = 0;
        // ADR 0033 extra-checks: scope present diagnostics to the recorded window's process
        // (0 for Monitor/Region = global), so discard/flip/mode stats reflect the captured
        // source rather than whatever last presented.
        present_provider_.SetTargetProcessId(record_page_ ? record_page_->selectedTargetWindowPid() : 0);
    } else if (!recording && was_recording) {
        present_provider_.SetTargetProcessId(0); // back to global attribution when idle
        // AUDIO-DEGRADED-NOTIFY-R1: the notice describes a live recording condition —
        // clear it at the latest here so it never outlives the session it was raised for,
        // even if the last diagnostics tick before stop still reported it degraded.
        clearAudioSourceDegradedNotification();
    }
    // ADR-0014: track remux-on-stop phase separately so closeEvent can guard it.
    remuxing_active_ = (record_status_label_ == QStringLiteral("SAVING"));
    if (record_status_label_.isEmpty())
        record_status_label_ = QStringLiteral("READY");

    if (config_page_) {
        // The title-bar pill distinguishes the post-recording "Saved" state, but the
        // Settings readiness badge has no Saved concept — map it to the equivalent
        // ready status so Settings behaviour is unchanged from before this slice.
        const QString config_status =
            (record_status_label_ == QStringLiteral("SAVED")) ? QStringLiteral("READY") : record_status_label_;
        config_page_->setReadinessStatus(config_status);
    }

    if (config_page_) {
        const QString upper = record_status_label_;
        const bool locked = (upper == QStringLiteral("REC") || upper == QStringLiteral("PAUSED") ||
                             upper == QStringLiteral("STOPPING") || upper == QStringLiteral("CHECKING") ||
                             upper == QStringLiteral("STARTING") || upper == QStringLiteral("COUNTDOWN"));
        config_page_->setRecordingControlsLocked(locked);
    }

    applyTitleBarStatus();
    switchRecordingIcon(recording_active_, record_status_label_ == QStringLiteral("PAUSED"));

    // Update tray presence (TRAY-PRESENCE-R1).
    if (tray_presence_) {
        const ui::tray::TrayIconState tray_state = ui::tray::TrayIconStateFromStatusLabel(record_status_label_);
        tray_presence_->applyState(tray_state, record_status_label_);
        // Blocked when status is BLOCKED or no recording is possible.
        const bool blocked =
            (record_status_label_ == QStringLiteral("BLOCKED") || record_status_label_ == QStringLiteral("ERROR") ||
             record_status_label_ == QStringLiteral("CHECKING") || record_status_label_ == QStringLiteral("STOPPING") ||
             record_status_label_ == QStringLiteral("SAVING"));
        tray_presence_->setRecordingBlocked(blocked && tray_state == ui::tray::TrayIconState::Idle);
    }

    // Lock hotkey editing (the embedded Settings hotkeys panel) while recording /
    // countdown / stopping.
    if (config_page_) {
        const bool hk_locked =
            (record_status_label_ == QStringLiteral("REC") || record_status_label_ == QStringLiteral("PAUSED") ||
             record_status_label_ == QStringLiteral("STOPPING") || record_status_label_ == QStringLiteral("COUNTDOWN"));
        // PS-PHASE-C: lock/unlock the embedded hotkeys panel in Settings.
        config_page_->setHotkeyEditingLocked(hk_locked);
    }

    if ((recording || record_status_label_ == QStringLiteral("COUNTDOWN")) && isVisible() && !isMinimized() &&
        stack_->currentIndex() != 0)
        navigateToPage(kRecordPageIndex);

    // EDIT-OVERLAY-R1 (review): a capture start (recording or countdown) dismisses
    // the Edit overlay. On main this happened implicitly via the stack swap-back
    // above; with the overlay the stack usually already shows Record, so the
    // dismissal must be explicit — otherwise a recording started by hotkey would
    // run invisibly underneath an editor opened on an old file. Deliberate: this
    // closes even while an export runs — closing only hides the progress UI, the
    // hosted page and its export worker thread live on, and re-entering after
    // Stop re-shows the running export (see navigateToEditExportPage; ADR 0022).
    // It is also the one close that skips the discard guard: a modal blocking a
    // hotkey-triggered recording is worse than a lost trim.
    if ((recording || record_status_label_ == QStringLiteral("COUNTDOWN")) && edit_export_overlay_ &&
        edit_export_overlay_->isOpen())
        edit_export_overlay_->closeOverlay();

    // Update recording overlay visibility/state.
    updateRecordingOverlay();
    // Update diagnostics overlay visibility/state.
    updateDiagnosticsOverlay();
    // QUICK-PILL-R1: update interactive quick-control pill visibility/state.
    updateQuickControlPill();

    // SETTINGS-HONESTY-R1: keep Diagnostics Phase ④'s "Open last report" link's
    // enabled state in sync with whether a completed recording actually exists.
    if (diagnostics_page_ && record_page_)
        diagnostics_page_->setHasLastRecording(record_page_->hasCompletedRecording());

    // FinalizingOverlay: shown for the FULL Stopping/Saving duration (not only on
    // a close attempt — replaces the native QMessageBox closeEvent used to show).
    // Container finalize (Stopping) is normally near-instant, a brief flash;
    // an MP4 remux (Saving) is visibly longer and gets real progress via
    // remuxProgressChanged above. showFinalizing() is idempotent to call again
    // on every refresh while Stopping (cheap: just resets to "Finishing…"/
    // indeterminate, matching the label having no progress data either way).
    const bool is_finalizing =
        (record_status_label_ == QStringLiteral("STOPPING") || record_status_label_ == QStringLiteral("SAVING"));
    if (is_finalizing) {
        buildFinalizingOverlay();
        // Stopping always precedes Saving (session_.Record() fully drains before
        // PostStateChange(Saving) — see RecordingCoordinator.cpp), so the overlay
        // is already visible by the time Saving begins; showSaving() (via the
        // remuxProgressChanged connect above) takes over the label/progress text
        // as soon as the first tick arrives. Re-asserting "Finishing…" here only
        // while still Stopping avoids clobbering that once Saving has taken over.
        if (record_status_label_ == QStringLiteral("STOPPING"))
            finalizing_overlay_->showFinalizing();
    } else if (was_finalizing_ && finalizing_overlay_) {
        finalizing_overlay_->hideOverlay();
    }
    was_finalizing_ = is_finalizing;

    // "Open editor when finished": auto-navigate to the Edit overlay on the
    // not-SAVED -> SAVED edge (a successful completion — SAVED only appears
    // when view_model_.last_succeeded, see RecordPage::statusLabelFor), never
    // on a failure. Hooked here rather than in the recordingResultReady handler
    // because PostResult() runs BEFORE PostStateChange(Completed) on the engine
    // side, so at result-ready time record_status_label_ still reads STOPPING/
    // SAVING and navigateToEditExportPage's own recording/countdown guard would
    // reject the call; this state is fully settled by the time it's observed here.
    const bool is_saved = (record_status_label_ == QStringLiteral("SAVED"));
    if (is_saved && !was_saved_ && persisted_settings_.open_editor_when_finished && record_page_)
        navigateToEditExportPage(record_page_->currentEditContext());
    was_saved_ = is_saved;
}

bool MainWindow::nativeEvent(const QByteArray& event_type, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    if (event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg != nullptr && msg->hwnd != nullptr) {
            const HWND main_hwnd = reinterpret_cast<HWND>(effectiveWinId());

            if (msg->hwnd == main_hwnd)
                traceFrameMessage(msg->hwnd, msg->message, msg->wParam, msg->lParam);

            if (msg->hwnd == main_hwnd && msg->message == WM_HOTKEY) {
                const int hk_id = static_cast<int>(msg->wParam);
                if (hk_id == GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording))
                    emit recordToggleRequested();
                else if (hk_id == GlobalHotkeyService::Win32IdForAction(HotkeyAction::TogglePause))
                    emit pauseToggleRequested();
                else if (hk_id == GlobalHotkeyService::Win32IdForAction(HotkeyAction::CaptureFrame))
                    emit captureFrameRequested();
                else if (hk_id == GlobalHotkeyService::Win32IdForAction(HotkeyAction::AddMarker))
                    emit addMarkerRequested();
                else if (hk_id == GlobalHotkeyService::Win32IdForAction(HotkeyAction::SplitRecording))
                    emit splitRecordingRequested();
                *result = 0;
                return true;
            }

            if (msg->hwnd == main_hwnd && msg->message == static_cast<UINT>(exosnap::update::kUpdaterHandoffMessage) &&
                msg->wParam == static_cast<WPARAM>(exosnap::update::kUpdaterHandoffMagic)) {
                // The updater has completed download verification and now owns
                // the close/install/relaunch sequence. Mark the request, bypass
                // close-to-tray, and let closeEvent commit the pending stamp only
                // after its recording/finalization guards accept the close.
                if (update_handoff_phase_ == UpdateHandoffPhase::UpdaterRunning) {
                    update_handoff_phase_ = UpdateHandoffPhase::ClosingForHandoff;
                    force_quit_ = true;
                    QMetaObject::invokeMethod(this, [this] { close(); }, Qt::QueuedConnection);
                }
                *result = 0;
                return true;
            }

            if (msg->hwnd == main_hwnd &&
                (msg->message == WM_NCACTIVATE || msg->message == WM_ACTIVATE || msg->message == WM_SETFOCUS)) {
                const char* reason = "focus-transition";
                if (msg->message == WM_NCACTIVATE)
                    reason = "WM_NCACTIVATE";
                else if (msg->message == WM_ACTIVATE)
                    reason = "WM_ACTIVATE";
                else if (msg->message == WM_SETFOCUS)
                    reason = "WM_SETFOCUS";
                applyDwmThemedBorder(msg->hwnd, reason);
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_NCACTIVATE) {
                // Let Windows update activation state without repainting default non-client visuals.
                *result = DefWindowProcW(msg->hwnd, msg->message, msg->wParam, -1);
                return true;
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_SIZE) {
                if (msg->wParam == SIZE_MAXIMIZED)
                    win32_maximized_ = true;
                else if (msg->wParam == SIZE_RESTORED)
                    win32_maximized_ = false;

                if (title_bar_ != nullptr)
                    title_bar_->setMaximizedState(effectiveMaximizedState());

                // Re-apply the themed DWM border after any size transition so the
                // OS-drawn accent border cannot reappear after restore/resize.
                applyDwmThemedBorder(msg->hwnd, "WM_SIZE");
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_GETMINMAXINFO) {
                auto* minmax_info = reinterpret_cast<MINMAXINFO*>(msg->lParam);
                if (minmax_info != nullptr) {
                    // Enforce minimum window size during native resize drag (device pixels).
                    const double dpr = devicePixelRatioF();
                    minmax_info->ptMinTrackSize.x = static_cast<LONG>(minimumWidth() * dpr);
                    minmax_info->ptMinTrackSize.y = static_cast<LONG>(minimumHeight() * dpr);

                    MONITORINFO monitor_info = {};
                    monitor_info.cbSize = sizeof(monitor_info);
                    const HMONITOR monitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
                    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                        const RECT& monitor_rect = monitor_info.rcMonitor;
                        const RECT& work_rect = monitor_info.rcWork;
                        minmax_info->ptMaxPosition.x = work_rect.left - monitor_rect.left;
                        minmax_info->ptMaxPosition.y = work_rect.top - monitor_rect.top;
                        minmax_info->ptMaxSize.x = work_rect.right - work_rect.left;
                        minmax_info->ptMaxSize.y = work_rect.bottom - work_rect.top;
                        minmax_info->ptMaxTrackSize = minmax_info->ptMaxSize;
                    }
                }
                *result = 0;
                return true;
            }

            if (msg->hwnd == main_hwnd && msg->message == WM_NCCALCSIZE && msg->wParam == TRUE) {
                auto* calc = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                // Use IsZoomed() for the real Win32 state — our tracked flag may lag during
                // the drag-to-restore gesture and would clip the rect at the wrong moment.
                const bool actually_maximized = (IsZoomed(msg->hwnd) != FALSE);
                if (calc != nullptr && actually_maximized) {
                    MONITORINFO monitor_info = {};
                    monitor_info.cbSize = sizeof(monitor_info);
                    const HMONITOR monitor = MonitorFromRect(&calc->rgrc[0], MONITOR_DEFAULTTONEAREST);
                    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE)
                        calc->rgrc[0] = monitor_info.rcWork;
                }

                *result = 0;
                return true;
            }

            // Resize cursor feedback: all zones are HTCLIENT, so WM_SETCURSOR's lParam
            // always carries HTCLIENT.  Read the live cursor position via Qt (logical
            // pixels) to derive the zone independently of NCHITTEST.
            // When leaving the resize zone we explicitly reset to IDC_ARROW — without
            // this the resize cursor sticks as Qt does not unconditionally call
            // SetCursor on every WM_SETCURSOR for client-area messages.
            if (msg->hwnd == main_hwnd && msg->message == WM_SETCURSOR) {
                if (!effectiveMaximizedState()) {
                    const QPoint local = mapFromGlobal(QCursor::pos());
                    const ResizeZone zone = resizeZoneFromLocalPoint(local, size(), false);
                    if (zone != ResizeZone::None) {
                        HCURSOR cursor = cursorFromHitTestCode(hitTestFromResizeZone(zone));
                        if (cursor != nullptr) {
                            SetCursor(cursor);
                            resize_cursor_shown_ = true;
                            *result = TRUE;
                            return true;
                        }
                    }
                    if (resize_cursor_shown_) {
                        // Just left the resize zone — force-reset so the resize cursor
                        // does not linger over the titlebar or content area.
                        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                        resize_cursor_shown_ = false;
                        // Return false so Qt can still set the correct cursor for the
                        // widget under the cursor (e.g. pointing-hand for nav items).
                    }
                } else {
                    resize_cursor_shown_ = false;
                }

                // Safety net for the titlebar drag/move override cursor: WM_EXITSIZEMOVE
                // below is the normal reset signal, but it depends on startSystemMove's
                // modal loop actually starting and cleanly exiting. WM_SETCURSOR instead
                // fires continuously for every mouse move anywhere over the window
                // (titlebar and plain body alike), so if it ever observes the left button
                // no longer held while the override is still active, the drag ended some
                // other way and the override would otherwise stick indefinitely.
                if (title_bar_ != nullptr && !(QGuiApplication::mouseButtons() & Qt::LeftButton))
                    title_bar_->resetDragCursor();
            }

            // Reset the drag/move override cursor when the window-move or resize
            // operation ends.  WM_CAPTURECHANGED fires too early (ReleaseCapture is
            // called inside startSystemMove before the loop starts), so WM_EXITSIZEMOVE
            // is the reliable signal that the modal loop has actually finished.
            if (msg->hwnd == main_hwnd && msg->message == WM_EXITSIZEMOVE) {
                if (title_bar_ != nullptr)
                    title_bar_->resetDragCursor();
            }

            // A live HDR/Advanced-Color toggle on any display re-probes DisplayHdrFacts so
            // the Diagnostics tab does not keep showing stale facts until the next recording
            // start (which already re-probes via RecordingCoordinator::RefreshedDisplayFacts()).
            // Live-verified 2026-07-24: WM_DISPLAYCHANGE fires reliably both for the per-display
            // HDR on/off switch and the separate "Automatic Color Management" toggle.
            if (msg->hwnd == main_hwnd && msg->message == WM_DISPLAYCHANGE && runtime_caps_ready_) {
                auto displays = capability::CapabilityBuilder::QueryDisplayFacts();
                // An empty result means the query failed (no DXGI factory) — keep what we
                // had rather than blanking every display to "no facts" (mirrors
                // RecordingCoordinator::RefreshDisplayFacts()'s same guard).
                if (!displays.empty()) {
                    runtime_caps_.runtime.displays = std::move(displays);
                    refreshDiagnosticsData();
                }
            }
        }
    }
#else
    Q_UNUSED(event_type);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(event_type, message, result);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // The Edit overlay spans the client area below the real title bar, so the
    // window stays movable and minimizable during an edit session. The height of
    // that band is pushed in from here — the overlay must not reach up for the
    // title bar itself — and re-applied whenever the client area changes size.
    if (event->type() == QEvent::Resize && watched == centralWidget() && edit_export_overlay_ && title_bar_)
        edit_export_overlay_->setTopInset(title_bar_->height());

    // A press outside an open Qt::Popup hub closes it (Qt auto-dismiss on
    // mouse-DOWN). If that press lands on the bell, flag it so the bell's
    // clicked() (fired on mouse-UP) does not immediately re-open the hub —
    // giving a clean single-click toggle instead of close-then-reopen.
    if (event->type() == QEvent::MouseButtonPress && notification_hub_ && notification_hub_->isVisible()) {
        auto* press = static_cast<QMouseEvent*>(event);
        if (press->button() == Qt::LeftButton && title_bar_ && title_bar_->bellWidget()) {
            auto* bell = title_bar_->bellWidget();
            // Any left-press while the popup hub is open dismisses it — reflect that
            // on the bell's open-state at once (covers clicking the bell and away).
            bell->setHubOpen(false);
            const QRect bell_rect(bell->mapToGlobal(QPoint(0, 0)), bell->size());
            if (bell_rect.contains(press->globalPosition().toPoint()))
                hub_just_dismissed_ = true; // bell's clicked() (on release) must not re-open
        }
    }

    // Intercept mouse presses for the resize border zones.  All zones are
    // HTCLIENT so Qt generates regular QMouseEvents — handle resize here.
    if (event->type() == QEvent::MouseButtonPress && isVisible() && !isMaximized()) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint local = mapFromGlobal(me->globalPosition().toPoint());
            const ResizeZone zone = resizeZoneFromLocalPoint(local, size(), false);
            if (zone != ResizeZone::None) {
                Qt::Edges edges;
                switch (zone) {
                case ResizeZone::Left:
                    edges = Qt::LeftEdge;
                    break;
                case ResizeZone::Right:
                    edges = Qt::RightEdge;
                    break;
                case ResizeZone::Top:
                    edges = Qt::TopEdge;
                    break;
                case ResizeZone::Bottom:
                    edges = Qt::BottomEdge;
                    break;
                case ResizeZone::TopLeft:
                    edges = Qt::LeftEdge | Qt::TopEdge;
                    break;
                case ResizeZone::TopRight:
                    edges = Qt::RightEdge | Qt::TopEdge;
                    break;
                case ResizeZone::BottomLeft:
                    edges = Qt::LeftEdge | Qt::BottomEdge;
                    break;
                case ResizeZone::BottomRight:
                    edges = Qt::RightEdge | Qt::BottomEdge;
                    break;
                default:
                    break;
                }
                if (edges) {
                    if (QWindow* win = windowHandle()) {
                        win->startSystemResize(edges);
                        return true; // consume — do not forward to child widgets
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    // NOTIFY-SKIN-R1: clear unread badge when the window becomes active.
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        if (tray_presence_)
            tray_presence_->clearUnreadCount();
    }
    if (event->type() == QEvent::WindowStateChange) {
        win32_maximized_ = isMaximized();
        if (title_bar_ != nullptr)
            title_bar_->setMaximizedState(effectiveMaximizedState());
#if defined(Q_OS_WIN)
        const auto* state_event = static_cast<QWindowStateChangeEvent*>(event);
        const bool was_maximized = (state_event->oldState() & Qt::WindowMaximized) != 0;
        const bool restored_from_maximized = was_maximized && !isMaximized();

        // On restore from maximized, force the NC area to be recalculated and
        // re-apply the themed DWM border so the accent frame cannot reappear.
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd != nullptr) {
            if (restored_from_maximized) {
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            applyDwmThemedBorder(hwnd, "changeEvent/WindowStateChange");
        }
#endif
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Flush a pending debounced live-config save so a quit within the 750 ms
    // window never loses the last edit.
    if (live_persist_timer_ && live_persist_timer_->isActive()) {
        live_persist_timer_->stop();
        persistPresetState();
    }

    saveWindowGeometry();

    if (update_handoff_phase_ == UpdateHandoffPhase::ClosingForHandoff) {
        const bool export_active =
            edit_export_overlay_ && edit_export_overlay_->page() && edit_export_overlay_->page()->isExportRunning();
        const bool handoff_blocked = record_status_label_ == QStringLiteral("STOPPING") || remuxing_active_ ||
                                     recording_active_ || export_active;
        if (handoff_blocked) {
            update_handoff_phase_ = UpdateHandoffPhase::Idle;
            force_quit_ = false;
            if (config_page_) {
                const QString state =
                    verify_update_reinstall_ ? QStringLiteral("verify-reinstall") : QStringLiteral("available");
                config_page_->setUpdateStatus(state, last_available_version_, QString());
            }
            diagnostics::AppLog::warning(
                QStringLiteral("update"),
                QStringLiteral("Updater close handoff refused because recording or finalization became active"));
            event->ignore();
            return;
        }
    }

    // TRAY-CLOSE-TO-TRAY-R1: if close-to-tray is enabled and this is NOT a
    // force-quit (e.g. from the tray "Quit" action or recording guard accept),
    // hide the window to the tray instead of quitting.
    if (ui::tray::ShouldHideToTray(persisted_settings_.keep_running_in_tray, force_quit_, tray_presence_ != nullptr)) {
        event->ignore();
        hide();
        if (tray_presence_)
            tray_presence_->setWindowVisible(false);

        // One-time close-to-tray notice: shown on the first hide so the user
        // knows ExoSnap is still running in the tray.
        if (!persisted_settings_.tray_close_notice_shown) {
            persisted_settings_.tray_close_notice_shown = true;
            settings_store_.Save(persisted_settings_);
            if (tray_presence_) {
                tray_presence_->showMessage(
                    QStringLiteral("ExoSnap is still running"),
                    QStringLiteral("ExoSnap is running in the tray. Right-click the tray icon to quit."),
                    QSystemTrayIcon::Information, 4000);
            }
        }
        return;
    }

    // Reset the force-quit flag for next time (in case the window is re-shown).
    force_quit_ = false;

    // The engine is finalizing the container after a stop (STOPPING): the mux
    // thread is still draining its queues and writing Cues/Duration/SeekHead.
    // Closing now would tear down the process mid-write and leave an unfinalized
    // file. Block the close until finalization completes — it is normally
    // near-instant, and the bounded join budget caps it even in the worst case.
    // There is deliberately no "force close" option here: unlike the MP4 remux
    // (which leaves the transient MKV intact), aborting the container finalize
    // would corrupt the recording being written. No separate dialog is shown —
    // FinalizingOverlay (onRecordChromeStateChanged) is already visible for the
    // full Stopping duration by this point, so the block is self-explanatory;
    // a native QMessageBox here used to be the only place in the app that
    // surfaced feedback as a separate OS window instead of in-app.
    if (record_status_label_ == QStringLiteral("STOPPING")) {
        event->ignore();
        return;
    }

    // ADR-0014: MP4 remux running after stop — ask user to wait.
    if (remuxing_active_) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("Saving in progress"));
        msgBox.setText(QStringLiteral("ExoSnap is saving your MP4 recording. Closing now will cancel the save and "
                                      "leave only the temporary MKV file on disk."));
        msgBox.setIcon(QMessageBox::Warning);

        auto* waitBtn = msgBox.addButton(QStringLiteral("Wait for save to finish"), QMessageBox::RejectRole);
        auto* closeBtn = msgBox.addButton(QStringLiteral("Cancel save and close"), QMessageBox::AcceptRole);
        Q_UNUSED(closeBtn);
        msgBox.setDefaultButton(waitBtn);

        msgBox.exec();

        if (msgBox.clickedButton() == static_cast<QAbstractButton*>(waitBtn)) {
            event->ignore();
        } else {
            // Cancel the remux and accept the close.  The coordinator cancels
            // cooperatively; the jthread will finish quickly and the transient
            // MKV is left on disk.
            if (record_page_)
                record_page_->cancelRemux();
            remuxing_active_ = false;
            event->accept();
        }
        return;
    }

    // EDIT-OVERLAY-R1 (review): a stream-copy export from the Edit surface is
    // running (possibly with its overlay dismissed by a recording start). Closing
    // would cancel it in ~EditExportPage without a word — ask, mirroring the remux
    // guard above. Data-wise "cancel and close" is safe: an export never mutates
    // the original recording; at most a partial .tmp/_edit file is abandoned.
    if (edit_export_overlay_ && edit_export_overlay_->page() && edit_export_overlay_->page()->isExportRunning()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("Export in progress"));
        msgBox.setText(QStringLiteral("ExoSnap is exporting your edited recording. Closing now will cancel the "
                                      "export. The original recording is untouched."));
        msgBox.setIcon(QMessageBox::Warning);

        auto* waitBtn = msgBox.addButton(QStringLiteral("Wait for export to finish"), QMessageBox::RejectRole);
        auto* closeBtn = msgBox.addButton(QStringLiteral("Cancel export and close"), QMessageBox::AcceptRole);
        Q_UNUSED(closeBtn);
        msgBox.setDefaultButton(waitBtn);

        msgBox.exec();

        if (msgBox.clickedButton() == static_cast<QAbstractButton*>(waitBtn)) {
            event->ignore();
            return;
        }
        // Accepted: fall through to the remaining guards (a recording could in
        // principle be active as well); ~EditExportPage cancels the export
        // cooperatively and joins the worker thread during teardown.
    }

    if (recording_active_) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(QStringLiteral("Recording in progress"));
        msgBox.setText(QStringLiteral("ExoSnap is still recording. Closing now will stop the current recording."));
        msgBox.setIcon(QMessageBox::Warning);

        auto* stopBtn = msgBox.addButton(QStringLiteral("Stop recording and close"), QMessageBox::AcceptRole);
        auto* cancelBtn = msgBox.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
        msgBox.setDefaultButton(cancelBtn);

        msgBox.exec();

        if (msgBox.clickedButton() == static_cast<QAbstractButton*>(stopBtn)) {
            emit recordToggleRequested();
            recording_active_ = false;
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    if (update_handoff_phase_ == UpdateHandoffPhase::ClosingForHandoff) {
        persisted_settings_.applied_version =
            AppliedVersionForCommittedHandoff(last_available_version_, verify_update_reinstall_);
        settings_store_.Save(persisted_settings_);
        if (config_page_)
            config_page_->setUpdateStatus(QStringLiteral("pending"), last_available_version_, QString());
        diagnostics::AppLog::info(
            QStringLiteral("update"),
            verify_update_reinstall_
                ? QStringLiteral("Verification reinstall handoff committed for %1; no loop guard persisted")
                      .arg(last_available_version_)
                : QStringLiteral("Update handoff committed for %1; closing the old app").arg(last_available_version_));
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::toggleFullScreen() {
    if (isFullScreen()) {
        pre_fullscreen_maximized_ ? showMaximized() : showNormal();
    } else {
        pre_fullscreen_maximized_ = isMaximized() || win32_maximized_;
        showFullScreen();
    }
}

void MainWindow::saveWindowGeometry() {
    auto& geo = persisted_settings_.window_geometry;
    geo.maximized = isMaximized() || win32_maximized_;
    // normalGeometry() returns the restore rect even when currently maximized or fullscreen.
    const QRect restore_rect = (isMaximized() || isFullScreen()) ? normalGeometry() : geometry();
    geo.x = restore_rect.x();
    geo.y = restore_rect.y();
    geo.width = restore_rect.width();
    geo.height = restore_rect.height();
    settings_store_.Save(persisted_settings_); // saves hotkeys + window geometry only
}

void MainWindow::saveWindowGeometryToSettings() {
    saveWindowGeometry();
}

void MainWindow::applyRestoredGeometry() {
    const auto& geo = persisted_settings_.window_geometry;
    // Resolved once below (title-strip intersect, or primary fallback) and reused for
    // the final B3 clamp — see the comment on that clamp for why it must not be
    // re-queried via this->screen().
    QScreen* target_screen = nullptr;

    if (geo.width > 0 && geo.height > 0) {
        // The title bar region that must remain accessible so the user can move the window.
        const QRect title_strip(geo.x, geo.y, std::min(geo.width, 200), 40);

        for (QScreen* s : QGuiApplication::screens()) {
            if (s->availableGeometry().intersects(title_strip)) {
                target_screen = s;
                break;
            }
        }

        // Saved position lands on no connected monitor: center on primary.
        const bool center_on_primary = target_screen == nullptr;
        if (center_on_primary)
            target_screen = QGuiApplication::primaryScreen();
        if (target_screen != nullptr) {
            const QRect saved(geo.x, geo.y, geo.width, geo.height);
            setGeometry(ui::ClampRestoredWindowGeometry(saved, target_screen->availableGeometry(),
                                                        QSize(minimumWidth(), minimumHeight()), center_on_primary));
        }

        if (geo.maximized) {
            // showMaximized() fills the work area itself; no further clamp needed.
            QTimer::singleShot(0, this, &MainWindow::showMaximized);
            return;
        }
    }

    // B3: whatever geometry we now have — restored-and-partially-clamped above (which
    // only guarantees a reachable title strip, so a bottom overhang under the taskbar
    // can still slip through), or Qt's own first-launch default placement when there is
    // no persisted geometry at all — pull it fully inside the work area of whichever
    // screen it ended up on.
    //
    // Reuses the screen resolved above instead of re-querying this->screen(): right
    // after setGeometry() (above) moves the window onto a non-primary monitor's
    // coordinates, Qt/Windows updates the QWindow→QScreen association asynchronously,
    // so this->screen() can still report the primary screen within this same
    // showEvent — which would clamp a legitimately restored Screen-2 position back
    // onto Screen 1's work area (e.g. saved (2100,100,800,600) -> (1120,100,800,600)),
    // a multi-monitor restore regression. When there was no persisted geometry at all
    // (first launch, target_screen still unset here) fall back to this->screen() —
    // Qt's own first-placement decision — then primaryScreen().
    //
    // Runs once, here on first show; never re-applied while the window is live, so a
    // user who deliberately drags the window under the taskbar afterwards is free to
    // do so.
    if (target_screen == nullptr)
        target_screen = this->screen();
    if (target_screen == nullptr)
        target_screen = QGuiApplication::primaryScreen();
    if (target_screen != nullptr)
        setGeometry(ui::ClampWindowToWorkArea(geometry(), target_screen->availableGeometry()));
}

int MainWindow::navHighlightIndexFor(int index) const {
    // Every stack page now has its own top-nav tab, so the highlighted tab is the
    // page index itself. (Kept as a seam in case future sub-pages need remapping.)
    return index;
}

void MainWindow::navigateToPage(int index) {
    if (index < 0 || index >= static_cast<int>(kPageDescriptors.size()))
        return;

    // Any page switch cleanly dismisses inline overlays so they never linger
    // over an unrelated page.
    if (source_picker_overlay_)
        source_picker_overlay_->closeOverlay();

    // EDIT-OVERLAY-R1: navigating to any page other than Record dismisses the Edit
    // overlay — it is only meaningful "over Record", never left floating above a
    // different page. A running export is the one exception: the same rule that
    // blocks Escape/backdrop dismiss also blocks nav-away, so it is never silently
    // abandoned by clicking another nav item. Unexported trim/marker work asks
    // first, and "Keep editing" cancels the navigation outright rather than
    // deferring it.
    if (edit_export_overlay_ && edit_export_overlay_->isOpen() && index != kRecordPageIndex) {
        if (edit_export_overlay_->isDismissBlocked())
            return; // stay put — the export keeps running
        if (!edit_export_overlay_->requestCloseOverlay())
            return;
    }

    setCurrentPage(index);
    if (title_bar_)
        title_bar_->setActivePage(navHighlightIndexFor(index));
}

void MainWindow::setCurrentPage(int index) {
    if (index < 0 || index >= static_cast<int>(kPageDescriptors.size()))
        return;

    // Ensure deferred pages are built before they are shown for the first time.
    if (index == kDevicePageIndex && !device_page_)
        buildDevicePage();
    if (index == kSettingsPageIndex && !config_page_)
        buildConfigPage();
    if (index == kDiagnosticsPageIndex && !diagnostics_page_)
        buildDiagnosticsPage();
    if (index == kLogsPageIndex && !logs_page_)
        buildLogsPage();
    if (index == kAboutPageIndex && !about_page_)
        buildAboutPage();

    stack_->setCurrentIndex(index);

    // Keep the title-bar status pill consistent across page switches (the "Saved"
    // state is scoped to the Record page — see applyTitleBarStatus()).
    applyTitleBarStatus();
}

void MainWindow::applyTitleBarStatus() {
    if (title_bar_ == nullptr)
        return;

    const bool on_record_page = (stack_ != nullptr && stack_->currentIndex() == kRecordPageIndex);
    title_bar_->setRecordingActive(recording_active_);
    title_bar_->setStatusLabel(ui::chrome::ScopeStatusLabelForActivePage(record_status_label_, on_record_page));
}

RecordingPresetConfig MainWindow::captureLiveConfig() const {
    RecordingPresetConfig cfg;
    if (record_page_)
        cfg.capture = record_page_->currentCapturePolicy();
    cfg.output = output_settings_;
    cfg.video = video_settings_;
    cfg.audio = live_audio_;
    cfg.webcam = live_webcam_;
    cfg.countdown_seconds = record_page_ ? record_page_->countdownSeconds() : 0;
    return SanitizePresetConfig(cfg);
}

void MainWindow::applyPresetConfig(const RecordingPresetConfig& cfg) {
    const RecordingPresetConfig cfg2 = SanitizePresetConfig(cfg);
    applying_preset_ = true;

    // Stage live mirrors.
    output_settings_ = cfg2.output;
    video_settings_ = cfg2.video;
    live_audio_ = cfg2.audio;
    live_webcam_ = cfg2.webcam;

    // Push to pages. record_page_->applyCapturePolicy() below can rebuild
    // audio rows and emit audioSettingsChanged with kind-default rows before
    // applyPersistedAudioSettings() (LAST) restores the preset's exact rows.
    // Every live-config listener below early-returns while applying_preset_
    // is set so those intermediate kind-default emissions never clobber
    // live_audio_ or trigger a spurious live-config persist mid-apply; order
    // still matters so the preset's rows are what finally lands.
    if (record_page_) {
        record_page_->setOutputSettings(cfg2.output);
        record_page_->setVideoSettings(cfg2.video);
        record_page_->setWebcamSettings(cfg2.webcam);
        record_page_->applyCapturePolicy(cfg2.capture);
        record_page_->setCountdownSeconds(cfg2.countdown_seconds);
        record_page_->applyPersistedAudioSettings(cfg2.audio); // LAST: wins over kind-defaults
    }
    if (config_page_) {
        config_page_->setOutputSettings(cfg2.output);
        config_page_->setVideoSettings(cfg2.video);
        config_page_->setAudioUiState(cfg2.audio);
        config_page_->setWebcamSettings(cfg2.webcam);
        config_page_->setOutputFolder(cfg2.output.output_folder);
        config_page_->setActiveProfileName(QString::fromStdString(preset_registry_.SelectedPreset().name));
    }

    applying_preset_ = false;

    // Finalize: recompute dirty and refresh preset UI once.
    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    if (config_page_)
        config_page_->setPresetDirty(dirty);
    refreshPresetUi();
    refreshDiagnosticsData();
}

void MainWindow::refreshPresetUi() {
    std::vector<ConfigPage::ProfileOption> config_options;
    config_options.reserve(preset_registry_.Count());

    for (const auto& preset : preset_registry_.Presets()) {
        ConfigPage::ProfileOption co;
        co.id = QString::fromStdString(preset.id);
        co.label = QString::fromStdString(preset.name);
        co.built_in = RecordingPresetRegistry::IsBuiltIn(preset.id);
        co.modified = false;
        co.available = true;
        config_options.push_back(co);
    }

    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    // ConfigPage does not emit its selection-changed signal from the sync setter
    // (setPresetOptions renders only — see ConfigPage.cpp's QSignalBlocker on the
    // combo) so this replay cannot re-enter onPresetSelected.
    if (config_page_) {
        config_page_->setPresetOptions(config_options, QString::fromStdString(preset_registry_.SelectedId()), dirty);
        config_page_->setActiveProfileName(QString::fromStdString(preset_registry_.SelectedPreset().name));
    }
}

void MainWindow::persistPresetState() {
    QString err;
    if (!preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(), captureLiveConfig(), &err)) {
        diagnostics::AppLog::warning(QStringLiteral("presets"),
                                     QStringLiteral("Failed to save recording settings: %1").arg(err));
        if (notification_manager_) {
            notifications::NotificationEvent event;
            event.type = notifications::NotificationType::SettingsSaveFailed;
            event.title = QStringLiteral("Settings could not be saved");
            event.body = QStringLiteral("Your latest changes could not be written to disk and may be lost.");
            notification_manager_->Enqueue(std::move(event));
        }
    }
}

void MainWindow::onLiveConfigChanged() {
    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    if (config_page_)
        config_page_->setPresetDirty(dirty);
    schedulePersistLiveState();
}

void MainWindow::schedulePersistLiveState() {
    if (!live_persist_timer_) {
        live_persist_timer_ = new QTimer(this);
        live_persist_timer_->setSingleShot(true);
        live_persist_timer_->setInterval(750); // coalesce slider drags into one write
        connect(live_persist_timer_, &QTimer::timeout, this, [this]() { persistPresetState(); });
    }
    live_persist_timer_->start();
}

void MainWindow::initHotkeyService() {
    hotkey_service_ = new GlobalHotkeyService(this);
    hotkey_service_->LoadFromStrings(persisted_settings_.hotkey_bindings);
}

void MainWindow::onHotkeyServiceBindingChanged(HotkeyAction /*action*/, QKeySequence /*seq*/) {
    // Service has already committed the new binding. Persist to settings file.
    if (hotkey_service_)
        hotkey_service_->SaveToStrings(persisted_settings_.hotkey_bindings);
    settings_store_.Save(persisted_settings_);
    refreshDiagnosticsData();
}

void MainWindow::refreshDiagnosticsData() {
    if (!diagnostics_page_ || !runtime_caps_ready_)
        return;

    std::string hotkeys_summary;
    if (hotkey_service_) {
        for (int i = 0; i < kHotkeyActionCount; ++i) {
            const auto action = static_cast<HotkeyAction>(i);
            const QKeySequence seq = hotkey_service_->GetBinding(action);
            if (!seq.isEmpty()) {
                if (!hotkeys_summary.empty())
                    hotkeys_summary += ", ";
                hotkeys_summary += GlobalHotkeyService::ActionDisplayName(action).toStdString();
                hotkeys_summary += ": ";
                hotkeys_summary += seq.toString(QKeySequence::PortableText).toStdString();
            }
        }
    }
    if (hotkeys_summary.empty())
        hotkeys_summary = "None configured";

    const std::string preset_name = preset_registry_.SelectedPreset().name;
    // Visual-test: re-apply the sticky GPU fixture — the async caps-ready path
    // re-assigned runtime_caps_ from the real probe after the scenario was applied.
    capability::CapabilitySet diag_caps = runtime_caps_;
    if (visual_diagnostics_gpu_override_)
        ApplyVisualGpuFixture(diag_caps);
    diagnostics_page_->setDiagnosticData(diag_caps, output_settings_, video_settings_, live_audio_, preset_name,
                                         hotkeys_summary, settings_store_.SettingsFilePath().toStdString(),
                                         hotkeys_registered_);
    std::optional<recorder_core::CaptureTarget> selected_target;
    if (record_page_) {
        selected_target = record_page_->selectedCaptureTarget();
        const RecordPage::SavedDisplayResolution res = record_page_->savedDisplayResolution();
        std::string label = res.friendly_name;
        if (label.empty() && res.seq_hint > 0) {
            label = "Display " + std::to_string(res.seq_hint);
        }
        diagnostics_page_->setSavedDisplayUnresolved(res.unresolved, label);
    }
    diagnostics_page_->setSelectedCaptureTarget(selected_target);

    // Selected-window exclusive-fullscreen probe (S2a/S2b) + pre-flight present
    // attribution (S6). Subscribe the probe to the selected window (0 for a
    // monitor target), pause it while recording (the engine owns the capture),
    // and feed its current snapshot to the page. The evidence accumulates on the
    // probe's own thread; refreshWindowEvidence() re-pulls it on a light cadence.
    uintptr_t selected_window = 0;
    if (selected_target.has_value() && selected_target->kind == recorder_core::CaptureTarget::Kind::Window) {
        selected_window = selected_target->native_id;
    }
    window_evidence_probe_.setWindowTarget(selected_window);
    window_evidence_probe_.setPaused(recording_active_);
    // Pre-flight PID attribution: while idle, scope present diagnostics to the
    // selected window's process so rec.present.exclusive is target-accurate rather
    // than "whatever last presented". The record-start edge owns it during recording.
    if (!recording_active_) {
        present_provider_.SetTargetProcessId(record_page_ ? record_page_->selectedTargetWindowPid() : 0);
    }
    refreshWindowEvidence();
}

void MainWindow::refreshWindowEvidence() {
    if (!diagnostics_page_ || !diagnostics_page_->isVisible())
        return;
    const WindowEvidenceProbe::Snapshot snap = window_evidence_probe_.snapshot();
    if (snap.active) {
        diagnostics_page_->setCaptureWindowEvidence(snap.facts, snap.evidence);
    } else {
        diagnostics_page_->setCaptureWindowEvidence(std::nullopt, {});
    }
}

// ---------------------------------------------------------------------------
// Stage 2 — Preset operation handlers
// ---------------------------------------------------------------------------

void MainWindow::onPresetSelected(const QString& id) {
    if (id.toStdString() == preset_registry_.SelectedId())
        return; // combo refresh echo (the preset combo re-selecting itself) — not a switch
    if (!record_page_ || !record_page_->canApplyPresetNow()) {
        // Reject switch during recording — revert the selector.
        refreshPresetUi();
        diagnostics::AppLog::warning(QStringLiteral("preset"),
                                     QStringLiteral("preset switch rejected: recording in progress"));
        return;
    }

    PresetSwitchUndo undo;
    undo.previous_live = captureLiveConfig();
    undo.previous_selected_id = preset_registry_.SelectedId();

    if (!preset_registry_.SetSelected(id.toStdString()))
        return;
    pending_preset_undo_ = std::move(undo);

    applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), captureLiveConfig()));
    persistPresetState();

    if (notification_manager_) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::PresetSwitched;
        event.title =
            QStringLiteral("Switched to '%1'").arg(QString::fromStdString(preset_registry_.SelectedPreset().name));
        event.action = notifications::NotificationAction::UndoPresetSwitch;
        notification_manager_->Enqueue(std::move(event));
    }
}

void MainWindow::onSavePresetAs(const QString& name) {
    preset_registry_.AddPreset(captureLiveConfig(), name.toStdString());
    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    if (config_page_)
        config_page_->setPresetDirty(dirty);
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onRenamePreset(const QString& name) {
    if (!preset_registry_.RenameSelected(name.toStdString())) {
        QMessageBox::warning(this, QStringLiteral("Rename Preset"),
                             QStringLiteral("Could not rename preset. The name may already be in use."));
        return;
    }
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onDeletePreset() {
    if (!preset_registry_.DeleteSelected()) {
        QMessageBox::warning(this, QStringLiteral("Delete Preset"),
                             QStringLiteral("Built-in presets cannot be deleted."));
        return;
    }
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onResetChanges() {
    applyPresetConfig(WithEnvironmentFields(preset_registry_.SelectedSavedConfig(), captureLiveConfig()));
    refreshPresetUi();
    // No registry mutation, but the live config just changed back to the
    // preset's saved values, so the persisted live state must follow.
    persistPresetState();
}

// ---------------------------------------------------------------------------
// Export / import handlers
// ---------------------------------------------------------------------------

void MainWindow::onExportSelectedProfile(const QString& path) {
    const RecordingPreset& selected = preset_registry_.SelectedPreset();
    QString err;
    if (!RecordingPresetStore::ExportPresetToFile(selected, path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Export Failed"), err);
        return;
    }
    diagnostics::AppLog::info(
        QStringLiteral("preset"),
        QStringLiteral("exported preset '%1' to %2").arg(QString::fromStdString(selected.name), path));
    QMessageBox::information(
        this, QStringLiteral("Export Successful"),
        QStringLiteral("Preset \"%1\" exported successfully.").arg(QString::fromStdString(selected.name)));
}

void MainWindow::onImportProfiles(const QString& path) {
    // Build the current id set for collision detection.
    std::vector<std::string> existing_ids;
    existing_ids.reserve(preset_registry_.Count());
    for (const auto& p : preset_registry_.Presets()) {
        existing_ids.push_back(p.id);
    }

    QString err;
    const QVector<RecordingPreset> imported = RecordingPresetStore::ImportPresetsFromFile(path, existing_ids, &err);

    if (imported.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import Failed"),
                             err.isEmpty() ? QStringLiteral("No valid presets found in the file.") : err);
        return;
    }

    for (const RecordingPreset& p : imported) {
        preset_registry_.ImportPreset(p);
    }

    refreshPresetUi();
    persistPresetState();

    diagnostics::AppLog::info(QStringLiteral("preset"),
                              QStringLiteral("imported %1 preset(s) from %2").arg(imported.size()).arg(path));
    QMessageBox::information(this, QStringLiteral("Import Successful"),
                             QStringLiteral("Imported %1 preset(s).").arg(imported.size()));
}

// ---------------------------------------------------------------------------
// Reactive device-change handlers
// ---------------------------------------------------------------------------

void MainWindow::onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap, exosnap::DiscoveryReason /*reason*/) {
    // Forward to both pages under the no-emit, no-dirty contract.
    // Neither page should emit audioSettingsChanged during these calls, so
    // live_audio_ and the preset dirty state remain unchanged.
    if (config_page_)
        config_page_->onAudioDevicesChanged(snap);
    if (record_page_)
        record_page_->onAudioDevicesChanged(snap);
}

void MainWindow::onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap,
                                        exosnap::DiscoveryReason /*reason*/) {
    // Forward to both consumers.  Neither should emit webcamSettingsChanged so
    // live_webcam_ and the preset dirty state remain unchanged.
    if (config_page_)
        config_page_->onWebcamDevicesChanged(snap);
    if (record_page_)
        record_page_->onWebcamDevicesChanged(snap);
}

void MainWindow::onDisplaysChanged(const exosnap::DisplaySnapshot& snap, exosnap::DiscoveryReason /*reason*/) {
    // The display handler does not emit recordingConfigChanged for availability
    // changes (only for actual user-initiated target switches), so preset stays clean.
    if (record_page_)
        record_page_->onDisplaysChanged(snap);
}

// ---------------------------------------------------------------------------
// Tray presence helpers (TRAY-PRESENCE-R1)
// ---------------------------------------------------------------------------

void MainWindow::onTrayActivateWindow() {
    // Raise and activate the window — same as clicking the taskbar button.
    // If the window is minimized, restore it first.
    if (isMinimized())
        showNormal();
    else if (!isVisible())
        show();
    raise();
    activateWindow();
    // Update the Show/Hide label to reflect the new visibility state.
    if (tray_presence_)
        tray_presence_->setWindowVisible(isVisible());
    // NOTIFY-SKIN-R1: clear unread badge when the user focuses the window via tray.
    if (tray_presence_)
        tray_presence_->clearUnreadCount();
}

// ---------------------------------------------------------------------------
// NOTIFY-TOASTS-R1: Notification toast wiring
// ---------------------------------------------------------------------------

void MainWindow::initNotificationToasts() {
    // Create the manager (parented to MainWindow — torn down with it).
    notification_manager_ = new notifications::NotificationManager(this);

    // The "Show notifications" setting gates only the toast glance; the hub
    // record below is fed unconditionally.
    notification_manager_->SetToastsEnabled(persisted_settings_.show_notifications);

    // The hub is the record: every enqueued notification lands there.
    connect(notification_manager_, &notifications::NotificationManager::eventRecorded, this,
            &MainWindow::recordEventInHub);

    // Toast window is top-level (no Qt parent) to avoid being clipped by MainWindow.
    // Destroyed explicitly in ~MainWindow().
    notification_toast_window_ = new ui::overlay::NotificationToastWindow(notification_manager_, nullptr);

    // Toasts appear on the screen hosting the app window, not the primary one.
    notification_toast_window_->setAnchorWidget(this);

    // The toast is now interactive: an action-pill click routes here so the toast
    // reuses the existing destinations (folder/reveal/recovery/about/settings)
    // instead of duplicating action logic. The manager has already dismissed the
    // toast by the time this fires.
    connect(notification_toast_window_, &ui::overlay::NotificationToastWindow::actionTriggered, this,
            &MainWindow::dispatchNotificationAction);

    // The preset store needed a field-wise repair before toasts existed to report
    // it (see the ctor's preset-load block) — raise it now that they do.
    if (preset_store_repaired_) {
        preset_store_repaired_ = false;
        if (notification_manager_) {
            notifications::NotificationEvent event;
            event.type = notifications::NotificationType::SettingsRepaired;
            event.title = QStringLiteral("Settings repaired");
            event.body = QStringLiteral("Some saved settings were invalid and have been repaired.");
            notification_manager_->Enqueue(std::move(event));
        }
    }

    // AppSettingsStore::Load() found settings.ini unreadable before toasts existed
    // to report it (see the ctor's settings-load block) — raise it now that they do.
    if (app_settings_corrupted_) {
        app_settings_corrupted_ = false;
        if (notification_manager_) {
            notifications::NotificationEvent event;
            event.type = notifications::NotificationType::SettingsRepaired;
            event.title = QStringLiteral("Settings reset");
            event.body = QStringLiteral("Your saved app settings could not be read and were reset to defaults.");
            notification_manager_->Enqueue(std::move(event));
        }
    }

    // NOTE: config_page_ setShowNotifications + showNotificationsChanged connect are
    // wired in buildConfigPage() (config_page_ does not exist yet at this call site).

    // ── Trigger 1 + 2 + 3: recording result ready (Saved / LowStorage / UnexpectedStop) ──
    // Hooked into RecordPage::recordingResultReady emitted from the SetResultReadyCallback.
    connect(record_page_, &RecordPage::recordingResultReady, this,
            [this](bool succeeded, const QString& output_path, const QString& error_phase) {
                if (!notification_manager_)
                    return;

                // "Open editor when finished": the Edit overlay opening directly (wired
                // in onRecordChromeStateChanged, on the SAVED transition) already IS the
                // post-recording feedback; the toast's Edit/Show-in-folder actions would
                // be a redundant second path to the same place. Suppressed here, not by
                // returning early — the frames-dropped follow-up below must still fire
                // regardless of this setting.
                const bool suppress_saved_toast = succeeded && persisted_settings_.open_editor_when_finished;

                notifications::NotificationEvent event;
                if (succeeded) {
                    if (!suppress_saved_toast) {
                        // Trigger 2: recording saved successfully.
                        event.type = notifications::NotificationType::Saved;
                        event.title = QStringLiteral("Recording saved");
                        // Prefer the filename; fall back to a generic body.
                        if (!output_path.isEmpty()) {
                            const QString name =
                                output_path.contains(QLatin1Char('/')) || output_path.contains(QLatin1Char('\\'))
                                    ? output_path.mid(
                                          output_path.lastIndexOf(QRegularExpression(QStringLiteral("[/\\\\]"))) + 1)
                                    : output_path;
                            event.body = name.isEmpty() ? QStringLiteral("File saved to output folder") : name;
                        } else {
                            event.body = QStringLiteral("File saved to output folder");
                        }
                        // Primary action: navigate to the Edit/Output page for the saved
                        // recording. Secondary action: reveal the file in Explorer.
                        // Both share action_payload (the output file path).
                        event.action = notifications::NotificationAction::Edit;
                        event.secondary_action = notifications::NotificationAction::OpenFolder;
                        event.action_payload = output_path;
                    }
                } else if (error_phase == QStringLiteral("DiskSpace")) {
                    // Trigger 1: disk monitor hard-stop — "Storage running low" (caution, sticky).
                    // Mappe spec: action "Change folder" (primary) + "Dismiss".
                    event.type = notifications::NotificationType::LowStorage;
                    event.title = QStringLiteral("Storage running low");
                    event.body = QStringLiteral("Recording stopped — output drive is critically low on disk space.");
                    event.action = notifications::NotificationAction::ChangeFolder;
                    event.secondary_action = notifications::NotificationAction::None; // Dismiss shown by ghost pill
                } else {
                    // RECORDING-ERROR-MODAL-R1: a non-disk-space failure is now surfaced
                    // by the modal RecordingErrorOverlay (RecordPage::recordingFailed), so
                    // we no longer enqueue a redundant "stopped unexpectedly" toast here.
                    // The modal carries the full detail and the opt-in error report.
                    return;
                }
                if (!suppress_saved_toast)
                    notification_manager_->Enqueue(std::move(event));

                // DROP-NOTIFY: on a successful save, raise a separate caution toast when
                // REAL frame drops occurred — encoder backpressure or a frame-processing
                // failure. Benign drops (capture coalescing / intentional CFR
                // downsampling) are excluded by design, so this fires only when the
                // recording genuinely lost picture. The body names no single cause
                // because two can produce it; the toast links to the Diagnostics page,
                // which renders the full per-stage breakdown. A separate event (not
                // folded into "Recording saved") because that toast's two action slots —
                // Edit + Show in folder — are both taken.
                if (succeeded && last_real_drops_ > 0) {
                    notifications::NotificationEvent drop_event;
                    drop_event.type = notifications::NotificationType::FramesDropped;
                    drop_event.title = QStringLiteral("Frames dropped");
                    drop_event.body =
                        last_real_drops_ == 1
                            ? QStringLiteral("1 frame did not make it into the recording.")
                            : QStringLiteral("%1 frames did not make it into the recording.").arg(last_real_drops_);
                    drop_event.action = notifications::NotificationAction::OpenDiagnostics;
                    notification_manager_->Enqueue(std::move(drop_event));
                }
            });

    // DROP-NOTIFY: tee the live diagnostics stream to track the running real-drop
    // count. This is independent of the (lazy) DiagnosticsPage, which owns its OWN
    // direct connect — so drop tracking works even if the user never opens that page.
    // The count is monotonic within a recording and reset on the start edge; the
    // result-ready handler above reads it to decide whether to raise the toast.
    connect(record_page_, &RecordPage::diagnosticsUpdated, this,
            [this](const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
                last_real_drops_ = snapshot.capture.frames_dropped_problem();
            });

    // FinalizingOverlay: MP4 remux progress ticks, only meaningful while the
    // overlay is actually showing Saving (guarded inside FinalizingOverlay itself
    // is unnecessary here — showSaving() is only ever called while state==Saving,
    // enforced by the STOPPING/SAVING edge-detection in onRecordChromeStateChanged).
    connect(record_page_, &RecordPage::remuxProgressChanged, this, [this](float fraction) {
        if (finalizing_overlay_ && record_status_label_ == QStringLiteral("SAVING"))
            finalizing_overlay_->showSaving(static_cast<int>(fraction * 100.0f + 0.5f));
    });

    // AUDIO-DEGRADED-NOTIFY-R1 (ADR 0046 follow-up): a second, independent tee of the
    // same live diagnostics stream — raises/refreshes/clears the standing "audio source
    // went silent" toast from AudioDiagnostics.source_degraded/degraded_sources. Calm,
    // informative, never alarmist (CLAUDE.md); the recording itself is unaffected.
    connect(record_page_, &RecordPage::diagnosticsUpdated, this,
            [this](const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
                updateAudioSourceDegradedNotification(snapshot);
            });

    // ── RECORDING-ERROR-MODAL-R1: modal failure dialog ──
    // A non-disk-space recording failure opens a prominent modal with the failure
    // detail and an opt-in error report. Independent of show_notifications.
    connect(record_page_, &RecordPage::recordingFailed, this,
            [this](const ui::dialogs::RecordingErrorModel& model) { openRecordingErrorOverlay(model); });

    // The engine is recording this HDR10 display without the webcam and cursor
    // overlays. The preview has already dropped its picture-in-picture to match; say
    // why, rather than leaving the user to notice the absence in the finished file.
    connect(record_page_, &RecordPage::webcamOverlayOmitted, this, [this]() {
        if (!notification_manager_)
            return;
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::OverlayOmitted;
        event.title = QStringLiteral("Webcam not recorded");
        event.body = QStringLiteral("This display's HDR10 format cannot carry the webcam or cursor overlay. "
                                    "The recording itself is unaffected.");
        event.action = notifications::NotificationAction::OpenDiagnostics;
        notification_manager_->Enqueue(std::move(event));
    });

    // ── CAPTURE-FRAME-BUTTON-R1: "Frame saved" success toast ──
    // Triggered by RecordPage::captureFrameSaved when a frame PNG is written.
    connect(record_page_, &RecordPage::captureFrameSaved, this, [this](const QString& frame_path) {
        if (!notification_manager_)
            return;
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::Saved;
        event.title = QStringLiteral("Frame saved");
        const QString filename = QFileInfo(frame_path).fileName();
        const QString folder = QFileInfo(frame_path).dir().path();
        event.body = filename.isEmpty() ? folder : QStringLiteral("%1 — %2").arg(filename, folder);
        event.action = notifications::NotificationAction::OpenFolder;
        event.action_payload = frame_path;
        notification_manager_->Enqueue(std::move(event));
    });

    // Record-page quick action (frame capture, split request) rejected or failed.
    // Success is silent by design (RecordPage::captureActionFailed doc comment);
    // only this failure path surfaces to the user, and only via the app-wide
    // toast — never an in-page widget, so it never affects the preview's layout.
    connect(record_page_, &RecordPage::captureActionFailed, this, [this](const QString& message) {
        if (!notification_manager_)
            return;
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::CaptureActionFailed;
        event.title = QStringLiteral("Action failed");
        event.body = message;
        notification_manager_->Enqueue(std::move(event));
    });

    // ── Trigger 4: RecoveryAvailable is enqueued in checkAndShowRecoveryOverlay() ──
    // (Wired there directly to avoid duplicating the candidate-count check.)

    // NOTIFY-SKIN-R1: wire unread badge → tray presence.
    // Increment when an actionable toast becomes visible; clear on window focus.
    if (tray_presence_ && notification_manager_) {
        connect(notification_manager_, &notifications::NotificationManager::actionableEventShown, this, [this]() {
            if (tray_presence_)
                tray_presence_->incrementUnreadCount();
        });
    }
}

void MainWindow::updateNotificationToastsEnabled() {
    // The manager owns the gate: with toasts disabled it suppresses (and clears)
    // the visible set while still recording every event to the hub. The toast
    // window auto-hides when the manager's visible set empties.
    if (notification_manager_)
        notification_manager_->SetToastsEnabled(persisted_settings_.show_notifications);
    if (!persisted_settings_.show_notifications && notification_toast_window_) {
        notification_toast_window_->hide();
    }
}

void MainWindow::recordEventInHub(const notifications::NotificationEvent& event) {
    using notifications::NotificationAction;
    using notifications::NotificationType;

    if (!notification_hub_)
        return;

    // UpdateAvailable keeps its dedicated hub wiring in onUpdateCheckComplete():
    // that path also CLEARS the advisory when a later check reports up-to-date,
    // which a pure append feed cannot do.
    if (event.type == NotificationType::UpdateAvailable)
        return;

    // Standing conditions and the preset switch keep one entry each (the newest
    // state of the condition); finished events append under their sequence.
    QString id;
    switch (event.type) {
    case NotificationType::LowStorage:
        id = QStringLiteral("low-disk");
        break;
    case NotificationType::RecoveryAvailable:
        id = QStringLiteral("recovery-available");
        break;
    case NotificationType::PresetSwitched:
        // Only the latest switch is undoable (a single undo slot exists).
        id = QStringLiteral("preset-switched");
        break;
    case NotificationType::AudioSourceDegraded:
        // One entry for the current degraded set; replaced in place as the count
        // changes and removed outright once every source reactivates (ADR 0046).
        id = QStringLiteral("audio-source-degraded");
        break;
    default:
        id = QStringLiteral("evt-%1").arg(event.sequence);
        break;
    }

    const QString status = notifications::AdvisoryStatusForType(event.type);

    // Map the toast's primary action onto the hub's action contract. Navigation
    // actions ride the deep-link route; file/undo actions carry an opaque target
    // the deepLinkRequested handler resolves back to dispatchNotificationAction.
    QString action_id;
    QString action_label;
    bool is_deep_link = false;
    switch (event.action) {
    case NotificationAction::ChangeFolder:
        action_id = QStringLiteral("settings/output");
        action_label = QStringLiteral("Change folder");
        is_deep_link = true;
        break;
    case NotificationAction::OpenRecovery:
        action_id = QStringLiteral("recovery-view");
        action_label = QStringLiteral("Recover");
        break;
    case NotificationAction::OpenDiagnostics:
        action_id = QStringLiteral("diagnostics");
        action_label = QStringLiteral("View diagnostics");
        is_deep_link = true;
        break;
    case NotificationAction::OpenHotkeys:
        action_id = QStringLiteral("settings/hotkeys");
        action_label = QStringLiteral("Rebind");
        is_deep_link = true;
        break;
    case NotificationAction::Edit:
    case NotificationAction::OpenFolder:
    case NotificationAction::ShowFile:
        // In the hub the useful leftover of a save is finding the file again.
        if (!event.action_payload.isEmpty()) {
            action_id = QStringLiteral("reveal:") + event.action_payload;
            action_label = QStringLiteral("Show in folder");
        }
        break;
    case NotificationAction::UndoPresetSwitch:
        action_id = QStringLiteral("preset-undo");
        action_label = QStringLiteral("Undo");
        break;
    case NotificationAction::RelaunchElevated:
        action_id = QStringLiteral("present-needs-admin");
        action_label = QStringLiteral("Restart as administrator");
        is_deep_link = true;
        break;
    default:
        break;
    }

    notification_hub_->removeAdvisoryById(id);
    notification_hub_->addAdvisory(id, status, event.title, event.body, QStringLiteral("now"),
                                   /*unread=*/true, action_id, action_label, is_deep_link);
    refreshHubUnreadBell();
}

void MainWindow::updateAudioSourceDegradedNotification(const recorder_core::RecordingDiagnosticsSnapshot& snapshot) {
    if (!notification_manager_)
        return;

    // Only a live recording/paused snapshot can report a degraded source; anything
    // else (idle, completed, failed, a stale/earlier-generation snapshot) reads as
    // "not degraded" so the toast clears on its own at the end of the session.
    const bool recording_or_paused =
        snapshot.valid && (snapshot.lifecycle == recorder_core::DiagnosticsLifecycle::Recording ||
                           snapshot.lifecycle == recorder_core::DiagnosticsLifecycle::Paused);
    const uint32_t count =
        (recording_or_paused && snapshot.audio.source_degraded) ? snapshot.audio.degraded_sources : 0;

    if (count == audio_degraded_source_count_)
        return; // unchanged since the last tick — never re-announce the same condition

    if (count == 0) {
        clearAudioSourceDegradedNotification();
        return;
    }

    // The degraded set changed (newly degraded, or the count moved) — replace any
    // previous standing toast for this recording rather than stacking a second one.
    if (audio_degraded_notification_sequence_ != 0)
        notification_manager_->Dismiss(audio_degraded_notification_sequence_);

    audio_degraded_notification_sequence_ =
        notification_manager_->Enqueue(notifications::MakeAudioSourceDegradedEvent(count));
    audio_degraded_source_count_ = count;
}

void MainWindow::clearAudioSourceDegradedNotification() {
    if (audio_degraded_notification_sequence_ != 0) {
        if (notification_manager_)
            notification_manager_->Dismiss(audio_degraded_notification_sequence_);
        audio_degraded_notification_sequence_ = 0;
    }
    audio_degraded_source_count_ = 0;
    if (notification_hub_) {
        notification_hub_->removeAdvisoryById(QStringLiteral("audio-source-degraded"));
        refreshHubUnreadBell();
    }
}

void MainWindow::onPresentDiagnosticsOptInToggled(bool enabled) {
    // Persist the opt-in regardless of elevation: the flag survives the self-relaunch
    // so the elevated instance can activate the provider (ADR 0033).
    persisted_settings_.present_diagnostics_optin = enabled;
    settings_store_.Save(persisted_settings_);

    // Start or stop the ETW session to match the new opt-in state.
    present_provider_.SetOptIn(enabled);
    // Keep the kernel DPC/ISR session in lockstep with the same gate (opt-in && elevation).
    if (enabled && elevation_provider_.IsElevated()) {
        [[maybe_unused]] const bool dpc_started = dpc_provider_.Start();
    } else {
        dpc_provider_.Stop();
    }

    if (!notification_hub_)
        return;

    static const QString kAdvisoryId = QStringLiteral("present-needs-admin");
    if (enabled && !elevation_provider_.IsElevated()) {
        // Offer the elevated relaunch. The advisory's deep-link target IS the advisory
        // id; the deepLinkRequested handler routes it to RelaunchElevated (which carries
        // the recording guard).
        notification_hub_->addAdvisory(kAdvisoryId, QStringLiteral("info"),
                                       QStringLiteral("Present diagnostics need administrator"),
                                       QStringLiteral("Restart ExoSnap as administrator to enable present & tearing "
                                                      "diagnostics."),
                                       QStringLiteral("now"), /*unread=*/true, kAdvisoryId,
                                       QStringLiteral("Restart as administrator"), /*is_deep_link=*/true);
        refreshHubUnreadBell();
    } else {
        // Off, or already elevated — no advisory needed.
        notification_hub_->removeAdvisoryById(kAdvisoryId);
        refreshHubUnreadBell();
    }
}

bool MainWindow::applyCrashConsentAction(CrashConsentAction action) {
    switch (action) {
    case CrashConsentAction::None:
        return true;
    case CrashConsentAction::SendPendingOnce:
        return crash_capture::SendPendingReportOnce();
    case CrashConsentAction::GrantPersistent:
        crash_capture::GiveUserConsent();
        return true;
    case CrashConsentAction::ResetToAsk:
        crash_capture::ResetUserConsent();
        return true;
    case CrashConsentAction::Revoke:
        crash_capture::RevokeUserConsent();
        return true;
    }
    return false;
}

void MainWindow::onCrashReportPolicyChanged(CrashReportPolicy policy) {
    persisted_settings_.crash_report_policy = policy;
    settings_store_.Save(persisted_settings_);

    if (policy == CrashReportPolicy::AlwaysSend) {
        applyCrashConsentAction(CrashConsentAction::GrantPersistent);
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Crash-report policy changed to Send automatically"));
    } else if (policy == CrashReportPolicy::NeverSend) {
        applyCrashConsentAction(CrashConsentAction::Revoke);
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Crash-report policy changed to Never send; consent revoked"));
    } else {
        applyCrashConsentAction(CrashConsentAction::ResetToAsk);
        diagnostics::AppLog::info(QStringLiteral("crash"),
                                  QStringLiteral("Crash-report policy changed to Ask every time"));
    }
}

void MainWindow::dispatchNotificationAction(const notifications::NotificationEvent& event,
                                            notifications::NotificationAction action) {
    using notifications::NotificationAction;
    switch (action) {
    case NotificationAction::Edit: {
        // Navigate to the Edit/Output page for the saved recording. The toast only
        // carries the output path (event.action_payload); recover the full metadata
        // (duration, size, resolution, codecs, container, markers) the same way the
        // result-panel Edit button and Recent-menu Edit action already do, via
        // RecordPage's current-recording/history lookup — not the bare-path stub
        // this used to build, which left every detail row and the Edit timeline
        // showing "–" / 00:00.
        const QString path = event.action_payload.trimmed();
        exosnap::EditContext ctx;
        ctx.output_path = path;
        ctx.mkv_master_path = path; // best-effort fallback if record_page_ is unavailable
        if (record_page_)
            ctx = record_page_->editContextForOutputPath(path);
        navigateToEditExportPage(ctx);
        break;
    }
    case NotificationAction::OpenFolder: {
        // Payload is the saved file (or folder). Open its containing folder —
        // mirrors RecordPage::openOutputFolder's QDesktopServices folder path.
        const QString path = event.action_payload.trimmed();
        if (path.isEmpty())
            return;
        const QFileInfo info(path);
        const QString folder = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
        if (!folder.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        break;
    }
    case NotificationAction::ShowFile: {
        // Reveal the partial file. Open the file directly when it exists, else its
        // folder — same QDesktopServices reveal path the result actions already use.
        const QString path = event.action_payload.trimmed();
        if (path.isEmpty())
            return;
        const QFileInfo info(path);
        if (info.exists() && info.isFile())
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
        else if (!info.absolutePath().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
        break;
    }
    case NotificationAction::ChangeFolder: {
        // Route to Settings → Output, identical to the low-disk hub deep-link.
        navigateToPage(kSettingsPageIndex);
        if (config_page_)
            config_page_->scrollToSection(QStringLiteral("settings/output"));
        break;
    }
    case NotificationAction::OpenRecovery: {
        // Reopen the recovery overlay if it still exists; otherwise land on Record.
        if (recovery_overlay_ != nullptr)
            recovery_overlay_->openOverlay();
        else
            navigateToPage(kRecordPageIndex);
        break;
    }
    case NotificationAction::OpenUpdate: {
        // Navigate to Settings and scroll to the updates card (ADR 0034).
        navigateToPage(kSettingsPageIndex);
        if (config_page_)
            config_page_->scrollToSection(QStringLiteral("settings/updates"));
        break;
    }
    case NotificationAction::OpenHotkeys: {
        // HotkeyConflict: jump to Settings → Hotkeys so the user can bind a working
        // shortcut for the action ExoSnap had to drop at startup.
        navigateToPage(kSettingsPageIndex);
        if (config_page_)
            config_page_->scrollToSection(QStringLiteral("settings/hotkeys"));
        break;
    }
    case NotificationAction::OpenDiagnostics: {
        // DROP-NOTIFY: jump to the Diagnostics page, where the per-stage frame-drop
        // breakdown (coalesce / cfr / backpressure) is rendered live. The page builds
        // lazily on first navigation, so navigateToPage is self-sufficient here.
        navigateToPage(kDiagnosticsPageIndex);
        break;
    }
    case NotificationAction::RelaunchElevated: {
        // ELEVATION-FOUNDATION-R1 (ADR 0033): relaunch as administrator to unlock
        // elevation-gated diagnostics. Recording guard: never relaunch during an
        // active recording or remux — defer the offer until the session stops
        // (mirrors the no-update-during-recording rule, ADR 0012).
        if (recording_active_ || remuxing_active_) {
            diagnostics::AppLog::info(QStringLiteral("diagnostics"),
                                      QStringLiteral("Elevated relaunch deferred: a recording/remux is active."));
            break;
        }
        // The relaunch unlocks the present-diagnostics opt-in, so re-enable it in
        // the elevated instance via the handoff arg. The opt-in is only persisted
        // there (after UAC succeeds), so a decline leaves the current state intact.
        reenable_present_diag_on_relaunch_ = true;
        elevated_relaunch_requested_ = true;
        diagnostics::AppLog::info(QStringLiteral("diagnostics"),
                                  QStringLiteral("User accepted relaunch as administrator."));
        qApp->quit();
        break;
    }
    case NotificationAction::UndoPresetSwitch: {
        if (!pending_preset_undo_)
            break; // already consumed by a later Undo click, or never set
        const PresetSwitchUndo undo = std::move(*pending_preset_undo_);
        pending_preset_undo_.reset();
        if (!preset_registry_.SetSelected(undo.previous_selected_id))
            preset_registry_.SetSelected(std::string(kDefaultPresetId)); // previous preset was deleted meanwhile
        // Overlay the *current* environment fields (capture target, bit depth, HDR
        // mode) rather than the ones captured before the switch — the display/HDR
        // state may have changed in the meantime, and Undo must not stomp it.
        applyPresetConfig(WithEnvironmentFields(undo.previous_live, captureLiveConfig()));
        persistPresetState();
        break;
    }
    case NotificationAction::Discard:
    case NotificationAction::None:
    default:
        // No navigation — the toast was already dismissed by the manager.
        break;
    }
}

// ---------------------------------------------------------------------------
// ELEVATION-FOUNDATION-R1 (ADR 0033): elevated self-relaunch handoff
// ---------------------------------------------------------------------------

QStringList MainWindow::elevatedRelaunchArgs() const {
    services::RelaunchHandoff handoff;
    if (stack_ != nullptr) {
        const int index = stack_->currentIndex();
        if (index >= 0 && index < static_cast<int>(kPageDescriptors.size()))
            handoff.page_name = QString::fromUtf8(kPageDescriptors[static_cast<std::size_t>(index)].nav_label);
    }
    handoff.reenable_present_diag = reenable_present_diag_on_relaunch_;
    return services::BuildRelaunchArgs(handoff);
}

void MainWindow::applyStartupRelaunchHandoff(const QString& page_name, bool reenable_present_diag) {
    // Land on the handed-off page, if it resolves to a real nav target.
    const int index = pageIndexForNavLabel(page_name);
    if (index >= 0)
        navigateToPage(index);

    // The relaunch succeeded (we are running), so it is now safe to persist the
    // present-diagnostics opt-in the user toggled before the restart.
    if (reenable_present_diag && !persisted_settings_.present_diagnostics_optin) {
        persisted_settings_.present_diagnostics_optin = true;
        settings_store_.Save(persisted_settings_);
        diagnostics::AppLog::info(QStringLiteral("diagnostics"),
                                  QStringLiteral("Present-diagnostics opt-in re-enabled after elevated relaunch."));
    }
}

void MainWindow::applyVerifyUpdateReinstallMode(bool enabled) {
    verify_update_reinstall_ = enabled;
    if (!enabled)
        return;
    // The service owns the engine-side opt-in (check params + updater argv) and
    // writes the "mode active" log line.
    if (update_service_)
        update_service_->SetVerifyReinstallMode(true);
}

// ---------------------------------------------------------------------------
// PS-PHASE-E: hub bell unread count refresh
// ---------------------------------------------------------------------------

void MainWindow::refreshHubUnreadBell() {
    if (!notification_hub_ || !title_bar_)
        return;
    title_bar_->setBellUnreadStatus(notification_hub_->worstUnreadStatus());
}

// ---------------------------------------------------------------------------
// UPDATE-WIRE-R1 (ADR 0012): update check + result handling
// ---------------------------------------------------------------------------

void MainWindow::triggerUpdateCheck() {
    if (update_service_ == nullptr)
        return;

    // App-layer recording guard: never contact the update server while a recording
    // or MP4 remux is in flight. The Settings card's action is already disabled by
    // setRecordingControlsLocked, so its state simply stays where it was.
    if (recording_active_ || remuxing_active_) {
        diagnostics::AppLog::info(QStringLiteral("update"),
                                  QStringLiteral("Update check skipped — recording/finalizing in progress"));
        return;
    }

    if (config_page_)
        config_page_->setUpdateStatus(QStringLiteral("checking"), QString(), QString());
    update_service_->RequestUpdateCheck();
}

void MainWindow::onUpdateCheckComplete(const update::UpdateCheckResult& result) {
    const QString current_version = QString::fromLatin1(exosnap::build::kVersion);
    const QString channel =
        update_service_ ? UpdateChannelToString(update_service_->Channel()) : persisted_settings_.update_channel;

    // The engine result exposes only a releases-page URL (no tag-specific notes body
    // / html URL). Map that to both the releases link and the notes link; leave
    // whats_new empty since there is no structured changelog from the check.
    last_update_releases_url_ = result.releases_page_url
                                    ? QString::fromStdString(*result.releases_page_url)
                                    : QStringLiteral("https://github.com/Exoridus/exosnap/releases");

    // A verification reinstall was granted on byte-identical version STRINGS, so
    // the running version string is the truthful label for it — SemVer::ToString
    // would re-render a foreign prerelease label as "-rc0".
    const QString available_version =
        result.verification_reinstall
            ? current_version
            : (result.available_version ? QString::fromStdString(result.available_version->ToString()) : QString());
    last_available_version_ = available_version;

    const QString last_checked = QDateTime::currentDateTime().toString(QStringLiteral("MMM d, h:mm AP"));

    if (result.check_failed || result.error_message) {
        const QString error_message = result.error_message ? QString::fromStdString(*result.error_message)
                                                           : QStringLiteral("Couldn't reach the update server.");
        if (config_page_)
            config_page_->setUpdateStatus(QStringLiteral("error"), QString(), QString(), error_message);
        // "Update checking disabled (unofficial build)" is an expected condition for
        // self/unofficial builds, not a failure — log it at info level so it doesn't
        // surface as a warning in the Logs view.
        if (error_message.contains(QStringLiteral("disabled"), Qt::CaseInsensitive))
            diagnostics::AppLog::info(QStringLiteral("update"),
                                      QStringLiteral("Update check skipped: %1").arg(error_message));
        else
            diagnostics::AppLog::warning(QStringLiteral("update"),
                                         QStringLiteral("Update check failed: %1").arg(error_message));
        manual_update_check_ = false;
        return;
    }

    if (config_page_) {
        // Loop-guard / recovery semantics live in the pure ResolveUpdateCardState
        // helper. A manual check clears applied_version before the check runs, so a
        // stuck "Restart pending" re-arms to "available" for a still-applicable
        // version; automatic checks keep available==applied pinned to "pending".
        const bool is_scoop = UpdateService::IsScoopManagedInstall(QCoreApplication::applicationDirPath());
        const QString card_state = exosnap::ResolveUpdateCardState(
            result.update_available, is_scoop, persisted_settings_.applied_version, available_version,
            verify_update_reinstall_, current_version, update_handoff_phase_);
        config_page_->setUpdateStatus(card_state, available_version, last_checked);
    }

    diagnostics::AppLog::info(
        QStringLiteral("update"),
        result.verification_reinstall
            // Never phrased as an available update: nothing newer exists here.
            ? QStringLiteral("Verification reinstall offered for %1 (%2)").arg(available_version, channel)
            : result.update_available
                  ? QStringLiteral("Update available: %1 → %2 (%3)").arg(current_version, available_version, channel)
                  : QStringLiteral("Up to date (%1, %2)").arg(current_version, channel));

    // Notify-on-available: hub advisory (persistent) + a transient toast.
    // ADR 0034: the advisory deep-links to the Settings updates card.
    if (notification_hub_) {
        // Always clear stale advisory first (handles: up-to-date after was-available).
        notification_hub_->removeAdvisoryById(QStringLiteral("update-available"));
        // A verification reinstall is not an available update and must not be
        // advertised as one — the user asked for it explicitly and is looking at
        // the card that offers it.
        if (result.update_available && !result.verification_reinstall) {
            notification_hub_->addAdvisory(QStringLiteral("update-available"), QStringLiteral("info"),
                                           QStringLiteral("Update available \xe2\x80\x94 %1").arg(available_version),
                                           QStringLiteral("Signature verified. Open Settings to update."),
                                           QStringLiteral("now"), /*unread=*/true, QStringLiteral("settings/updates"),
                                           QStringLiteral("Open in Settings"),
                                           /*is_deep_link=*/true);
            refreshHubUnreadBell();
        }
    }

    // Transient toast only on an *automatic* check — a manual check already has the
    // user looking at the Settings card, so a popup would be redundant (ADR 0034).
    if (result.update_available && !result.verification_reinstall && !manual_update_check_ && notification_manager_) {
        notifications::NotificationEvent event;
        event.type = notifications::NotificationType::UpdateAvailable;
        event.title = QStringLiteral("Update available — %1").arg(available_version);
        event.body = QStringLiteral("%1 channel · %2 → %3").arg(channel, current_version, available_version);
        event.action = notifications::NotificationAction::OpenUpdate;
        event.secondary_action = notifications::NotificationAction::None;
        notification_manager_->Enqueue(std::move(event));
    }

    manual_update_check_ = false;
}

void MainWindow::navigateToEditExportPage(const EditContext& ctx) {
    // EDIT-OVERLAY-R1 (review): a live capture owns the Record surface — opening
    // the editor over a running recording/countdown makes no sense (on main the
    // stack swap-back would have immediately undone it anyway). A toast's Edit
    // action simply stays available until after Stop.
    if (recording_active_ || record_status_label_ == QStringLiteral("COUNTDOWN"))
        return;
    if (!edit_export_overlay_)
        buildEditExportOverlay();
    // A running export must never be clobbered by a new context (stale toast Edit
    // pill, Recent-menu action). If its overlay was dismissed (e.g. by a recording
    // start), re-show the in-progress export instead of resetting the page.
    if (edit_export_overlay_->isDismissBlocked()) {
        navigateToPage(kRecordPageIndex);
        edit_export_overlay_->openOverlay();
        return;
    }
    // Activate Record first (if not already active) — the overlay is only
    // meaningful "over Record" — then populate and open the overlay itself.
    navigateToPage(kRecordPageIndex);
    edit_export_overlay_->page()->setEditContext(ctx);
    edit_export_overlay_->openOverlay();
}

// ---- Staged post-show page hydration (PERF-B1) ----

void MainWindow::hydrateSecondaryPages() {
    // Register one deferred page builder per event-loop tick so the UI can paint
    // and respond between constructors. Order: ConfigPage first — it is the
    // heaviest and the most commonly visited item; then DevicePage (construction
    // only — the adapter scan runs async on its first showEvent),
    // DiagnosticsPage, LogsPage, AboutPage, EditExportOverlay (not a stack page —
    // see EDIT-OVERLAY-R1). Hotkeys and webcam configuration are embedded cards in
    // ConfigPage, so they hydrate with it and need no separate step.
    //
    // PageHydrationController owns the singleShot(0) staging and the "perf"
    // AppLog milestone bracketing (same "<name> <elapsed> ms" convention as
    // first-paint / preview-live) so a later optimization slice has a per-page
    // cost baseline; this method only supplies the ordered list of page builders.
    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("config"), [this] { buildConfigPage(); }});
    steps.push_back({QStringLiteral("device"), [this] { buildDevicePage(); }});
    steps.push_back({QStringLiteral("diagnostics"), [this] { buildDiagnosticsPage(); }});
    steps.push_back({QStringLiteral("logs"), [this] { buildLogsPage(); }});
    steps.push_back({QStringLiteral("about"), [this] { buildAboutPage(); }});
    steps.push_back({QStringLiteral("edit-export"), [this] { buildEditExportOverlay(); }});

    page_hydration_controller_ = new PageHydrationController(std::move(steps), this);
    page_hydration_controller_->start();
}

void MainWindow::buildConfigPage() {
    if (config_page_)
        return; // already built (e.g. by an early navigation or visual harness)
    config_page_ = new ConfigPage(output_settings_, video_settings_, stack_);
    config_page_->setHotkeyService(hotkey_service_);
    if (config_placeholder_) {
        // Replace the placeholder in-place so kSettingsPageIndex stays valid for all
        // widgets already past it in the stack (diagnostics=3, logs=4, about=5).
        const int idx = stack_->indexOf(config_placeholder_);
        stack_->insertWidget(idx, config_page_);
        config_placeholder_->deleteLater();
        config_placeholder_ = nullptr;
    } else {
        stack_->addWidget(config_page_);
    }

    // ---- Initial setters (previously in ctor) ----
    config_page_->setAudioUiState(live_audio_);
    config_page_->setWebcamSettings(live_webcam_);

    // SETTINGS-TIERS-R1: expert mode toggle + per-card expander state.
    config_page_->setExpertModeEnabled(persisted_settings_.expert_mode_enabled);
    config_page_->setAudioSeparateExpanderExpanded(persisted_settings_.audio_separate_expander_expanded);
    connect(config_page_, &ConfigPage::expertModeChanged, this, [this](bool enabled) {
        persisted_settings_.expert_mode_enabled = enabled;
        settings_store_.Save(persisted_settings_);
        // Single global Expert state: mirror onto the Diagnostics page (no-op guarded).
        if (diagnostics_page_)
            diagnostics_page_->setExpertModeEnabled(enabled);
    });
    connect(config_page_, &ConfigPage::audioSeparateExpanderChanged, this, [this](bool expanded) {
        persisted_settings_.audio_separate_expander_expanded = expanded;
        settings_store_.Save(persisted_settings_);
    });

    // SETTINGS-HONESTY-R1: Developer card log-level combo — genuinely wired (was a
    // UI-only stub). Seed from the persisted value, persist + apply on change.
    config_page_->setDeveloperLogLevel(persisted_settings_.developer_log_level);
    connect(config_page_, &ConfigPage::developerLogLevelChanged, this, [this](const QString& level) {
        diagnostics::AppLog::info(QStringLiteral("settings"),
                                  QStringLiteral("Developer log level changed to %1").arg(level));
        persisted_settings_.developer_log_level = level;
        settings_store_.Save(persisted_settings_);
        diagnostics::AppLog::setMinSeverity(DeveloperLogLevelFromString(level));
    });

    // ---- Format / preset / video / audio / webcam signal connects ----
    connect(config_page_, &ConfigPage::formatSettingsChanged, this, [this](const OutputSettingsModel& settings) {
        if (applying_preset_)
            return;
        // ONE merge function (unit-tested) instead of ad-hoc field copies: the old
        // per-field copy silently dropped color_range and bit_depth, so those combo
        // selections never reached output_settings_ (and thus never the recording).
        MergeFormatSelection(output_settings_, settings);
        record_page_->setOutputSettings(output_settings_);
        onLiveConfigChanged();
        // CRASH-WIRE-R1: container/codec context changed — refresh the sidecar.
        refreshCrashSessionContext();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::presetSelected, this, [this](const QString& id) { onPresetSelected(id); });
    connect(config_page_, &ConfigPage::videoSettingsChanged, this, [this](const VideoSettingsModel& settings) {
        if (applying_preset_)
            return;
        video_settings_ = settings;
        record_page_->setVideoSettings(settings);
        onLiveConfigChanged();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        if (applying_preset_)
            return;
        live_audio_ = state;
        if (record_page_)
            record_page_->applyPersistedAudioSettings(state);
        onLiveConfigChanged();
        refreshDiagnosticsData();
    });
    connect(config_page_, &ConfigPage::webcamSettingsChanged, this, [this](const WebcamSettings& settings) {
        if (applying_preset_)
            return;
        live_webcam_ = settings;
        record_page_->setWebcamSettings(settings);
        onLiveConfigChanged();
    });

    // Shared webcam capture: the Settings panel is a consumer of the coordinator's single
    // reader. Route its "I want a preview" request to the coordinator, and fan the
    // coordinator's frames back into the panel — so both surfaces show one capture, with
    // no second reader fighting for the device (works during recording too).
    connect(config_page_, &ConfigPage::webcamPreviewActiveRequested, this, [this](bool active) {
        if (record_page_)
            record_page_->setSettingsWebcamPreviewActive(active);
    });
    connect(record_page_, &RecordPage::webcamFrameReady, this, [this](const QImage& frame) {
        if (config_page_)
            config_page_->setWebcamPreviewFrame(frame);
    });

    // ---- Preset management operations ----
    connect(config_page_, &ConfigPage::savePresetAsRequested, this, &MainWindow::onSavePresetAs);
    connect(config_page_, &ConfigPage::renamePresetRequested, this, &MainWindow::onRenamePreset);
    connect(config_page_, &ConfigPage::deletePresetRequested, this, &MainWindow::onDeletePreset);
    connect(config_page_, &ConfigPage::resetChangesRequested, this, &MainWindow::onResetChanges);
    connect(config_page_, &ConfigPage::exportCurrentPresetRequested, this, &MainWindow::onExportSelectedProfile);
    connect(config_page_, &ConfigPage::importPresetsRequested, this, &MainWindow::onImportProfiles);

    // ---- CRITICAL: direct connect (record_page_ → config_page_) ----
    // This must live in buildConfigPage() so the receiver pointer is valid when the
    // connection is made. The live audio-meter feed into Settings would be silently
    // lost if this were attempted before config_page_ exists.
    // CRITICAL: record_page_ is built unconditionally in the ctor and is always valid here.
    connect(record_page_, &RecordPage::audioMeterLevelsUpdated, config_page_, &ConfigPage::setAudioMeterLevels);

    // ---- Navigation connects ----
    connect(config_page_, &ConfigPage::diagnosticsRequested, this, [this]() {
        refreshDiagnosticsData();
        navigateToPage(kDiagnosticsPageIndex);
    });

    // ---- Overlay / tray / quick-controls / theme setters + connects ----
    config_page_->setShowOverlay(persisted_settings_.show_recording_overlay);
    connect(config_page_, &ConfigPage::showOverlayChanged, this, [this](bool show) {
        persisted_settings_.show_recording_overlay = show;
        settings_store_.Save(persisted_settings_);
        updateRecordingOverlay();
    });
    config_page_->setShowDiagnosticsOverlay(persisted_settings_.show_diagnostics_overlay);
    connect(config_page_, &ConfigPage::showDiagnosticsOverlayChanged, this, [this](bool show) {
        persisted_settings_.show_diagnostics_overlay = show;
        settings_store_.Save(persisted_settings_);
        updateDiagnosticsOverlay();
    });
    config_page_->setKeepRunningInTray(persisted_settings_.keep_running_in_tray);
    connect(config_page_, &ConfigPage::keepRunningInTrayChanged, this, [this](bool keep) {
        persisted_settings_.keep_running_in_tray = keep;
        settings_store_.Save(persisted_settings_);
    });
    config_page_->setShowQuickControls(persisted_settings_.show_quick_controls);
    connect(config_page_, &ConfigPage::showQuickControlsChanged, this, [this](bool show) {
        persisted_settings_.show_quick_controls = show;
        settings_store_.Save(persisted_settings_);
        if (quick_control_pill_)
            quick_control_pill_->setShowQuickControls(show);
        updateQuickControlPill();
    });
    config_page_->setThemeId(persisted_settings_.theme_id);
    connect(config_page_, &ConfigPage::themeIdChanged, this, [this](const QString& id) {
        persisted_settings_.theme_id = id;
        settings_store_.Save(persisted_settings_);
        // ReapplyTheme() swaps the app stylesheet/palette AND notifies every
        // OnThemeChanged() subscriber (inline stylesheets, tinted pixmaps, the
        // two-tone brand wordmarks) — no per-widget patch-up calls here.
        ui::theme::ReapplyTheme(*qApp, id);
        for (QWidget* w : findChildren<QWidget*>())
            w->update();
    });

    // ---- Audio rescan, update card, present-diagnostics opt-in, webcam-panel rescan ----
    connect(config_page_, &ConfigPage::audioRescanRequested, &audio_notifier_, &AudioDeviceNotifier::rescan);
    config_page_->setAutoUpdateCheck(persisted_settings_.check_updates_on_start);
    config_page_->setUpdateChannel(persisted_settings_.update_channel);
    connect(config_page_, &ConfigPage::channelChanged, this, [this](const QString& channel) {
        persisted_settings_.update_channel = channel;
        settings_store_.Save(persisted_settings_);
        if (update_service_)
            update_service_->SetChannel(UpdateChannelFromString(channel));
        // Keep the hidden About-panel shim and the About page's channel metadata
        // row in sync (both still read persisted_settings_.update_channel elsewhere).
        if (about_page_)
            about_page_->setChannelHint(channel);
        // Channel applies immediately: re-check on the new channel (guarded).
        triggerUpdateCheck();
    });
    connect(config_page_, &ConfigPage::checkForUpdatesRequested, this, [this]() {
        manual_update_check_ = true;
        // Recovery: a user-initiated check clears any persisted "Restart pending"
        // stamp so the card can re-arm to "Update to vX.Y" if the updater launched
        // earlier but never completed the swap. Automatic startup checks keep the
        // loop-guard semantics (available==applied stays "pending").
        if (!persisted_settings_.applied_version.isEmpty()) {
            persisted_settings_.applied_version.clear();
            settings_store_.Save(persisted_settings_);
        }
        triggerUpdateCheck();
    });
    connect(config_page_, &ConfigPage::updatePrimaryActionRequested, this, [this]() {
        // App-layer recording guard (belt-and-suspenders alongside UpdateService's own
        // engine-layer guard, wired via SetRecordingCoordinator() above). Never
        // stage/launch the swap updater while a recording or MP4 finalize is in
        // flight — mirror how triggerUpdateCheck() gates the check path.
        if (recording_active_ || remuxing_active_) {
            if (config_page_)
                config_page_->setUpdateStatus(QStringLiteral("error"), QString(), QString(),
                                              QStringLiteral("Finish the recording before updating."));
            diagnostics::AppLog::info(QStringLiteral("update"),
                                      QStringLiteral("Update launch skipped — recording/finalizing in progress"));
            return;
        }
        // Scoop installs are notify-only: the primary button opens the releases
        // page (today's behavior); the staged swap must not touch a Scoop tree.
        if (UpdateService::IsScoopManagedInstall(QCoreApplication::applicationDirPath())) {
            const QString url = last_update_releases_url_.isEmpty()
                                    ? QStringLiteral("https://github.com/Exoridus/exosnap/releases")
                                    : last_update_releases_url_;
            QDesktopServices::openUrl(QUrl(url));
            return;
        }
        // Otherwise stage + launch the swap updater. The app keeps running and
        // exits normally when the updater sends WM_CLOSE.
        if (update_service_)
            update_service_->LaunchUpdater();
    });
    // WHATS-NEW: card "See what's new in vX.Y" link -> overlay with the full channel
    // history from the last check (pre-update mode; no suppress checkbox). Never
    // gated by the suppress setting.
    connect(config_page_, &ConfigPage::whatsNewRequested, this, [this]() {
        if (!update_service_)
            return;
        QVector<WhatsNewNote> notes;
        for (const auto& n : update_service_->LastAllChannelNotes()) {
            WhatsNewNote note;
            note.version = QString::fromStdString(n.version.ToString());
            note.body = QString::fromStdString(n.body_markdown);
            note.html_url = QString::fromStdString(n.html_url);
            notes.push_back(note);
        }
        openWhatsNewOverlay(notes, /*post_update_mode=*/false);
    });
    connect(config_page_, &ConfigPage::autoUpdateCheckToggled, this, [this](bool enabled) {
        persisted_settings_.check_updates_on_start = enabled;
        settings_store_.Save(persisted_settings_);
    });
    config_page_->setPresentDiagnosticsOptIn(persisted_settings_.present_diagnostics_optin);
    connect(config_page_, &ConfigPage::presentDiagnosticsOptInToggled, this,
            &MainWindow::onPresentDiagnosticsOptInToggled);
    config_page_->setCrashReportPolicy(persisted_settings_.crash_report_policy);
    connect(config_page_, &ConfigPage::crashReportPolicyChanged, this, &MainWindow::onCrashReportPolicyChanged);
    // Route Settings webcam panel Rescan through the webcam notifier.
    // The WebcamSetupPanel is embedded in ConfigPage; access via findChild.
    auto* setup_panel =
        config_page_->findChild<exosnap::ui::widgets::WebcamSetupPanel*>(QStringLiteral("settingsWebcamSetupPanel"));
    if (setup_panel) {
        connect(setup_panel, &exosnap::ui::widgets::WebcamSetupPanel::rescanRequested, &webcam_notifier_,
                &WebcamDeviceNotifier::rescan);
        // S4: Apply MF-absent gate if the capability probe already resolved.
        if (runtime_caps_ready_ && !runtime_caps_.mf_webcam_available)
            setup_panel->setMfUnavailable(true);
    }

    // Deliver probed capabilities to the expert 4:4:4 chroma gate if the async
    // probe already resolved before this (lazily built) page existed; otherwise
    // onRuntimeCapsReady() applies them once they arrive.
    if (runtime_caps_ready_)
        config_page_->setRuntimeCapabilities(runtime_caps_);

    // ---- Notification toasts wiring (moved from initNotificationToasts()) ----
    // initNotificationToasts() runs before buildConfigPage() so config_page_ was null there.
    config_page_->setShowNotifications(persisted_settings_.show_notifications);
    connect(config_page_, &ConfigPage::showNotificationsChanged, this, [this](bool show) {
        persisted_settings_.show_notifications = show;
        settings_store_.Save(persisted_settings_);
        updateNotificationToastsEnabled();
    });

    config_page_->setOpenEditorWhenFinished(persisted_settings_.open_editor_when_finished);
    connect(config_page_, &ConfigPage::openEditorWhenFinishedChanged, this, [this](bool open) {
        persisted_settings_.open_editor_when_finished = open;
        settings_store_.Save(persisted_settings_);
    });

    // ---- Fan-out replay ----
    // applyPresetConfig delivers the full live config (output/video/audio/webcam/
    // folder/name) to the freshly-built page — the CURRENT live state, not the
    // boot snapshot, since the page can be built well after startup.
    // refreshPresetUi delivers preset combo options + active profile name +
    // dirty flag. Both are safe regardless of runtime_caps_ready_ because they
    // only update UI from in-memory state (same as what a ctor call would do).
    applyPresetConfig(captureLiveConfig());
    refreshPresetUi();

    // Chrome-state replay: deliver the current readiness + lock status that was last set by
    // onRecordChromeStateChanged. record_status_label_ is settled by the time buildConfigPage()
    // runs (rebroadcastChromeState() fired during ctor). Empty label → treat as READY.
    {
        const QString status = record_status_label_.isEmpty() ? QStringLiteral("READY") : record_status_label_;
        const QString config_status = (status == QStringLiteral("SAVED")) ? QStringLiteral("READY") : status;
        config_page_->setReadinessStatus(config_status);
        const bool locked = (status == QStringLiteral("REC") || status == QStringLiteral("PAUSED") ||
                             status == QStringLiteral("STOPPING") || status == QStringLiteral("CHECKING") ||
                             status == QStringLiteral("STARTING") || status == QStringLiteral("COUNTDOWN"));
        config_page_->setRecordingControlsLocked(locked);
        const bool hk_locked = (status == QStringLiteral("REC") || status == QStringLiteral("PAUSED") ||
                                status == QStringLiteral("STOPPING") || status == QStringLiteral("COUNTDOWN"));
        config_page_->setHotkeyEditingLocked(hk_locked);
    }
}

void MainWindow::buildLogsPage() {
    if (logs_page_)
        return; // already built (e.g. by an early navigation)
    logs_page_ = new LogsPage(stack_);
    connect(logs_page_, &LogsPage::createSupportBundleRequested, this, &MainWindow::createSupportBundle);
    if (logs_placeholder_) {
        // Replace the placeholder in-place so kLogsPageIndex stays valid for all
        // widgets already past it in the stack (about=5).
        const int idx = stack_->indexOf(logs_placeholder_);
        stack_->insertWidget(idx, logs_page_);
        logs_placeholder_->deleteLater();
        logs_placeholder_ = nullptr;
    } else {
        stack_->addWidget(logs_page_);
    }
}

void MainWindow::createSupportBundle() {
    using namespace exosnap::diagnostics;

    const QString log_dir = QFileInfo(diagnostics::AppLog::logFilePath()).absolutePath();
    if (log_dir.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Support bundle"),
                             QStringLiteral("No log directory is available yet."));
        return;
    }

    BundleInputs inputs;
    inputs.log_dir = log_dir;
    inputs.max_reports = 10;
    inputs.launch_session_id = diagnostics::AppLog::sessionId();
    inputs.created_at = QDateTime::currentDateTime().toString(Qt::ISODate);
    inputs.scrubber_version = QStringLiteral("1");
    inputs.app_version = QString::fromLatin1(build::kVersion);
    inputs.commit_sha = QString::fromLatin1(build::kGitCommit);
    inputs.verify_update_reinstall = verify_update_reinstall_;

    const auto& rt = runtime_caps_.runtime;
    inputs.capability.gpu_adapter_name = QString::fromStdString(runtime_caps_.gpu_adapter_name);
    inputs.capability.nvenc_dll_present = rt.nvidia.nvenc_dll_present;
    inputs.capability.nvenc_api_version =
        rt.nvidia.nvenc_api_version_valid ? QString::number(rt.nvidia.nvenc_api_version) : QString();
    inputs.capability.nvenc_av1 = rt.nvidia.nvenc_av1;
    inputs.capability.nvenc_hevc = rt.nvidia.nvenc_hevc;
    inputs.capability.nvenc_h264 = rt.nvidia.nvenc_h264;
    inputs.capability.nvenc_444 = rt.nvidia.nvenc_yuv444_h264 || rt.nvidia.nvenc_yuv444_hevc;
    inputs.capability.os_version_string = QString::fromStdString(rt.os.version_string);
    inputs.capability.os_build_number = QString::number(rt.os.build_number);
    inputs.capability.mf_webcam = rt.mf_webcam.available;

    for (const auto& a : capability::EnumerateAdapters()) {
        BundleAdapter ba;
        ba.name = QString::fromStdString(a.name);
        ba.vendor = a.vendor == capability::AdapterVendor::Nvidia  ? QStringLiteral("NVIDIA")
                    : a.vendor == capability::AdapterVendor::Amd   ? QStringLiteral("AMD")
                    : a.vendor == capability::AdapterVendor::Intel ? QStringLiteral("Intel")
                                                                   : QStringLiteral("Other");
        ba.kind = a.kind == capability::AdapterKind::Discrete     ? QStringLiteral("discrete")
                  : a.kind == capability::AdapterKind::Integrated ? QStringLiteral("integrated")
                                                                  : QStringLiteral("unknown");
        ba.vendor_id = a.vendor_id;
        ba.device_id = a.device_id;
        ba.dedicated_vram_bytes = a.dedicated_video_memory_bytes;
        inputs.adapters.push_back(ba);
    }

    for (const auto& d : rt.displays) {
        BundleDisplay bd;
        bd.name = QString::fromStdString(d.name);
        bd.hdr_active = d.hdr_active;
        bd.bits_per_color = d.bits_per_color;
        bd.min_luminance = d.min_luminance_nits;
        bd.max_luminance = d.max_luminance_nits;
        bd.max_full_frame_luminance = d.max_full_frame_nits;
        inputs.displays.push_back(bd);
    }

    // Settings summary from the same ConfigSummary the Diagnostics page builds.
    const std::string preset_name = preset_registry_.SelectedPreset().name;
    const ConfigSummary cs = ConfigSummary::FromCurrentSettings(
        output_settings_, video_settings_, live_audio_,
        std::filesystem::path(settings_store_.SettingsFilePath().toStdWString()), preset_name, std::string());
    QString settings_text;
    for (const auto& e : cs.entries) {
        settings_text +=
            QStringLiteral("%1: %2\n").arg(QString::fromStdString(e.label), QString::fromStdString(e.value));
    }
    inputs.settings_summary = settings_text;

    // Save dialog defaulting to the Desktop, then reveal (no auto-upload).
    const QString default_name = QStringLiteral("exosnap-support-%1.zip")
                                     .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString suggested = desktop.isEmpty() ? default_name : QDir(desktop).filePath(default_name);
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save support bundle"), suggested,
                                                      QStringLiteral("Zip archives (*.zip)"));
    if (path.isEmpty())
        return; // cancelled

    const auto entries = CollectBundleEntries(inputs);
    QString err;
    if (!WriteBundleZip(path, entries, &err)) {
        QMessageBox::warning(this, QStringLiteral("Support bundle"),
                             QStringLiteral("Could not write the support bundle: %1").arg(err));
        return;
    }

    // Reveal the file in Explorer (selecting it). This drives the OS file manager,
    // not the running ExoSnap instance.
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
}

void MainWindow::buildDiagnosticsPage() {
    if (diagnostics_page_)
        return; // already built (e.g. by an early navigation or visual harness)
    diagnostics_page_ = new DiagnosticsPage(stack_);
    diagnostics_page_->setPresentProvider(&present_provider_);
    diagnostics_page_->setDpcProvider(&dpc_provider_);
    diagnostics_page_->setElevationProvider(&elevation_provider_);
    // Light cadence to surface the selected-window exclusive-fullscreen evidence,
    // which needs ~2 s to accumulate. refreshWindowEvidence() no-ops while the page
    // is hidden, so this costs nothing off the Diagnostics view.
    if (!window_evidence_timer_) {
        window_evidence_timer_ = new QTimer(this);
        window_evidence_timer_->setInterval(1000);
        connect(window_evidence_timer_, &QTimer::timeout, this, &MainWindow::refreshWindowEvidence);
        window_evidence_timer_->start();
    }
    // Single global Expert state, shared with Settings (AppSettingsStore::expert_mode_enabled).
    diagnostics_page_->setExpertModeEnabled(persisted_settings_.expert_mode_enabled);
    connect(diagnostics_page_, &DiagnosticsPage::expertModeChanged, this, [this](bool enabled) {
        persisted_settings_.expert_mode_enabled = enabled;
        settings_store_.Save(persisted_settings_);
        if (config_page_)
            config_page_->setExpertModeEnabled(enabled);
    });
    if (diagnostics_placeholder_) {
        // Replace the placeholder in-place so kDiagnosticsPageIndex stays valid for all
        // widgets already past it in the stack (logs=4, webcam=5, output=6, about=7).
        const int idx = stack_->indexOf(diagnostics_placeholder_);
        stack_->insertWidget(idx, diagnostics_page_);
        diagnostics_placeholder_->deleteLater();
        diagnostics_placeholder_ = nullptr;
    } else {
        stack_->addWidget(diagnostics_page_);
    }
    // ---- FixAction routing (ADR 0033 / v0.8.0) ----
    // Auto fixes apply a settings change directly after a confirm; Assisted fixes
    // navigate to the relevant Settings section so the user finishes the change.
    connect(
        diagnostics_page_, &DiagnosticsPage::applyFixActionRequested, this,
        [this](const QString& fix_id, const QString& changes_summary) {
            const QString body = changes_summary.isEmpty()
                                     ? QStringLiteral("Apply this fix to your recording settings?")
                                     : changes_summary;
            if (QMessageBox::question(this, QStringLiteral("Apply fix"), body) != QMessageBox::Yes)
                return;
            if (fix_id == QStringLiteral("fix.capture.monitor_instead")) {
                // rec.capture.exclusive_window: the selected window is in exclusive
                // fullscreen and cannot be captured. Retarget to the hosting monitor
                // (DXGI OD can capture FSE). The confirm above already stated the
                // scope + APP-audio consequences via changes_summary.
                if (record_page_)
                    record_page_->selectMonitorTargetForWindow();
                refreshDiagnosticsData();
                diagnostics::AppLog::info(QStringLiteral("diagnostics"), QStringLiteral("Applied fix %1").arg(fix_id));
                return;
            }
            if (fix_id == QStringLiteral("fix.frame_pacing.smooth")) {
                // Video-settings-only fix: switch pacing mode, propagate, refresh UI.
                video_settings_.frame_pacing = recorder_core::FramePacingMode::Smooth;
                if (config_page_)
                    config_page_->setVideoSettings(video_settings_);
                if (record_page_)
                    record_page_->setVideoSettings(video_settings_);
                onLiveConfigChanged();
                refreshDiagnosticsData();
                diagnostics::AppLog::info(QStringLiteral("diagnostics"), QStringLiteral("Applied fix %1").arg(fix_id));
                return;
            }
            if (fix_id == QStringLiteral("fix.codec.video.default")) {
                // rec.003: the configured video codec is unavailable. Fall back to the best
                // codec this GPU + container actually supports (not a blind H.264), keeping
                // H.264 only as the last-resort default.
                output_settings_.video_codec =
                    capability::BestAvailableVideoCodec(runtime_caps_, output_settings_.container)
                        .value_or(capability::VideoCodec::H264);
            } else if (fix_id == QStringLiteral("fix.profile.codec.best")) {
                // rec.profile.codec: switch to the recommended best GPU-supported codec
                // (shared resolver — identical pick to the recommendation), then reconcile
                // the audio codec for the resulting container.
                if (const auto best = capability::BestAvailableVideoCodec(runtime_caps_, output_settings_.container))
                    output_settings_.video_codec = *best;
                ReconcileContainerCodecs(output_settings_);
            } else if (fix_id == QStringLiteral("fix.codec.audio.default")) {
                output_settings_.audio_codec = capability::AudioCodec::Aac;
            } else if (fix_id == QStringLiteral("fix.audio.opus_to_aac")) {
                // rec.009 Notice (Opus-in-MP4): switch audio to AAC and reconcile.
                output_settings_.audio_codec = capability::AudioCodec::Aac;
                ReconcileContainerCodecs(output_settings_);
            } else if (fix_id == QStringLiteral("fix.color.range")) {
                // rec.color.range Notice: Full range is crushed/too dark in players that ignore
                // the range flag (e.g. VLC). Switch to Limited, the compatible-everywhere choice.
                output_settings_.color_range = capability::ColorRange::Limited;
            } else if (fix_id == QStringLiteral("fix.hdr.codec.av1") ||
                       fix_id == QStringLiteral("fix.hdr.codec.hevc")) {
                // rec.hdr.h264 Blocker: H.264 has no HDR10-native (10-bit/P010, PQ/BT.2020) path.
                // The engine picks whichever of AV1/HEVC is actually GPU-selectable (see
                // RecommendationEngine::checkHdrH264Blocker) and keys the fix id off that choice
                // so this handler applies the exact codec the FixAction proposed, never a blind AV1.
                output_settings_.video_codec = fix_id == QStringLiteral("fix.hdr.codec.av1")
                                                   ? capability::VideoCodec::Av1
                                                   : capability::VideoCodec::Hevc;
                ReconcileContainerCodecs(output_settings_);
            } else {
                return; // unknown auto fix — no-op
            }
            // Propagate like a user-driven format change.
            if (config_page_)
                config_page_->setOutputSettings(output_settings_);
            if (record_page_)
                record_page_->setOutputSettings(output_settings_);
            onLiveConfigChanged();
            refreshCrashSessionContext();
            refreshDiagnosticsData();
            diagnostics::AppLog::info(QStringLiteral("diagnostics"), QStringLiteral("Applied fix %1").arg(fix_id));
        });
    connect(diagnostics_page_, &DiagnosticsPage::openAssistedFixRequested, this, [this](const QString& fix_id) {
        navigateToPage(kSettingsPageIndex);
        if (!config_page_)
            return;
        if (fix_id == QStringLiteral("fix.output.change_folder") || fix_id == QStringLiteral("fix.output.fat32_folder"))
            config_page_->scrollToSection(QStringLiteral("settings/output"));
        else // fix.container.mkv / fix.fps.cap / fix.profile.select → format/quality area
            config_page_->scrollToSection(QStringLiteral("settings/format"));
        diagnostics::AppLog::info(QStringLiteral("diagnostics"), QStringLiteral("Opened assisted fix %1").arg(fix_id));
    });
    connect(diagnostics_page_, &DiagnosticsPage::createSupportBundleRequested, this, &MainWindow::createSupportBundle);
    connect(diagnostics_page_, &DiagnosticsPage::navigateToLogsRequested, this,
            [this]() { navigateToPage(kLogsPageIndex); });
    // Capability facts moved to the Device page; the Expert environment row links there.
    connect(diagnostics_page_, &DiagnosticsPage::openDevicePageRequested, this,
            [this]() { navigateToPage(kDevicePageIndex); });
    // SETTINGS-HONESTY-R1: Phase ④'s "Open last report" link routes to the REAL
    // post-flight report on the Edit overlay's Review step (EditExportPage) instead
    // of duplicating it in Diagnostics. Guarded the same way the result Edit button
    // is: only meaningful once a recording has completed.
    connect(diagnostics_page_, &DiagnosticsPage::openLastReportRequested, this, [this]() {
        if (!record_page_ || !record_page_->hasCompletedRecording())
            return;
        navigateToEditExportPage(record_page_->currentEditContext());
    });
    // Review F4: re-push the gate on every page show, so the link cannot stay stale
    // if last_succeeded settled after the most recent chrome-state event.
    connect(diagnostics_page_, &DiagnosticsPage::lastRecordingGateRefreshRequested, this, [this]() {
        if (record_page_ && diagnostics_page_)
            diagnostics_page_->setHasLastRecording(record_page_->hasCompletedRecording());
    });
    // Seed the initial gate state (record_page_ is always valid; see above).
    diagnostics_page_->setHasLastRecording(record_page_->hasCompletedRecording());
    // Route live recording-pipeline diagnostics from the Record page's coordinator to
    // the Diagnostics page (same UI thread; direct connection).
    // CRITICAL: record_page_ is built unconditionally in the ctor and is always valid here.
    connect(record_page_, &RecordPage::diagnosticsUpdated, diagnostics_page_, &DiagnosticsPage::applyLiveDiagnostics);
    // Fan-out replay: deliver current static diagnostic data to the freshly-built page.
    // refreshDiagnosticsData() self-guards on runtime_caps_ready_, so this is a no-op until
    // the caps probe completes — the caps-ready path will call refreshDiagnosticsData() again.
    refreshDiagnosticsData();
}

void MainWindow::buildAboutPage() {
    if (about_page_)
        return; // already built (e.g. by an early navigation)
    about_page_ = new pages::AboutPage(stack_);
    if (about_placeholder_) {
        // Replace the placeholder in-place so kAboutPageIndex stays valid; EditExportPage
        // lives past the kPageDescriptors slots and is unaffected.
        const int idx = stack_->indexOf(about_placeholder_);
        stack_->insertWidget(idx, about_page_);
        about_placeholder_->deleteLater();
        about_placeholder_ = nullptr;
    } else {
        stack_->addWidget(about_page_);
    }
    // Apply conditional initial state that would have run in the ctor:
    // non-default-theme refreshBrand, plus the persisted channel metadata row.
    if (persisted_settings_.theme_id != QStringLiteral("dark-default"))
        about_page_->refreshBrand();
    about_page_->setChannelHint(persisted_settings_.update_channel);
}

void MainWindow::buildDevicePage() {
    if (device_page_)
        return; // already built (e.g. by an early navigation)
    device_page_ = new DevicePage(stack_);
    if (device_placeholder_) {
        // Replace the placeholder in-place so kDevicePageIndex stays valid for all
        // widgets already past it in the stack (settings=2, hotkeys=3, ... about=8).
        const int idx = stack_->indexOf(device_placeholder_);
        stack_->insertWidget(idx, device_page_);
        device_placeholder_->deleteLater();
        device_placeholder_ = nullptr;
    } else {
        stack_->addWidget(device_page_);
    }
    // Static bit-depth/rate-control facts for the capability-matrix feature rows
    // (additive to the per-adapter probe; see DevicePage.h). Only meaningful once
    // the async runtime probe has completed — harmless no-op default otherwise.
    if (runtime_caps_ready_)
        device_page_->setCapabilitySet(runtime_caps_);
    // PERF: NO adapter scan here — buildDevicePage runs in the post-paint
    // hydration tick, which must stay cheap. DevicePage scans on its first
    // showEvent (first real navigation to the page), on a worker thread.
    connect(device_page_, &DevicePage::openSettingsRequested, this, [this]() { navigateToPage(kSettingsPageIndex); });
}

void MainWindow::buildEditExportOverlay() {
    if (edit_export_overlay_)
        return; // already built (e.g. by navigateToEditExportPage or visual harness)
    // EDIT-OVERLAY-R1: parented to the central widget — same recipe as
    // source_picker_overlay_ — so it covers the full client area (title bar
    // included) with a backdrop that correctly composites over the native DXGI
    // live-preview HWND. Never added to stack_. The overlay wires its own hosted
    // page's backRequested -> closeOverlay() internally (see EditExportOverlay ctor).
    edit_export_overlay_ = new ui::dialogs::EditExportOverlay(centralWidget());
    edit_export_overlay_->hide();
    // Leave the real title bar uncovered (ADR 0022): the window stays movable and
    // minimizable for the whole edit session, export included.
    if (title_bar_)
        edit_export_overlay_->setTopInset(title_bar_->height());
    // Mic privacy (review): the overlay covers the Record page without hiding it,
    // so RecordPage's hideEvent gating never fires — suspend/resume the
    // visibility-gated meter monitoring (mic-in-use indicator) explicitly.
    connect(edit_export_overlay_, &ui::dialogs::EditExportOverlay::opened, this,
            [this]() { record_page_->setEditOverlayActive(true); });
    connect(edit_export_overlay_, &ui::dialogs::EditExportOverlay::closed, this,
            [this]() { record_page_->setEditOverlayActive(false); });
}

void MainWindow::buildFinalizingOverlay() {
    if (finalizing_overlay_)
        return; // already built
    // Same DXGI-safe recipe as buildEditExportOverlay(): parented to the central
    // widget so it correctly composites over the native DXGI live-preview HWND —
    // never a child stacked directly inside RecordPage/PreviewSurface.
    finalizing_overlay_ = new ui::dialogs::FinalizingOverlay(centralWidget());
    finalizing_overlay_->hide();
}

} // namespace exosnap
