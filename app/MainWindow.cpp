#include "MainWindow.h"

#include "diagnostics/AppLog.h"
#include "models/RecordingPreset.h"
#include "pages/AdvancedPage.h"
#include "pages/ConfigPage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/HotkeysPage.h"
#include "pages/LogsPage.h"
#include "pages/RecordPage.h"
#include "pages/WebcamPage.h"
#include "services/GlobalHotkeyService.h"
#include "ui/WindowGeometryPolicy.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/dialogs/AboutOverlay.h"
#include "ui/dialogs/SourcePickerOverlay.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "ui/theme/ExoSnapPalette.h"
#include "ui/widgets/WebcamSetupPanel.h"
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
#include "visual_tests/VisualScenario.h"

#include <QToolButton>
#endif

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/user_config.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFile>
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
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <algorithm>
#include <array>
#include <optional>

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

enum class SidebarIcon {
    Record = 0,
    Video = 1,
    Audio = 2,
    Output = 3,
    Webcam = 4,
    Hotkeys = 5,
    Diagnostics = 6,
    Logs = 7,
    Advanced = 8,
    Setup = 9,
};

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

struct PageDescriptor {
    const char* nav_label;
    const char* subtitle;
    const char* page_meta;
    bool primary_nav;
    SidebarIcon icon;
};

constexpr std::array<PageDescriptor, 7> kPageDescriptors = {{
    {"Record", "Operational view — target, readiness, and live runtime.", "", true, SidebarIcon::Record},
    {"Settings", "Unified recording configuration — format, sources, and output.", "", true, SidebarIcon::Setup},
    {"Hotkeys", "Global command access for recording operations.", "GLOBAL SHORTCUTS", true, SidebarIcon::Hotkeys},
    {"Diagnostics", "Capability checks, blockers, and system readiness.", "BLOCKER-FIRST", true,
     SidebarIcon::Diagnostics},
    {"Logs", "Runtime events and recording diagnostics.", "SESSION EVENTS", true, SidebarIcon::Logs},
    {"Advanced", "Lower-level behavior and non-default controls.", "EXPERT SETTINGS", false, SidebarIcon::Advanced},
    {"Webcam", "Webcam device and capture settings.", "", false, SidebarIcon::Webcam},
}};

constexpr int pageIndexForIcon(SidebarIcon icon) {
    for (std::size_t i = 0; i < kPageDescriptors.size(); ++i) {
        if (kPageDescriptors[i].icon == icon)
            return static_cast<int>(i);
    }
    return -1;
}
constexpr int kRecordPageIndex = pageIndexForIcon(SidebarIcon::Record);
constexpr int kSettingsPageIndex = pageIndexForIcon(SidebarIcon::Setup);
constexpr int kHotkeysPageIndex = pageIndexForIcon(SidebarIcon::Hotkeys);
constexpr int kAdvancedPageIndex = pageIndexForIcon(SidebarIcon::Advanced);
constexpr int kDiagnosticsPageIndex = pageIndexForIcon(SidebarIcon::Diagnostics);
constexpr int kWebcamPageIndex = pageIndexForIcon(SidebarIcon::Webcam);
constexpr int kLogsPageIndex = pageIndexForIcon(SidebarIcon::Logs);
static_assert(kRecordPageIndex >= 0, "Record page must exist in kPageDescriptors.");
static_assert(kSettingsPageIndex >= 0, "Settings page must exist in kPageDescriptors.");
static_assert(kHotkeysPageIndex >= 0, "Hotkeys page must exist in kPageDescriptors.");
static_assert(kAdvancedPageIndex >= 0, "Advanced page must exist in kPageDescriptors.");
static_assert(kDiagnosticsPageIndex >= 0, "Diagnostics page must exist in kPageDescriptors.");
static_assert(kWebcamPageIndex >= 0, "Webcam page must exist in kPageDescriptors.");
static_assert(kLogsPageIndex >= 0, "Logs page must exist in kPageDescriptors.");

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

void applyDwmBorderSuppression(HWND hwnd, const char* reason) {
    if (hwnd == nullptr)
        return;

#if !defined(DWMWA_BORDER_COLOR)
#define DWMWA_BORDER_COLOR 34
#endif
#if !defined(DWMWA_COLOR_NONE)
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

    const COLORREF border_color = DWMWA_COLOR_NONE;
    const HRESULT hr =
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, static_cast<DWORD>(sizeof(border_color)));

    if (kTraceFrameActivation) {
        const QString line = QStringLiteral("%1 [FrameDbg] DwmSetWindowAttribute(DWMWA_BORDER_COLOR=NONE) reason=%2 "
                                            "hwnd=0x%3 hr=0x%4")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")))
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

void applyDwmBorderSuppression(HWND hwnd) {
    applyDwmBorderSuppression(hwnd, "unspecified");
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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    diagnostics::AppLog::init();
    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("MainWindow constructing"));

    setWindowTitle("ExoSnap");
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint | Qt::WindowSystemMenuHint);

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
    setMinimumSize(1120, 700);

    // ---- Load reduced AppSettingsStore (hotkeys + window geometry only) ----
    persisted_settings_ = settings_store_.Load();
    initHotkeyService();

    // ---- Load preset store ----
    PersistedPresetState loaded_presets = preset_store_.Load();
    if (loaded_presets.was_reset) {
        diagnostics::AppLog::warning(QStringLiteral("presets"),
                                     QStringLiteral("Preset store missing/incompatible — reset to defaults"));
        preset_store_.Save(loaded_presets.presets, loaded_presets.selected_id, loaded_presets.default_id);
    }
    preset_registry_.LoadState(loaded_presets.presets, loaded_presets.selected_id, loaded_presets.default_id);
    // Startup: boot to the DEFAULT preset.
    preset_registry_.SetSelected(preset_registry_.DefaultId());

    // Initialize live mirrors from the selected preset.
    const RecordingPresetConfig& startup_cfg = preset_registry_.SelectedSavedConfig();
    output_settings_ = startup_cfg.output;
    video_settings_ = startup_cfg.video;
    live_audio_ = startup_cfg.audio;
    live_webcam_ = startup_cfg.webcam;

    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("settings loaded"));

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
    config_page_ = new ConfigPage(output_settings_, video_settings_, stack_);
    hotkeys_page_ = new HotkeysPage(stack_);
    hotkeys_page_->setService(hotkey_service_);
    diagnostics_page_ = new DiagnosticsPage(stack_);
    webcam_page_ = new WebcamPage(stack_);
    webcam_page_->applySettings(live_webcam_);
    stack_->addWidget(record_page_);
    stack_->addWidget(config_page_);
    stack_->addWidget(hotkeys_page_);
    stack_->addWidget(diagnostics_page_);
    logs_page_ = new LogsPage(stack_);
    stack_->addWidget(logs_page_);
    advanced_page_ = new AdvancedPage(stack_);
    advanced_page_->setBaseline(output_settings_, video_settings_,
                                QString::fromStdString(preset_registry_.SelectedPreset().name));
    stack_->addWidget(advanced_page_);
    stack_->addWidget(webcam_page_);
    record_page_->setOutputSettings(output_settings_);
    record_page_->setVideoSettings(video_settings_);
    record_page_->setWebcamSettings(live_webcam_);
    record_page_->applyPersistedAudioSettings(live_audio_);
    record_page_->setCountdownSeconds(startup_cfg.countdown_seconds);
    record_page_->restoreRecordingHistory();
    config_page_->setAudioUiState(live_audio_);
    config_page_->setWebcamSettings(live_webcam_);
    main_layout->addWidget(stack_, 1);

    // In-window About surface — a translucent backdrop + centered card overlaid on the
    // whole window content area.  Parented to the central widget (not the page stack)
    // so the overlay subtree is accessible via UI Automation regardless of which
    // QStackedWidget page is current (DF-A11Y: QAccessibleStackedWidget only exposes
    // the current page child; parenting to central widget sidesteps this).  The overlay
    // covers the full central area including the title bar — per design the nav must
    // not be clickable while a modal overlay is open.  syncGeometryToParent() and the
    // resize event filter both use parentWidget(), so they work correctly with the new
    // parent.  raise() in openOverlay() / showEvent() ensures the overlay sits above
    // the title bar widget in paint order.
    about_overlay_ = new ui::dialogs::AboutOverlay(central);
    about_overlay_->hide();

    // Source picker overlay — in-window, same accessibility-first parenting as About.
    source_picker_overlay_ = new ui::dialogs::SourcePickerOverlay(central);
    source_picker_overlay_->hide();
    record_page_->setSourcePickerOverlay(source_picker_overlay_);

    title_bar_->setNavItems({
        {QStringLiteral("Record"), kRecordPageIndex},
        {QStringLiteral("Settings"), kSettingsPageIndex},
        {QStringLiteral("Hotkeys"), kHotkeysPageIndex},
        {QStringLiteral("Diagnostics"), kDiagnosticsPageIndex},
        {QStringLiteral("Logs"), kLogsPageIndex},
        {QStringLiteral("About"), -1},
    });

    connect(title_bar_, &ui::chrome::OperationalTitleBar::navPageRequested, this, &MainWindow::navigateToPage);
    connect(title_bar_, &ui::chrome::OperationalTitleBar::aboutRequested, this, [this]() {
        if (about_overlay_)
            about_overlay_->openOverlay();
    });
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
    connect(record_page_, &RecordPage::navigateToOutputPage, this, [this]() { navigateToPage(kSettingsPageIndex); });
    connect(record_page_, &RecordPage::navigateToDiagnosticsPage, this,
            [this]() { navigateToPage(kDiagnosticsPageIndex); });
    // ---- Format settings changed (from ConfigPage) ----
    connect(config_page_, &ConfigPage::formatSettingsChanged, this, [this](const OutputSettingsModel& settings) {
        if (applying_preset_)
            return;
        output_settings_.container = settings.container;
        output_settings_.video_codec = settings.video_codec;
        output_settings_.audio_codec = settings.audio_codec;
        output_settings_.output_folder = settings.output_folder;
        output_settings_.naming_pattern = settings.naming_pattern;
        output_settings_.resolution = settings.resolution;
        record_page_->setOutputSettings(output_settings_);
        if (advanced_page_) {
            advanced_page_->setBaseline(output_settings_, video_settings_,
                                        QString::fromStdString(preset_registry_.SelectedPreset().name));
        }
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
        refreshDiagnosticsData();
    });

    // ---- Preset selected (combo changed) ----
    connect(config_page_, &ConfigPage::presetSelected, this, [this](const QString& id) { onPresetSelected(id); });

    // ---- Video settings changed ----
    connect(config_page_, &ConfigPage::videoSettingsChanged, this, [this](const VideoSettingsModel& settings) {
        if (applying_preset_)
            return;
        video_settings_ = settings;
        record_page_->setVideoSettings(settings);
        if (advanced_page_) {
            advanced_page_->setBaseline(output_settings_, video_settings_,
                                        QString::fromStdString(preset_registry_.SelectedPreset().name));
        }
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
        refreshDiagnosticsData();
    });

    // ---- Audio settings changed (from ConfigPage) ----
    connect(config_page_, &ConfigPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        if (applying_preset_)
            return;
        live_audio_ = state;
        if (record_page_)
            record_page_->applyPersistedAudioSettings(state);
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
        refreshDiagnosticsData();
    });

    // ---- Audio settings changed (from RecordPage) ----
    connect(record_page_, &RecordPage::audioSettingsChanged, this, [this](const capability::AudioUiState& state) {
        if (applying_preset_)
            return;
        live_audio_ = state;
        if (config_page_)
            config_page_->setAudioUiState(state);
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
        refreshDiagnosticsData();
    });

    // ---- Recording config changed (target/region/countdown user action) ----
    connect(record_page_, &RecordPage::recordingConfigChanged, this, [this]() {
        if (applying_preset_)
            return;
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
    });

    // ---- Webcam settings changed (from WebcamPage) ----
    connect(webcam_page_, &WebcamPage::settingsChanged, this, [this](const WebcamSettings& settings) {
        if (applying_preset_)
            return;
        live_webcam_ = settings;
        record_page_->setWebcamSettings(settings);
        if (config_page_)
            config_page_->setWebcamSettings(settings);
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
    });

    // ---- Webcam settings changed (from ConfigPage embedded panel) ----
    connect(config_page_, &ConfigPage::webcamSettingsChanged, this, [this](const WebcamSettings& settings) {
        if (applying_preset_)
            return;
        live_webcam_ = settings;
        record_page_->setWebcamSettings(settings);
        if (webcam_page_)
            webcam_page_->applySettings(settings);
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
    });

    // ---- PiP placement confirmed in the Record preview ----
    connect(record_page_, &RecordPage::webcamSettingsChanged, this, [this](const WebcamSettings& settings) {
        if (applying_preset_)
            return;
        live_webcam_ = settings;
        if (config_page_)
            config_page_->setWebcamSettings(settings);
        if (webcam_page_)
            webcam_page_->applySettings(settings);
        const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
        if (config_page_)
            config_page_->setPresetDirty(dirty);
    });

    // ---- Preset management operations ----
    connect(config_page_, &ConfigPage::savePresetRequested, this, &MainWindow::onSavePreset);
    connect(config_page_, &ConfigPage::savePresetAsRequested, this, &MainWindow::onSavePresetAs);
    connect(config_page_, &ConfigPage::newPresetRequested, this, &MainWindow::onNewPreset);
    connect(config_page_, &ConfigPage::duplicatePresetRequested, this, &MainWindow::onDuplicatePreset);
    connect(config_page_, &ConfigPage::renamePresetRequested, this, &MainWindow::onRenamePreset);
    connect(config_page_, &ConfigPage::deletePresetRequested, this, &MainWindow::onDeletePreset);
    connect(config_page_, &ConfigPage::resetChangesRequested, this, &MainWindow::onResetChanges);
    connect(config_page_, &ConfigPage::resetToDefaultsRequested, this, &MainWindow::onResetToDefaults);
    connect(config_page_, &ConfigPage::setDefaultPresetRequested, this, &MainWindow::onSetDefaultPreset);

    // ---- Hotkeys ----
    connect(this, &MainWindow::recordToggleRequested, record_page_, &RecordPage::onHotkeyToggle);
    connect(this, &MainWindow::pauseToggleRequested, record_page_, &RecordPage::onHotkeyPauseToggle);
    connect(this, &MainWindow::captureFrameRequested, record_page_, &RecordPage::onHotkeyCaptureFrame);
    connect(this, &MainWindow::addMarkerRequested, record_page_, &RecordPage::onHotkeyAddMarker);
    connect(this, &MainWindow::splitRecordingRequested, record_page_, &RecordPage::onHotkeySplitRecording);
    connect(hotkey_service_, &GlobalHotkeyService::bindingChanged, this, &MainWindow::onHotkeyServiceBindingChanged);
    connect(record_page_, &RecordPage::audioMeterLevelsUpdated, config_page_, &ConfigPage::setAudioMeterLevels);

    // Re-apply the selected preset once the deferred coordinator init completes.
    // initCoordinator() resets audio rows and enumerates targets, clobbering the
    // preset applied in the constructor.  This connection restores the exact audio
    // rows and capture target from the preset after all init work is done.
    connect(record_page_, &RecordPage::coordinatorInitialized, this,
            [this]() { applyPresetConfig(preset_registry_.SelectedSavedConfig()); });

    connect(config_page_, &ConfigPage::diagnosticsRequested, this, [this]() {
        refreshDiagnosticsData();
        navigateToPage(kDiagnosticsPageIndex);
    });
    connect(diagnostics_page_, &DiagnosticsPage::navigateToLogsRequested, this,
            [this]() { navigateToPage(kLogsPageIndex); });
    // Route live recording-pipeline diagnostics from the Record page's coordinator to
    // the Diagnostics page (same UI thread; direct connection).
    connect(record_page_, &RecordPage::diagnosticsUpdated, diagnostics_page_, &DiagnosticsPage::applyLiveDiagnostics);
    connect(config_page_, &ConfigPage::webcamDetailsRequested, this, [this]() { navigateToPage(kWebcamPageIndex); });
    connect(config_page_, &ConfigPage::advancedRequested, this, [this]() { navigateToPage(kAdvancedPageIndex); });
    connect(webcam_page_, &WebcamPage::backToSettingsRequested, this, [this]() { navigateToPage(kSettingsPageIndex); });
    connect(advanced_page_, &AdvancedPage::backToSettingsRequested, this,
            [this]() { navigateToPage(kSettingsPageIndex); });

    // ---- Reactive device discovery wiring ----
    // Audio: forward to both ConfigPage and RecordPage (under no-emit contract).
    connect(&audio_notifier_, &AudioDeviceNotifier::snapshotChanged, this, &MainWindow::onAudioDevicesChanged);
    // Webcam: forward to ConfigPage (which forwards to WebcamSetupPanel), WebcamPage, and RecordPage.
    connect(&webcam_notifier_, &WebcamDeviceNotifier::snapshotChanged, this, &MainWindow::onWebcamDevicesChanged);
    // Display: replaces the old QGuiApplication::screenAdded/Removed lambdas.
    // DisplayDeviceNotifier covers add/remove AND geometry/DPI changes.
    connect(&display_notifier_, &DisplayDeviceNotifier::snapshotChanged, this, &MainWindow::onDisplaysChanged);

    // Route webcam Rescan through the canonical notifier path.
    if (webcam_page_)
        connect(webcam_page_, &WebcamPage::rescanRequested, &webcam_notifier_, &WebcamDeviceNotifier::rescan);
    if (config_page_) {
        // Route Settings audio Rescan through the audio notifier.
        connect(config_page_, &ConfigPage::audioRescanRequested, &audio_notifier_, &AudioDeviceNotifier::rescan);
        // Route Settings webcam panel Rescan through the webcam notifier.
        // The WebcamSetupPanel is embedded in ConfigPage; access via findChild.
        auto* setup_panel = config_page_->findChild<exosnap::ui::widgets::WebcamSetupPanel*>(
            QStringLiteral("settingsWebcamSetupPanel"));
        if (setup_panel)
            connect(setup_panel, &exosnap::ui::widgets::WebcamSetupPanel::rescanRequested, &webcam_notifier_,
                    &WebcamDeviceNotifier::rescan);
    }

    record_page_->rebroadcastChromeState();
    // Apply the startup preset config to all pages.
    applyPresetConfig(startup_cfg);

    diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("MainWindow constructed"));

    navigateToPage(kRecordPageIndex);

    auto* fullscreen_shortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreen_shortcut->setContext(Qt::ApplicationShortcut);
    connect(fullscreen_shortcut, &QShortcut::activated, this, &MainWindow::toggleFullScreen);

    // Application-level filter to catch mouse-press events in the resize border
    // regardless of which child widget lies under the cursor.  Resize zones are
    // HTCLIENT, so WM_NCLBUTTONDOWN never fires for them; we handle them here.
    qApp->installEventFilter(this);

    QTimer::singleShot(0, this, [this]() {
        runtime_caps_ = capability::CapabilityBuilder::BuildFromHardwareQuery();
        runtime_caps_ready_ = true;
        diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("capabilities probed"));
        if (record_page_)
            record_page_->setRuntimeCapabilities(runtime_caps_);
        refreshPresetUi();
        refreshDiagnosticsData();

        // Start the device notifiers after the capability probe so the first
        // snapshotChanged emission has the correct runtime context.
        // rescan() seeds the initial availability state synchronously so pages
        // know the device state without waiting for a native event.
        audio_notifier_.start();
        audio_notifier_.rescan();
        webcam_notifier_.start();
        webcam_notifier_.rescan();
        display_notifier_.start();
        display_notifier_.rescan();
        diagnostics::AppLog::info(QStringLiteral("window"), QStringLiteral("device notifiers started"));
    });
}

MainWindow::~MainWindow() {
    // Stop notifiers FIRST, before any pages are torn down, so no late callback
    // fires into a partially-destroyed page.  The notifiers also stop in their own
    // destructors, but explicit ordering here prevents a race with the Qt object
    // tree teardown.
    audio_notifier_.stop();
    webcam_notifier_.stop();
    display_notifier_.stop();
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
        applyDwmBorderSuppression(hwnd);
        resizable_style_applied_ = true;
    }
    if (!hotkeys_registered_) {
        hotkeys_registered_ = true;
#if defined(Q_OS_WIN)
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd && hotkey_service_) {
            win32_hotkey_registrar_ = std::make_unique<Win32HotkeyRegistrar>(hwnd);
            hotkey_service_->SetRegistrar(win32_hotkey_registrar_.get());
        }
#endif
    }
#endif

    if (!runtime_window_icon_bound_)
        applyRuntimeWindowIcon();
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
    recording_active_ = recording;
    record_status_label_ = status_label.trimmed().toUpper();
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
        if (webcam_page_)
            webcam_page_->setRecordingControlsLocked(locked);
    }

    applyTitleBarStatus();
    switchRecordingIcon(recording_active_, record_status_label_ == QStringLiteral("PAUSED"));

    // Lock hotkeys page editing while recording / countdown / stopping.
    if (hotkeys_page_) {
        const bool hk_locked =
            (record_status_label_ == QStringLiteral("REC") || record_status_label_ == QStringLiteral("PAUSED") ||
             record_status_label_ == QStringLiteral("STOPPING") || record_status_label_ == QStringLiteral("COUNTDOWN"));
        hotkeys_page_->setEditingLocked(hk_locked);
    }

    if ((recording || record_status_label_ == QStringLiteral("COUNTDOWN")) && isVisible() && !isMinimized() &&
        stack_->currentIndex() != 0)
        navigateToPage(kRecordPageIndex);
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

            if (msg->hwnd == main_hwnd &&
                (msg->message == WM_NCACTIVATE || msg->message == WM_ACTIVATE || msg->message == WM_SETFOCUS)) {
                const char* reason = "focus-transition";
                if (msg->message == WM_NCACTIVATE)
                    reason = "WM_NCACTIVATE";
                else if (msg->message == WM_ACTIVATE)
                    reason = "WM_ACTIVATE";
                else if (msg->message == WM_SETFOCUS)
                    reason = "WM_SETFOCUS";
                applyDwmBorderSuppression(msg->hwnd, reason);
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

                // Re-apply DWM border suppression after any size transition to prevent
                // the OS-drawn accent border from reappearing after restore/resize.
                applyDwmBorderSuppression(msg->hwnd, "WM_SIZE");
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
            }

            // Reset the drag/move override cursor when the window-move or resize
            // operation ends.  WM_CAPTURECHANGED fires too early (ReleaseCapture is
            // called inside startSystemMove before the loop starts), so WM_EXITSIZEMOVE
            // is the reliable signal that the modal loop has actually finished.
            if (msg->hwnd == main_hwnd && msg->message == WM_EXITSIZEMOVE) {
                if (title_bar_ != nullptr)
                    title_bar_->resetDragCursor();
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
    if (event->type() == QEvent::WindowStateChange) {
        win32_maximized_ = isMaximized();
        if (title_bar_ != nullptr)
            title_bar_->setMaximizedState(effectiveMaximizedState());
#if defined(Q_OS_WIN)
        const auto* state_event = static_cast<QWindowStateChangeEvent*>(event);
        const bool was_maximized = (state_event->oldState() & Qt::WindowMaximized) != 0;
        const bool restored_from_maximized = was_maximized && !isMaximized();

        // On restore from maximized, force the NC area to be recalculated and
        // re-apply the DWM border suppression so the accent frame cannot reappear.
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd != nullptr) {
            if (restored_from_maximized) {
                SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
            applyDwmBorderSuppression(hwnd, "changeEvent/WindowStateChange");
        }
#endif
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveWindowGeometry();

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
    if (geo.width <= 0 || geo.height <= 0)
        return;

    // The title bar region that must remain accessible so the user can move the window.
    const QRect title_strip(geo.x, geo.y, std::min(geo.width, 200), 40);

    QScreen* screen = nullptr;
    for (QScreen* s : QGuiApplication::screens()) {
        if (s->availableGeometry().intersects(title_strip)) {
            screen = s;
            break;
        }
    }

    // Saved position lands on no connected monitor: center on primary.
    const bool center_on_primary = screen == nullptr;
    if (center_on_primary)
        screen = QGuiApplication::primaryScreen();
    if (screen == nullptr)
        return;
    const QRect saved(geo.x, geo.y, geo.width, geo.height);
    setGeometry(ui::ClampRestoredWindowGeometry(saved, screen->availableGeometry(),
                                                QSize(minimumWidth(), minimumHeight()), center_on_primary));

    if (geo.maximized)
        QTimer::singleShot(0, this, &MainWindow::showMaximized);
}

int MainWindow::navHighlightIndexFor(int index) const {
    // Advanced and Webcam are sub-pages reached from Settings; keep the Settings tab lit
    // while they are shown (no dedicated top-nav tab exists for them).
    if (index == kAdvancedPageIndex || index == kWebcamPageIndex)
        return kSettingsPageIndex;
    return index;
}

void MainWindow::navigateToPage(int index) {
    if (index < 0 || index >= static_cast<int>(kPageDescriptors.size()))
        return;

    // Any page switch cleanly dismisses inline overlays so they never linger
    // over an unrelated page.
    if (about_overlay_)
        about_overlay_->closeOverlay();
    if (source_picker_overlay_)
        source_picker_overlay_->closeOverlay();

    setCurrentPage(index);
    if (title_bar_)
        title_bar_->setActivePage(navHighlightIndexFor(index));
}

void MainWindow::setCurrentPage(int index) {
    if (index < 0 || index >= static_cast<int>(kPageDescriptors.size()))
        return;

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

    // Push to pages (handlers early-return while applying_preset_).
    // Order matters: applyCapturePolicy can rebuild/reset audio rows via
    // ApplyTargetKindPreservingAudio; applyPersistedAudioSettings must come LAST
    // so the preset's exact audio rows win over any kind-default rows.
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
    if (advanced_page_) {
        advanced_page_->setBaseline(cfg2.output, cfg2.video,
                                    QString::fromStdString(preset_registry_.SelectedPreset().name));
    }
    if (webcam_page_) {
        webcam_page_->applySettings(cfg2.webcam);
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
    std::vector<ConfigPage::ProfileOption> options;
    options.reserve(preset_registry_.Count());

    for (const auto& preset : preset_registry_.Presets()) {
        ConfigPage::ProfileOption option;
        option.id = QString::fromStdString(preset.id);
        option.label = QString::fromStdString(preset.name);
        option.built_in = false;
        option.modified = false;
        option.available = true;
        options.push_back(std::move(option));
    }

    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    syncing_preset_ui_ = true;
    if (config_page_) {
        config_page_->setPresetOptions(options, QString::fromStdString(preset_registry_.SelectedId()),
                                       QString::fromStdString(preset_registry_.DefaultId()), dirty);
        config_page_->setActiveProfileName(QString::fromStdString(preset_registry_.SelectedPreset().name));
    }
    syncing_preset_ui_ = false;
}

void MainWindow::persistPresetState() {
    preset_store_.Save(preset_registry_.Presets(), preset_registry_.SelectedId(), preset_registry_.DefaultId());
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
    diagnostics_page_->setDiagnosticData(runtime_caps_, output_settings_, video_settings_, live_audio_, preset_name,
                                         hotkeys_summary, settings_store_.SettingsFilePath().toStdString(),
                                         hotkeys_registered_);
}

// ---------------------------------------------------------------------------
// Stage 2 — Preset operation handlers
// ---------------------------------------------------------------------------

void MainWindow::onPresetSelected(const QString& id) {
    if (syncing_preset_ui_)
        return;
    if (!record_page_ || !record_page_->canApplyPresetNow()) {
        // Reject switch during recording — revert the selector.
        refreshPresetUi();
        diagnostics::AppLog::warning(QStringLiteral("preset"),
                                     QStringLiteral("preset switch rejected: recording in progress"));
        return;
    }
    if (!preset_registry_.SetSelected(id.toStdString()))
        return;
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
    persistPresetState();
}

void MainWindow::onSavePreset() {
    preset_registry_.SaveSelected(captureLiveConfig());
    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    if (config_page_)
        config_page_->setPresetDirty(dirty);
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onSavePresetAs(const QString& name) {
    preset_registry_.AddPreset(captureLiveConfig(), name.toStdString());
    const bool dirty = preset_registry_.IsSelectedDirty(captureLiveConfig());
    if (config_page_)
        config_page_->setPresetDirty(dirty);
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onNewPreset() {
    preset_registry_.AddDefaultPreset();
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
    persistPresetState();
}

void MainWindow::onDuplicatePreset() {
    preset_registry_.DuplicateSelected();
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
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
        QMessageBox::warning(this, QStringLiteral("Delete Preset"), QStringLiteral("Cannot delete the only preset."));
        return;
    }
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
    refreshPresetUi();
    persistPresetState();
}

void MainWindow::onResetChanges() {
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
    refreshPresetUi();
    // No persistence needed (no registry mutation).
}

void MainWindow::onResetToDefaults() {
    preset_registry_.ResetAllToDefault();
    // Also reset hotkeys via service (handles Win32 re-registration + persistence signal).
    if (hotkey_service_)
        hotkey_service_->ResetAllToDefaults();
    applyPresetConfig(preset_registry_.SelectedSavedConfig());
    refreshPresetUi();
    persistPresetState();
    QMessageBox::information(this, QStringLiteral("Reset Complete"),
                             QStringLiteral("Settings and presets were reset to defaults."));
}

void MainWindow::onSetDefaultPreset() {
    preset_registry_.SetDefault(preset_registry_.SelectedId());
    refreshPresetUi();
    persistPresetState();
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace {

capability::AudioUiState VisualAudioStateForSettings(visual::VisualSettingsTarget target) {
    using K = recorder_core::AudioSourceKind;
    capability::AudioUiState state;
    state.target_kind = target == visual::VisualSettingsTarget::Window ? capability::CaptureTargetKind::Window
                                                                       : capability::CaptureTargetKind::Display;
    state.selected_window_pid =
        target == visual::VisualSettingsTarget::Window ? std::optional<uint32_t>{4242} : std::nullopt;
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
        {K::Sys, true, false},
    };
    state.selected_mic_device_id = std::string("visual-test-mic");
    return state;
}

ui::dialogs::SourcePickerPanel::SourceOption VisualScreenOption() {
    ui::dialogs::SourcePickerPanel::SourceOption option;
    option.target_index = 0;
    option.native_id = 1001;
    option.title = QStringLiteral("Visual Test Display 1");
    option.detail = QStringLiteral("2560 × 1440 · 60 Hz · primary");
    option.primary = true;
    option.status_badge = QStringLiteral("TEST");
    option.monitor_width = 2560;
    option.monitor_height = 1440;
    return option;
}

ui::dialogs::SourcePickerPanel::SourceOption VisualWindowOption() {
    ui::dialogs::SourcePickerPanel::SourceOption option;
    option.target_index = 1;
    option.native_id = 2001;
    option.title = QStringLiteral("Visual Test Window");
    option.detail = QStringLiteral("ExoSnap Fixture · PID 4242");
    option.status_badge = QStringLiteral("TEST");
    return option;
}

} // namespace

void MainWindow::applyVisualScenario(const visual::VisualScenario& scenario) {
    if (about_overlay_)
        about_overlay_->closeOverlay();
    if (source_picker_overlay_)
        source_picker_overlay_->closeOverlay();

    if (record_page_)
        record_page_->applyVisualScenario(scenario);

    switch (scenario.page) {
    case visual::VisualPage::Record:
        setCurrentPage(kRecordPageIndex);
        break;
    case visual::VisualPage::Settings:
        applyVisualSettingsScenario(scenario);
        break;
    case visual::VisualPage::Webcam:
        if (webcam_page_) {
            webcam_page_->applyVisualState(scenario.webcam_state);
            if (!scenario.webcam_chroma_color_mode.isEmpty()) {
                const auto colorModeFromStr = [](const QString& s) -> WebcamChromaKeyColorMode {
                    if (s == QStringLiteral("blue"))
                        return WebcamChromaKeyColorMode::Blue;
                    if (s == QStringLiteral("magenta"))
                        return WebcamChromaKeyColorMode::Magenta;
                    if (s == QStringLiteral("custom"))
                        return WebcamChromaKeyColorMode::Custom;
                    return WebcamChromaKeyColorMode::Green;
                };
                WebcamSettings ws;
                ws.enabled = scenario.webcam_state == visual::VisualWebcamState::Active;
                ws.chroma_key.enabled = scenario.webcam_chroma_enabled;
                ws.chroma_key.color_mode = colorModeFromStr(scenario.webcam_chroma_color_mode);
                webcam_page_->applySettings(ws);
            }
            webcam_page_->setRecordingControlsLocked(scenario.controls_locked);
        }
        setCurrentPage(kWebcamPageIndex);
        break;
    case visual::VisualPage::Hotkeys:
        setCurrentPage(kHotkeysPageIndex);
        applyVisualHotkeysScenario(scenario);
        break;
    case visual::VisualPage::Diagnostics:
        applyVisualDiagnosticsScenario(scenario);
        setCurrentPage(kDiagnosticsPageIndex);
        break;
    case visual::VisualPage::Logs:
        if (logs_page_)
            logs_page_->applyVisualScenario(scenario);
        setCurrentPage(kLogsPageIndex);
        break;
    case visual::VisualPage::About:
        setCurrentPage(kRecordPageIndex);
        if (about_overlay_)
            about_overlay_->openOverlay();
        break;
    }

    if (scenario.source_picker_tab != visual::VisualSourcePickerTab::None)
        applyVisualSourcePickerScenario(scenario);

    // Device-discovery state is applied last so it can override audio/webcam
    // state set by the page-specific helpers above.
    applyVisualDeviceDiscoveryScenario(scenario);

    if (title_bar_ && stack_)
        title_bar_->setActivePage(navHighlightIndexFor(stack_->currentIndex()));

    // Deterministic keyboard focus (VR-004): give the named widget tab focus so
    // :focus styling is visible in screenshots.
    if (!scenario.focused_object.isEmpty()) {
        if (auto* target = findChild<QWidget*>(scenario.focused_object); target && target->isEnabled())
            target->setFocus(Qt::TabFocusReason);
    }

    installVisualReadyMarker(scenario.id);
    QTimer::singleShot(0, this, [this, scenario_id = scenario.id]() { installVisualReadyMarker(scenario_id); });
    setWindowTitle(QStringLiteral("ExoSnap [visual-test:%1]").arg(scenario.id));
}

void MainWindow::installVisualReadyMarker(const QString& scenario_id) {
    auto* host = centralWidget();
    if (host == nullptr)
        return;

    auto* marker = findChild<QLabel*>(QStringLiteral("visualTestReadyMarker"));
    if (marker == nullptr) {
        marker = new QLabel(host);
        marker->setObjectName(QStringLiteral("visualTestReadyMarker"));
        marker->setAttribute(Qt::WA_TransparentForMouseEvents);
        marker->setStyleSheet(QStringLiteral("QLabel#visualTestReadyMarker {"
                                             "background: rgba(31, 196, 140, 0.18);"
                                             "border: 1px solid rgba(31, 196, 140, 0.55);"
                                             "border-radius: 4px;"
                                             "color: #b9f6df;"
                                             "font: 11px 'JetBrains Mono';"
                                             "padding: 3px 6px;"
                                             "}"));
    }
    marker->setText(QStringLiteral("VISUAL_TEST_READY:%1").arg(scenario_id));
    marker->adjustSize();
    marker->move(host->width() - marker->width() - 12, host->height() - marker->height() - 12);
    marker->raise();
    marker->show();
}

void MainWindow::applyVisualSettingsScenario(const visual::VisualScenario& scenario) {
    setCurrentPage(kSettingsPageIndex);
    if (!config_page_)
        return;

    OutputSettingsModel output;
    output.container = capability::Container::WebM;
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    output.audio_codec = capability::AudioCodec::Opus;
    output.output_folder = std::filesystem::path(L"C:\\Users\\User\\Videos\\ExoSnap");
    output.naming_pattern = L"visual-test_{datetime}_{title}";
    output.container = scenario.container;
    output.video_codec = scenario.video_codec;
    output.audio_codec = scenario.audio_codec;
    output.resolution.mode = scenario.output_resolution_mode;
    if (scenario.output_resolution_mode == OutputResolutionMode::Custom) {
        output.resolution.custom_width = static_cast<uint32_t>((std::max)(0, scenario.requested_width));
        output.resolution.custom_height = static_cast<uint32_t>((std::max)(0, scenario.requested_height));
    }

    VideoSettingsModel video;
    video.frame_rate_num = scenario.frame_rate_num;
    video.frame_rate_den = scenario.frame_rate_den;
    video.cfr = scenario.cfr;
    config_page_->setOutputSettings(output);
    config_page_->setVideoSettings(video);
    config_page_->setAudioUiState(VisualAudioStateForSettings(scenario.settings_target));
    config_page_->setReadinessStatus(QStringLiteral("READY"));
    config_page_->setRecordingControlsLocked(scenario.controls_locked);
    config_page_->setAudioMeterLevels(0.37f, 0.56f, 0.42f, true, true, true);

    // Webcam-card scenarios (mirror off/on, unavailable) drive the embedded panel
    // deterministically without opening a real camera.
    if (scenario.webcam_state != visual::VisualWebcamState::None) {
        config_page_->applyVisualWebcamState(scenario.webcam_state == visual::VisualWebcamState::Active,
                                             scenario.webcam_mirror);
    }

    // Preset card — inject synthetic ProfileOption data when preset_count > 0 or
    // the scenario id starts with "settings-preset".  Never touches
    // RecordingPresetStore or RecordingPresetRegistry.
    //
    // The injection is deferred by one event-loop turn (singleShot 20 ms) so it
    // fires AFTER the MainWindow constructor's singleShot(0) that calls
    // refreshPresetUi() on capabilities-probe completion.  Without this the
    // real preset registry would overwrite the synthetic data before the harness
    // manifest is written at t=120 ms.
    const bool drive_preset = scenario.preset_count > 0 || scenario.id.startsWith(QStringLiteral("settings-preset"));
    if (drive_preset) {
        // Capture scenario fields by value for the deferred lambda.
        const int count = scenario.preset_count > 0 ? scenario.preset_count : 3;
        const QString selected_name = scenario.preset_selected_name;
        const QString default_name = scenario.preset_default_name;
        const bool dirty = scenario.preset_dirty;
        const bool save_error = scenario.preset_save_error;
        const bool menu_open = scenario.preset_menu_open;

        QTimer::singleShot(20, this, [this, count, selected_name, default_name, dirty, save_error, menu_open]() {
            if (!config_page_)
                return;

            // Synthetic preset names in declaration order.
            const QStringList kPresetNames = {QStringLiteral("Default"),   QStringLiteral("Gaming"),
                                              QStringLiteral("Tutorial"),  QStringLiteral("Streaming"),
                                              QStringLiteral("Cinematic"), QStringLiteral("Podcast"),
                                              QStringLiteral("Archive")};

            std::vector<ConfigPage::ProfileOption> opts;
            opts.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                ConfigPage::ProfileOption opt;
                opt.id = QStringLiteral("preset.vis%1").arg(i + 1);
                opt.label = (i < kPresetNames.size()) ? kPresetNames[i] : QStringLiteral("Preset %1").arg(i + 1);
                opt.built_in = (i == 0); // first entry is the built-in default
                opt.available = true;
                opts.push_back(opt);
            }

            // Match selected_id and default_id to the scenario's named fields.
            QString selected_id = opts.front().id;
            QString default_id = opts.front().id;
            for (const auto& opt : opts) {
                if (opt.label == selected_name)
                    selected_id = opt.id;
                if (opt.label == default_name)
                    default_id = opt.id;
            }

            config_page_->setPresetOptions(opts, selected_id, default_id, dirty);

            // Inline save-error affordance (no modal — entirely deterministic).
            config_page_->applyVisualPresetSaveError(save_error);

            // Open the Manage overflow menu after a short delay so the widget is
            // visible and the screenshot captures it.  The menu is non-blocking in
            // Qt's event loop (QMenu::exec would block, showMenu() does not).
            if (menu_open) {
                if (auto* btn = config_page_->findChild<QToolButton*>(QStringLiteral("presetManageButton")))
                    QTimer::singleShot(80, btn, [btn]() { btn->showMenu(); });
            }
        });
    }
}

void MainWindow::applyVisualSourcePickerScenario(const visual::VisualScenario& scenario) {
    setCurrentPage(kRecordPageIndex);
    if (!source_picker_overlay_)
        return;

    source_picker_overlay_->setScreenOptions({VisualScreenOption()});
    source_picker_overlay_->setWindowOptions({VisualWindowOption()});
    const QRect visual_region(scenario.region_x, scenario.region_y, scenario.region_width, scenario.region_height);
    bool has_region = true;
    bool select_on_record = false;
    QString region_summary =
        QStringLiteral("VISUAL TEST: %1 × %2 on Display 1").arg(visual_region.width()).arg(visual_region.height());
    if (scenario.region_state == visual::VisualRegionState::Empty ||
        scenario.region_state == visual::VisualRegionState::None) {
        has_region = false;
        select_on_record = true;
        region_summary = QStringLiteral("VISUAL TEST: no saved region");
    } else if (scenario.region_state == visual::VisualRegionState::Editing) {
        region_summary = QStringLiteral("VISUAL TEST EDITING: %1, %2 — %3 × %4")
                             .arg(visual_region.x())
                             .arg(visual_region.y())
                             .arg(visual_region.width())
                             .arg(visual_region.height());
    } else if (scenario.region_state == visual::VisualRegionState::Invalid) {
        has_region = false;
        select_on_record = true;
        region_summary = QStringLiteral("VISUAL TEST INVALID: below 64 × 64 minimum");
    }
    source_picker_overlay_->setRegionState(region_summary, has_region, select_on_record, visual_region);

    ui::dialogs::SourcePickerPanel::Section section = ui::dialogs::SourcePickerPanel::Section::Screens;
    int target_index = 0;
    if (scenario.source_picker_tab == visual::VisualSourcePickerTab::Windows) {
        section = ui::dialogs::SourcePickerPanel::Section::Windows;
        target_index = 1;
    } else if (scenario.source_picker_tab == visual::VisualSourcePickerTab::Region) {
        section = ui::dialogs::SourcePickerPanel::Section::Region;
        target_index = 0;
    }

    source_picker_overlay_->setCurrentSection(section, target_index);
    if (scenario.region_state == visual::VisualRegionState::Preset16x9) {
        source_picker_overlay_->applyVisualRegionPreset(1920, 1080);
    } else if (scenario.region_state == visual::VisualRegionState::Preset9x16) {
        source_picker_overlay_->applyVisualRegionPreset(1080, 1920);
    }
    source_picker_overlay_->openOverlay();
}

namespace {

// Deterministic synthetic live-pipeline snapshots for the Visual Harness. They are fed
// through the SAME presentation path as production (DiagnosticsPage::applyLiveDiagnostics).
recorder_core::RecordingDiagnosticsSnapshot makeLiveDiagnosticsSnapshot(const QString& kind) {
    using namespace recorder_core;
    RecordingDiagnosticsSnapshot s;
    s.session_generation = 1;

    if (kind == QStringLiteral("idle")) {
        s.lifecycle = DiagnosticsLifecycle::Idle;
        s.valid = false;
        s.health = PipelineHealth::Idle;
        return s;
    }

    s.lifecycle = DiagnosticsLifecycle::Recording;
    s.valid = true;
    s.elapsed_seconds = 42.0;

    s.capture.target_fps = 60.0;
    s.capture.actual_fps = 59.8;
    s.capture.frames_captured = 2600;
    s.capture.frames_emitted = 2520;
    s.capture.frames_dropped_coalesced = 80;
    s.capture.frames_duplicated = 3;
    s.capture.frame_interval_ms = 1000.0 / 60.0;
    s.capture.interval_observed = MetricAvailability::Unavailable;
    s.capture.source_type = CaptureSourceType::Display;

    s.compositor.active = true;
    s.compositor.latest_ms = 1.3;
    s.compositor.average_ms = 1.4;
    s.compositor.peak_ms = 2.8;
    s.compositor.frames_composed = 2520;

    s.video_encoder.latest_ms = 2.0;
    s.video_encoder.average_ms = 2.1;
    s.video_encoder.peak_ms = 3.5;
    s.video_encoder.output_fps = 60.0;
    s.video_encoder.frames_submitted = 2520;
    s.video_encoder.frames_encoded = 2520;
    s.video_encoder.codec = VideoCodec::Av1Nvenc;
    s.video_encoder.width = 1920;
    s.video_encoder.height = 1080;
    s.video_encoder.cfr = true;

    s.audio.active = true;
    s.audio.packets_encoded = 2000;
    s.audio.bytes_encoded = 256000;
    s.audio.queue_depth = 1;
    s.audio.queue_peak = 3;
    s.audio.sample_rate = 48000;
    s.audio.channels = 2;
    s.audio.codec = AudioCodec::Opus;
    s.audio.track_count = 1;

    s.video_queue.current_depth = 1;
    s.video_queue.peak_depth = 3;
    s.video_queue.capacity = 0;
    s.video_queue.bounded = false;
    s.audio_queue.current_depth = 0;
    s.audio_queue.peak_depth = 12;
    s.audio_queue.capacity = 600;
    s.audio_queue.bounded = true;

    s.mux.packets_processed = 4520;
    s.mux.bytes_written = 18ull * 1024ull * 1024ull;
    s.mux.throughput_mib_s = 18.7;
    s.mux.latest_write_ms = 0.8;
    s.mux.average_write_ms = 0.8;
    s.mux.peak_write_ms = 4.2;
    s.mux.current_segment_index = 0;
    s.mux.segment_count = 1;
    s.mux.reorder_packets = 1;
    s.mux.reorder_packets_peak = 3;
    s.mux.reorder_bytes = 2048;
    s.mux.reorder_bytes_peak = 6144;
    s.mux.availability = MetricAvailability::Available;

    s.disk.bytes_written = s.mux.bytes_written;
    s.disk.throughput_mib_s = 18.7;
    s.disk.latest_write_ms = 0.8;
    s.disk.average_write_ms = 0.8;
    s.disk.peak_write_ms = 4.2;
    s.disk.output_target = "C:";
    s.disk.latency_availability = MetricAvailability::Available;

    s.split.split_supported = true;
    s.split.current_segment = 1;
    s.split.completed_segments = 0;
    s.split.availability = MetricAvailability::Available;
    s.split.seconds_until_auto_split = -1.0;

    s.bottleneck = PipelineBottleneck::None;
    s.health = PipelineHealth::Good;

    if (kind == QStringLiteral("encoder")) {
        s.video_encoder.average_ms = 9.2;
        s.video_encoder.peak_ms = 14.0;
        s.video_encoder.backlog = 6;
        s.video_encoder.frames_encoded = 2440;
        s.video_queue.current_depth = 5;
        s.bottleneck = PipelineBottleneck::VideoEncoder;
        s.bottleneck_reason = "Encoder backlog rising";
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("disk")) {
        s.mux.average_write_ms = 14.0;
        s.mux.peak_write_ms = 22.0;
        s.mux.throughput_mib_s = 4.0;
        s.disk.average_write_ms = 14.0;
        s.disk.peak_write_ms = 22.0;
        s.disk.throughput_mib_s = 4.0;
        s.video_queue.current_depth = 12;
        s.bottleneck = PipelineBottleneck::Disk;
        s.bottleneck_reason = "Write latency high";
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("paused")) {
        s.lifecycle = DiagnosticsLifecycle::Paused;
        s.capture.actual_fps = 0.0;
        s.video_encoder.output_fps = 0.0;
        s.mux.throughput_mib_s = 0.0;
        s.disk.throughput_mib_s = 0.0;
    } else if (kind == QStringLiteral("split")) {
        s.split.current_segment = 2;
        s.split.completed_segments = 1;
        s.split.split_pending = true;
        s.split.last_trigger = DiagnosticsSplitTrigger::ManualButton;
        s.mux.current_segment_index = 1;
        s.mux.segment_count = 2;
        s.mux.split_transitions = 1;
        s.video_encoder.forced_keyframes = 1;
    }

    return s;
}

} // namespace

void MainWindow::applyVisualDiagnosticsScenario(const visual::VisualScenario& scenario) {
    if (!diagnostics_page_)
        return;

    capability::CapabilitySet caps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    OutputSettingsModel output;
    output.container = capability::Container::WebM;
    output.video_codec = capability::VideoCodec::Av1Nvenc;
    output.audio_codec = capability::AudioCodec::Opus;
    VideoSettingsModel video;
    diagnostics_page_->setDiagnosticData(
        caps, output, video, VisualAudioStateForSettings(visual::VisualSettingsTarget::Window),
        "Visual Test WebM AV1 Opus", "Start/Stop: Alt+F9", "visual-test-settings.json", true);

    if (!scenario.diag_live.isEmpty()) {
        diagnostics_page_->applyLiveDiagnostics(makeLiveDiagnosticsSnapshot(scenario.diag_live));
    }
}

void MainWindow::applyVisualHotkeysScenario(const visual::VisualScenario& scenario) {
    if (!hotkeys_page_)
        return;

    // Apply custom bindings (non-persistent: bypass service to avoid Win32 registration).
    if (!scenario.hk_custom_binding_0.isEmpty() || !scenario.hk_custom_binding_1.isEmpty()) {
        const QKeySequence b0 =
            scenario.hk_custom_binding_0.isEmpty()
                ? GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording)
                : QKeySequence::fromString(scenario.hk_custom_binding_0, QKeySequence::PortableText);
        const QKeySequence b1 =
            scenario.hk_custom_binding_1.isEmpty()
                ? GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause)
                : QKeySequence::fromString(scenario.hk_custom_binding_1, QKeySequence::PortableText);
        hotkeys_page_->setBindings({b0, b1});
    }

    hotkeys_page_->setEditingLocked(scenario.hk_editing_locked);

    // Simulate conflict message on a specific row (visual-only, no Win32 state).
    if (scenario.hk_conflict_shown && scenario.hk_conflict_action >= 0) {
        const QString msg = scenario.hk_conflict_message.isEmpty() ? QStringLiteral("This shortcut is already in use.")
                                                                   : scenario.hk_conflict_message;
        // Use public slots to drive the error display.
        // We call a test-only helper on HotkeysPage via objectName lookup.
        auto* row =
            hotkeys_page_->findChild<QLabel*>(QStringLiteral("hotkeyError_%1").arg(scenario.hk_conflict_action));
        if (row) {
            row->setText(msg);
            row->show();
        }
    }
}

void MainWindow::applyVisualDeviceDiscoveryScenario(const visual::VisualScenario& scenario) {
    // Only engage when at least one device-discovery field is set.
    const bool has_audio = scenario.dd_audio_input_count >= 0;
    const bool has_webcam = scenario.dd_webcam_count >= 0;

    // --- Audio mic list (Settings/Audio card) ---
    // Build a synthetic AudioDeviceSnapshot matching the scenario state and
    // push it to ConfigPage via the existing reactive handler.  This is the
    // same path used during live hot-plug, so no new UI hooks are needed.
    // Non-persistent: onAudioDevicesChanged() never writes to any store.
    if (has_audio && config_page_) {
        AudioDeviceSnapshot snap;
        // Populate synthetic input devices.
        const int n_inputs = scenario.dd_audio_input_count;
        for (int i = 0; i < n_inputs; ++i) {
            recorder_core::AudioInputDeviceInfo d;
            d.device_id = QStringLiteral("vis-input-%1").arg(i + 1).toStdString();
            d.display_name = QStringLiteral("Visual Test Input %1").arg(i + 1).toStdString();
            d.is_default = (i == 0);
            snap.inputs.push_back(d);
        }
        if (!snap.inputs.empty())
            snap.default_input_id = snap.inputs.front().device_id;

        // If the scenario has a selected mic that should be present, make sure
        // that id appears in the snapshot.  If it should be absent, leave it out.
        if (!scenario.dd_selected_mic_stable_id.isEmpty()) {
            if (scenario.dd_selected_mic_available) {
                // Replace the first synthetic device with the configured id so
                // onAudioDevicesChanged() finds it and keeps it selected.
                const std::string target_id = scenario.dd_selected_mic_stable_id.toStdString();
                bool already_present = false;
                for (auto& d : snap.inputs) {
                    if (d.device_id == target_id) {
                        already_present = true;
                        break;
                    }
                }
                if (!already_present) {
                    recorder_core::AudioInputDeviceInfo target_dev;
                    target_dev.device_id = target_id;
                    target_dev.display_name =
                        QStringLiteral("Visual Test Mic (%1)").arg(scenario.dd_selected_mic_stable_id).toStdString();
                    target_dev.is_default = false;
                    snap.inputs.push_back(target_dev);
                }
                // Pre-select by setting audio_ui_state before the reactive push.
                capability::AudioUiState state;
                state.selected_mic_device_id = target_id;
                config_page_->setAudioUiState(state);
            } else {
                // Device is absent: configure the id first, then push a snapshot
                // without it so the placeholder is shown.
                capability::AudioUiState state;
                state.selected_mic_device_id = scenario.dd_selected_mic_stable_id.toStdString();
                config_page_->setAudioUiState(state);
                // snap intentionally does NOT contain dd_selected_mic_stable_id.
            }
        }

        config_page_->onAudioDevicesChanged(snap);
    }

    // --- Webcam card (Settings/Webcam) ---
    // For webcam-missing scenarios the webcam_state is already set to
    // Unavailable by the page-dispatch block; applyVisualWebcamState() is the
    // existing harness hook — just make sure it is driven correctly here for
    // discovery-specific scenarios that set dd_webcam_count.
    if (has_webcam && config_page_) {
        const bool cam_available = scenario.dd_selected_webcam_available;
        const bool mirror = scenario.webcam_mirror;
        // Only override if the scenario is on the Settings page (the webcam
        // card lives there); the Record preview uses a separate path.
        if (scenario.page == visual::VisualPage::Settings)
            config_page_->applyVisualWebcamState(cam_available, mirror);
    }
}
#endif

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
    // Forward to all three consumers.  None of these should emit webcamSettingsChanged
    // so live_webcam_ and the preset dirty state remain unchanged.
    if (config_page_)
        config_page_->onWebcamDevicesChanged(snap);
    if (webcam_page_)
        webcam_page_->onWebcamDevicesChanged(snap);
    if (record_page_)
        record_page_->onWebcamDevicesChanged(snap);
}

void MainWindow::onDisplaysChanged(const exosnap::DisplaySnapshot& snap, exosnap::DiscoveryReason /*reason*/) {
    // The display handler does not emit recordingConfigChanged for availability
    // changes (only for actual user-initiated target switches), so preset stays clean.
    if (record_page_)
        record_page_->onDisplaysChanged(snap);
}

} // namespace exosnap
