#include "QuickWindowChrome.h"

#include "QuickWindowGeometry.h"

#include "models/WindowPresencePolicy.h"

#include <QCoreApplication>
#include <QDebug>
#include <QIcon>
#include <QQuickWindow>
#include <QWindow>

#include <cmath>

#if defined(Q_OS_WIN)
#include <windows.h>
// windowsx.h supplies GET_X_LPARAM/GET_Y_LPARAM; dwmapi.h the border attribute.
#include <dwmapi.h>
#include <windowsx.h>

#include "exosnap_resource.h"

// The Quick module's CMake target does not link Dwmapi. Declaring the dependency
// here keeps this slice to new files only; if the target ever gains an explicit
// `dwmapi` entry this pragma becomes a harmless duplicate.
#pragma comment(lib, "dwmapi.lib")
#endif

namespace exosnap::quick {
namespace {

#if defined(Q_OS_WIN)

// dwmapi.h only declares DWMWA_BORDER_COLOR as a DWMWINDOWATTRIBUTE enumerator in
// the Windows 11 SDK. DwmSetWindowAttribute takes the attribute as a plain DWORD,
// so pinning the documented value keeps older SDKs building; on Windows releases
// before build 22000 the call just returns a failure HRESULT and the window keeps
// its default frame.
constexpr DWORD kDwmwaBorderColor = 34;

// Explorer's announcement that a window's taskbar button exists. Registered once
// per process -- RegisterWindowMessage returns the same value for the same string
// for the lifetime of the session, and 0 when the atom table refuses, which is
// the one value no real message can have.
UINT taskbarButtonCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarButtonCreated");
    return message;
}

// The scale factor to convert this window's physical pixels into the logical
// units QML reports its geometry in.
//
// Deliberately NOT QWindow::devicePixelRatio(): that is a Qt-side cache updated
// through the screen-changed signal path, while GetClientRect and the
// WM_NCHITTEST coordinates it is compared against are live OS state. Windows
// resizes a per-monitor-v2 window synchronously while handling WM_DPICHANGED, so
// between that resize and Qt processing its own notification the two disagree —
// and a hit test in that gap divides the new physical extents by the old ratio.
// GetDpiForWindow is the same call generation as GetClientRect, which removes the
// event-queue timing from this path entirely.
double windowScaleFactor(HWND hwnd, const QQuickWindow* fallback) {
    if (hwnd != nullptr) {
        const UINT dpi = GetDpiForWindow(hwnd);
        if (dpi > 0)
            return static_cast<double>(dpi) / static_cast<double>(USER_DEFAULT_SCREEN_DPI);
    }
    // Pre-1607 Windows and a failed query both land here.
    const double qt_ratio = fallback != nullptr ? fallback->devicePixelRatio() : 1.0;
    return qt_ratio > 0.0 ? qt_ratio : 1.0;
}

// One line per window-state transition, under the same switch as the startup
// geometry trace. The startup message trace deliberately stops at the first
// frame, which is exactly when this becomes interesting: a maximize that Qt and
// Windows disagree about is invisible in every other instrument, and the restore
// rect Windows will return to is not observable from Qt at all.
void traceWindowState(const char* reason, HWND hwnd, const QQuickWindow* window) {
    if (!WindowGeometryTraceEnabled() || hwnd == nullptr)
        return;

    RECT rect{};
    GetWindowRect(hwnd, &rect);

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    const bool have_placement = GetWindowPlacement(hwnd, &placement) != FALSE;
    const RECT& normal = placement.rcNormalPosition;

    // The style is part of the line because WS_MAXIMIZE (0x01000000) is the bit
    // Windows itself keys the maximized state on, and placement_flags because
    // WPF_RESTORETOMAXIMIZED (0x0002) is the one that decides where a minimized
    // window comes back to. A state that Qt believes in
    // without that bit being set is not a maximized window, however large it is.
    qInfo("window-state: %s zoomed=%d iconic=%d qt_visibility=%d window=%ld,%ld %ldx%ld restore=%ld,%ld %ldx%ld "
          "show_cmd=%u placement_flags=0x%04x style=0x%08llx dpr=%.2f",
          reason, IsZoomed(hwnd) != FALSE ? 1 : 0, IsIconic(hwnd) != FALSE ? 1 : 0,
          window != nullptr ? static_cast<int>(window->visibility()) : -1, rect.left, rect.top, rect.right - rect.left,
          rect.bottom - rect.top, have_placement ? normal.left : 0L, have_placement ? normal.top : 0L,
          have_placement ? normal.right - normal.left : 0L, have_placement ? normal.bottom - normal.top : 0L,
          have_placement ? placement.showCmd : 0u, have_placement ? placement.flags : 0u,
          static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
          window != nullptr ? window->devicePixelRatio() : 0.0);
}

const char* sysCommandName(WPARAM wparam) {
    switch (wparam & 0xFFF0u) {
    case SC_MAXIMIZE:
        return "SC_MAXIMIZE";
    case SC_RESTORE:
        return "SC_RESTORE";
    case SC_MINIMIZE:
        return "SC_MINIMIZE";
    case SC_SIZE:
        return "SC_SIZE";
    case SC_MOVE:
        return "SC_MOVE";
    default:
        return "SC_other";
    }
}

#endif // Q_OS_WIN

} // namespace

QuickWindowChrome::QuickWindowChrome(QObject* parent) : QObject(parent) {
}

QuickWindowChrome::~QuickWindowChrome() {
    // Removing the filter from the destructor is the whole reason this class does
    // not delegate to a separate filter object: there is exactly one lifetime to
    // reason about, and it ends here.
    detach();
}

// ---------------------------------------------------------------------------
// Attachment
// ---------------------------------------------------------------------------

void QuickWindowChrome::attach(QQuickWindow* window) {
    setTarget(window);
}

void QuickWindowChrome::setTarget(QQuickWindow* window) {
    if (target_ == window && (window == nullptr || hwnd_ != nullptr))
        return;

    detach();
    if (window == nullptr) {
        emit targetChanged();
        return;
    }

    target_ = window;
    TraceWindowGeometry("chrome-attach", window);
    // QML attaches from Component.onCompleted, which can run before the window is
    // exposed. create() forces the platform window into existence so winId() can
    // never hand back a null handle that would silently disable every handler.
    window->create();
    hwnd_ = reinterpret_cast<void*>(window->winId());
    affinity_.setHandle(hwnd_);
    emit nativeHandleChanged();
    TraceWindowGeometry("chrome-hwnd-created", window);

    // A screen change is the moment the DWM border wants re-applying, and the
    // handle is re-read on the way. It is NOT a recreate notification: the Windows
    // QPA plugin handles screen and DPI transitions on the existing HWND. What
    // keeps the cached handle valid is that nothing destroys the root window while
    // the process lives -- an accepted close posts a quit, and no top-level window
    // refuses one. Break that and this connection will not catch it.
    QObject::connect(window, &QWindow::screenChanged, this, [this]() { refreshHandle(); });
    QObject::connect(window, &QObject::destroyed, this, [this]() { detach(); });

    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->installNativeEventFilter(this);

    ensureNativeFrameStyle();
    refreshWindowMaximized();
    applyBorderColor("attach");
    TraceWindowGeometry("chrome-style-applied", window);
    emit targetChanged();
}

void QuickWindowChrome::detach() {
    // detach() is the second writer of target_, reachable from QML, from the
    // destructor and from the target's own destroyed() signal — so it owes the
    // property the same notification setTarget() gives it, or a consumer binding
    // to `target` never learns the window went away.
    const bool had_target = !target_.isNull();

    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeNativeEventFilter(this);
    if (!target_.isNull()) {
        // Broad form on purpose: QML property bindings that read this object's
        // properties are owned by binding objects, never by `this`, so nothing a
        // QML author writes can be caught by this disconnect.
        QObject::disconnect(target_, nullptr, this, nullptr);
    }
    const bool had_handle = hwnd_ != nullptr;
    target_ = nullptr;
    hwnd_ = nullptr;
    affinity_.setHandle(nullptr);
    if (had_handle)
        emit nativeHandleChanged();
    non_client_leave_tracked_ = false;
    applied_border_valid_ = false;
    applied_border_hwnd_ = nullptr;
    // With no handle left there is nothing to be maximized, and the property has
    // no other writer once the message stream is gone.
    refreshWindowMaximized();
    setMaximizeButtonHovered(false);
    setMaximizeButtonPressed(false);
    if (had_target)
        emit targetChanged();
}

void QuickWindowChrome::refreshHandle() {
    if (target_.isNull())
        return;
    target_->create();
    void* fresh = reinterpret_cast<void*>(target_->winId());
    if (fresh == hwnd_) {
        applyBorderColor("refresh");
        return;
    }
    hwnd_ = fresh;
    non_client_leave_tracked_ = false;
    ensureNativeFrameStyle();
    // Display affinity is per-HWND and a recreated window comes back without it.
    // This is the one place a real identity change is observed, so it is also the
    // place any future per-HWND shell integration re-asserts itself from.
    affinity_.setHandle(fresh);
    // A recreated window's taskbar button is a new button with none of the
    // previous one's registrations, and Explorer announces it separately.
    emit nativeHandleChanged();
    applyBorderColor("handle-recreated");
}

void QuickWindowChrome::applyNativeWindowStyle() {
    if (target_.isNull())
        return;
    // The handle first: applying the window flags is exactly the kind of change
    // that can make Qt recreate the platform window, and a stale HWND here would
    // style a window that no longer exists.
    refreshHandle();
    ensureNativeFrameStyle();
    TraceWindowGeometry("chrome-native-style", target_.data());
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

QQuickWindow* QuickWindowChrome::target() const noexcept {
    return target_.data();
}

int QuickWindowChrome::titleBarHeight() const noexcept {
    return title_bar_height_;
}

void QuickWindowChrome::setTitleBarHeight(int height) {
    const int clamped = height > 0 ? height : 0;
    if (clamped == title_bar_height_)
        return;
    title_bar_height_ = clamped;
    emit titleBarHeightChanged();
}

int QuickWindowChrome::resizeBorderThickness() const noexcept {
    return resize_border_thickness_;
}

void QuickWindowChrome::setResizeBorderThickness(int thickness) {
    const int clamped = thickness > 0 ? thickness : 0;
    if (clamped == resize_border_thickness_)
        return;
    resize_border_thickness_ = clamped;
    emit resizeBorderThicknessChanged();
}

const QList<QRectF>& QuickWindowChrome::interactiveRects() const noexcept {
    return interactive_rects_;
}

void QuickWindowChrome::setInteractiveRects(const QList<QRectF>& rects) {
    if (rects == interactive_rects_)
        return;
    interactive_rects_ = rects;
    emit interactiveRectsChanged();
}

void QuickWindowChrome::clearInteractiveRects() {
    if (interactive_rects_.isEmpty())
        return;
    interactive_rects_.clear();
    emit interactiveRectsChanged();
}

void QuickWindowChrome::addInteractiveRect(const QRectF& rect) {
    if (rect.isEmpty())
        return;
    interactive_rects_.append(rect);
    emit interactiveRectsChanged();
}

QRectF QuickWindowChrome::maximizeButtonRect() const noexcept {
    return maximize_button_rect_;
}

void QuickWindowChrome::setMaximizeButtonRect(const QRectF& rect) {
    if (rect == maximize_button_rect_)
        return;
    maximize_button_rect_ = rect;
    // A rect that moved out from under the cursor can never deliver the
    // WM_NCMOUSELEAVE that would otherwise clear the hover.
    setMaximizeButtonHovered(false);
    setMaximizeButtonPressed(false);
    emit maximizeButtonRectChanged();
}

bool QuickWindowChrome::maximizeButtonHovered() const noexcept {
    return maximize_button_hovered_;
}

bool QuickWindowChrome::maximizeButtonPressed() const noexcept {
    return maximize_button_pressed_;
}

void QuickWindowChrome::setMaximizeButtonHovered(bool hovered) {
    if (hovered == maximize_button_hovered_)
        return;
    maximize_button_hovered_ = hovered;
    emit maximizeButtonHoveredChanged();
}

void QuickWindowChrome::setMaximizeButtonPressed(bool pressed) {
    if (pressed == maximize_button_pressed_)
        return;
    maximize_button_pressed_ = pressed;
    emit maximizeButtonPressedChanged();
}

QColor QuickWindowChrome::borderColor() const noexcept {
    return border_color_;
}

void QuickWindowChrome::setBorderColor(const QColor& color) {
    if (color == border_color_)
        return;
    border_color_ = color;
    // DWMWA_BORDER_COLOR carries no alpha, so QML is expected to hand over an
    // already-composited opaque colour (QuickThemeTokens.line over .background).
    applyBorderColor("borderColor");
    emit borderColorChanged();
}

bool QuickWindowChrome::snapLayoutsEnabled() const noexcept {
    return snap_layouts_enabled_;
}

void QuickWindowChrome::setSnapLayoutsEnabled(bool enabled) {
    if (enabled == snap_layouts_enabled_)
        return;
    snap_layouts_enabled_ = enabled;
    if (!enabled) {
        setMaximizeButtonHovered(false);
        setMaximizeButtonPressed(false);
    }
    emit snapLayoutsEnabledChanged();
}

bool QuickWindowChrome::nonClientActivationWorkaround() const noexcept {
    return non_client_activation_workaround_;
}

void QuickWindowChrome::setNonClientActivationWorkaround(bool enabled) {
    if (enabled == non_client_activation_workaround_)
        return;
    non_client_activation_workaround_ = enabled;
    emit nonClientActivationWorkaroundChanged();
}

// ---------------------------------------------------------------------------
// Window icon
// ---------------------------------------------------------------------------

void QuickWindowChrome::setWindowMaximized(bool maximized) {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return;
    const bool zoomed = IsZoomed(hwnd) != FALSE;
    if (zoomed == maximized)
        return;
    // ShowWindow rather than QWindow::setVisibility/showMaximized: Qt answers the
    // latter on a frameless window with a bare SetWindowPos onto the work area,
    // which leaves Windows in SW_SHOWNORMAL and overwrites the restore rect with
    // the maximized one. SW_MAXIMIZE also goes through WM_GETMINMAXINFO and
    // WM_NCCALCSIZE below, so the taskbar clamp still applies.
    ShowWindow(hwnd, maximized ? SW_MAXIMIZE : SW_RESTORE);
    refreshWindowMaximized();
#else
    Q_UNUSED(maximized);
#endif
}

void QuickWindowChrome::minimizeWindow() {
    // Same question the SC_MINIMIZE branch of the filter asks, so the title bar's
    // button cannot resolve differently from Win+Down or the window menu.
    if (requestMinimizeToTray())
        return;
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return;
    ShowWindow(hwnd, SW_MINIMIZE);
#endif
}

void QuickWindowChrome::setMinimizeToTrayProvider(std::function<bool()> provider) {
    minimize_to_tray_provider_ = std::move(provider);
}

bool QuickWindowChrome::handleSysCommand(quint64 wparam) {
    return IsMinimizeSysCommand(wparam) && requestMinimizeToTray();
}

bool QuickWindowChrome::requestMinimizeToTray() {
    if (!minimize_to_tray_provider_ || !minimize_to_tray_provider_())
        return false;
    emit minimizeToTrayRequested();
    return true;
}

void QuickWindowChrome::setNativeCommandHandler(std::function<bool(quint64)> handler) {
    native_command_handler_ = std::move(handler);
}

bool QuickWindowChrome::handleNativeCommand(quint64 wparam) {
    return native_command_handler_ && native_command_handler_(wparam);
}

void* QuickWindowChrome::nativeHandle() const noexcept {
    return hwnd_;
}

void QuickWindowChrome::setCaptureExcluded(bool excluded) {
    affinity_.setExcludedFromCapture(excluded);
}

bool QuickWindowChrome::captureExcluded() const noexcept {
    return affinity_.excludedFromCapture();
}

bool QuickWindowChrome::captureExclusionApplied() const noexcept {
    return affinity_.applied();
}

void QuickWindowChrome::setAffinityFunctionForTest(MainWindowAffinity::AffinityFunction fn) {
    affinity_.setAffinityFunctionForTest(std::move(fn));
}

void QuickWindowChrome::restoreWindow() {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr || IsIconic(hwnd) == FALSE)
        return;
    ShowWindow(hwnd, SW_RESTORE);
    refreshWindowMaximized();
#endif
}

bool QuickWindowChrome::willOccupyScreenMaximized() const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return false;
    if (IsIconic(hwnd) == FALSE)
        return IsZoomed(hwnd) != FALSE;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) == FALSE || placement.showCmd != SW_SHOWMINIMIZED)
        return false;
    return (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
#else
    return false;
#endif
}

void QuickWindowChrome::toggleMaximized() {
    setWindowMaximized(!windowMaximized());
}

bool QuickWindowChrome::windowMaximized() const noexcept {
    return window_maximized_;
}

void QuickWindowChrome::refreshWindowMaximized() {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    const bool maximized = hwnd != nullptr && IsZoomed(hwnd) != FALSE;
#else
    const bool maximized = false;
#endif
    if (maximized == window_maximized_)
        return;
    window_maximized_ = maximized;
    emit windowMaximizedChanged();
}

void QuickWindowChrome::applyWindowIcon() {
#if defined(Q_OS_WIN)
    // The application icon, and only ever the application icon. A window icon
    // that changed with the session put the recording state on the taskbar
    // BUTTON, which is where the button's own overlay badge belongs -- and a
    // WM_SETICON is a full taskbar redraw, so a state that pulses would have
    // redrawn it several times a second for the length of a recording.
    const QIcon icon(QStringLiteral(":/brand/exosnap-app.ico"));
    if (icon.isNull()) {
        qWarning().noquote() << "QuickWindowChrome: icon load failed from :/brand/exosnap-app.ico";
    } else if (!target_.isNull()) {
        // Qt's Windows platform plugin turns setIcon into WM_SETICON for both
        // ICON_SMALL and ICON_BIG, so this alone already updates frame + taskbar.
        target_->setIcon(icon);
    }

    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr)
        return;

    // Belt-and-braces path carried over from the Widgets shell: the EXE's own icon
    // resource, which is the multi-resolution one Explorer shows. It contributes
    // only if exosnap.rc is compiled into this target; when it is not, LoadImageW
    // fails and the Qt path above is the whole story. LR_SHARED is safe because
    // the OS caches per (instance, id, size) tuple.
    HICON small_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                     LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_EXOSNAP_APP_ICON), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                                   LR_DEFAULTCOLOR | LR_SHARED));
    if (small_icon != nullptr)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (big_icon != nullptr)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
#endif
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------

void QuickWindowChrome::ensureNativeFrameStyle() const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return;

    // Windows decides every native window gesture from the style bits, not from
    // what the title band draws or from the HTCAPTION this class returns:
    //
    //   WS_THICKFRAME   native resize drag, Aero Snap to the screen edges, Win+Arrow
    //   WS_MAXIMIZEBOX  double-click-to-maximize, drag-to-top-to-maximize, Win+Up,
    //                   and the Windows 11 Snap Layouts flyout over HTMAXBUTTON
    //   WS_MINIMIZEBOX  Win+Down and the taskbar's minimize/restore
    //   WS_SYSMENU      the window menu on Alt+Space and on right-click
    //
    // A frameless Qt window carries none of them, so all four are re-added and
    // committed with SWP_FRAMECHANGED — without that flag Windows keeps using the
    // cached frame metrics and the style change has no observable effect.
    //
    // WS_CAPTION stays out: it is what would make Windows reserve non-client area
    // again and draw a second title bar above the product's own band.
    constexpr LONG_PTR kRequired = WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    // A live top-level window always carries at least WS_CLIPSIBLINGS, so a zero
    // style is the API's failure return rather than a real value. Writing
    // `0 | kRequired` back would strip WS_VISIBLE and every clipping bit off the
    // window; refusing the write leaves the gestures dead but the window intact,
    // and the log line is what turns that into something findable.
    if (style == 0) {
        qWarning("QuickWindowChrome: GWL_STYLE read failed, leaving the window style untouched");
        return;
    }
    if ((style & kRequired) == kRequired)
        return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | kRequired);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void QuickWindowChrome::applyBorderColor(const char* reason) const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr || !border_color_.isValid())
        return;

    const COLORREF colorref = RGB(border_color_.red(), border_color_.green(), border_color_.blue());
    // WM_SIZE arrives continuously through an interactive resize and a single
    // focus change delivers WM_NCACTIVATE, WM_ACTIVATE and WM_SETFOCUS, so this
    // runs far more often than the colour changes. DwmSetWindowAttribute is a
    // call into dwm.exe; repeating it with a value already applied buys nothing.
    // Keyed by the HWND as well, because a recreated window has not been told.
    if (applied_border_valid_ && applied_border_color_ == static_cast<quint32>(colorref) &&
        applied_border_hwnd_ == hwnd_)
        return;
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &colorref, static_cast<DWORD>(sizeof(colorref)));
    if (FAILED(hr)) {
        // One line, once: pre-Windows-11 systems fail this every single activation
        // and a per-message warning would drown the log.
        static bool warned_once = false;
        if (!warned_once) {
            warned_once = true;
            qWarning().nospace() << "QuickWindowChrome: DwmSetWindowAttribute(BORDER_COLOR) failed, reason="
                                 << (reason != nullptr ? reason : "null") << " hr=0x"
                                 << QString::number(static_cast<quint32>(hr), 16);
        }
        return;
    }
    applied_border_color_ = static_cast<quint32>(colorref);
    applied_border_valid_ = true;
    applied_border_hwnd_ = hwnd_;
#else
    Q_UNUSED(reason);
#endif
}

qintptr QuickWindowChrome::resolveHitTest(qintptr lparam) const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr || target_.isNull())
        return HTCLIENT;

    // WM_NCHITTEST carries SCREEN coordinates in PHYSICAL pixels, and they are
    // signed: a monitor left of or above the primary produces negative values.
    // GET_X_LPARAM does the short cast; a plain LOWORD would wrap -1 to 65535.
    const auto native_lparam = static_cast<LPARAM>(lparam);
    POINT point{GET_X_LPARAM(native_lparam), GET_Y_LPARAM(native_lparam)};
    if (ScreenToClient(hwnd, &point) == FALSE)
        return HTCLIENT;

    RECT client{};
    if (GetClientRect(hwnd, &client) == FALSE)
        return HTCLIENT;

    // The one conversion point between the two coordinate spaces. GetClientRect is
    // used rather than QWindow::width()/height() because Qt's cached logical size
    // lags by a frame during a live resize drag, which is exactly when the border
    // zones matter most — and the scale factor is taken from the same live source
    // for the same reason.
    const double dpr = windowScaleFactor(hwnd, target_.data());
    const double x = static_cast<double>(point.x) / dpr;
    const double y = static_cast<double>(point.y) / dpr;
    const double width = static_cast<double>(client.right - client.left) / dpr;
    const double height = static_cast<double>(client.bottom - client.top) / dpr;

    // 1. Resize borders win over everything, and corners over edges — otherwise the
    //    top-left corner would resolve to the title bar's HTCAPTION and the
    //    diagonal grip would be unreachable.
    //    IsZoomed rather than a tracked maximized flag: the flag lags the
    //    drag-to-restore gesture, the same reason WM_NCCALCSIZE consults IsZoomed.
    if (IsZoomed(hwnd) == FALSE) {
        const auto grip = static_cast<double>(resize_border_thickness_);
        const bool left = x < grip;
        const bool right = x >= width - grip;
        const bool top = y < grip;
        const bool bottom = y >= height - grip;

        if (top && left)
            return HTTOPLEFT;
        if (top && right)
            return HTTOPRIGHT;
        if (bottom && left)
            return HTBOTTOMLEFT;
        if (bottom && right)
            return HTBOTTOMRIGHT;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
        if (top)
            return HTTOP;
        if (bottom)
            return HTBOTTOM;
    }

    if (y < static_cast<double>(title_bar_height_)) {
        // 2. The maximize button claims HTMAXBUTTON so Windows 11 offers the Snap
        //    Layouts flyout on hover. See handleSnapLayoutMessage's note: this also
        //    costs us every client mouse event in that rect.
        if (snap_layouts_enabled_ && !maximize_button_rect_.isEmpty() && maximize_button_rect_.contains(x, y))
            return HTMAXBUTTON;

        // 3. Explicit exclusion list, replacing OperationalTitleBar::isInDragArea's
        //    "walk up the widget tree and reject anything that is a QAbstractButton".
        //    There is no widget tree to walk here, so the interactive geometry is
        //    pushed down from QML instead — nav tabs, the bell, the window buttons.
        for (const QRectF& rect : interactive_rects_) {
            if (rect.contains(x, y))
                return HTCLIENT;
        }

        // 4. Everything else in the band is the drag handle. HTCAPTION is what buys
        //    the native move loop, double-click-to-maximize, the window menu on
        //    right-click and Aero Shake — none of which we have to implement.
        return HTCAPTION;
    }

    return HTCLIENT;
#else
    Q_UNUSED(lparam);
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Native event filter
// ---------------------------------------------------------------------------

bool QuickWindowChrome::nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) {
#if defined(Q_OS_WIN)
    if (hwnd_ == nullptr || message == nullptr)
        return false;
    if (event_type != QByteArrayLiteral("windows_generic_MSG") &&
        event_type != QByteArrayLiteral("windows_dispatcher_MSG")) {
        return false;
    }

    auto* msg = static_cast<MSG*>(message);
    HWND hwnd = static_cast<HWND>(hwnd_);
    // Strict identity filter: a native event filter is process-wide, so without
    // this every tooltip, popup and drag proxy window would run through the
    // handlers below.
    if (msg->hwnd != hwnd)
        return false;

    const auto store = [result](LRESULT value) {
        if (result != nullptr)
            *result = static_cast<qintptr>(value);
    };

    // Not a switch case: the value is only known at runtime, and 0 (a refused
    // registration) must never match a real message.
    const UINT taskbar_created = taskbarButtonCreatedMessage();
    if (taskbar_created != 0 && msg->message == taskbar_created) {
        emit taskbarButtonCreated();
        // Deliberately not consumed. The message is a broadcast notification and
        // other components in the process may want it too.
        return false;
    }

    switch (msg->message) {
    case WM_COMMAND:
        // THBN_CLICKED from the thumbnail toolbar arrives here. The handler
        // decides whether it was one of ours; anything else falls through, which
        // is what keeps this filter out of the way of the rest of the process.
        if (handleNativeCommand(static_cast<quint64>(msg->wParam))) {
            store(0);
            return true;
        }
        return false;

    case WM_NCHITTEST: {
        store(static_cast<LRESULT>(resolveHitTest(static_cast<qintptr>(msg->lParam))));
        return true;
    }

    case WM_NCCALCSIZE: {
        // wParam FALSE carries a plain RECT and no client-area request; leave it to
        // DefWindowProc rather than guessing at its semantics.
        if (msg->wParam != static_cast<WPARAM>(TRUE))
            return false;
        auto* calc = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
        if (calc != nullptr && IsZoomed(hwnd) != FALSE) {
            // A maximized borderless window is sized to the MONITOR rect by
            // default, which puts its bottom edge underneath the taskbar. Clamping
            // to rcWork is what keeps the taskbar visible; this is not optional.
            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            const HMONITOR monitor = MonitorFromRect(&calc->rgrc[0], MONITOR_DEFAULTTONEAREST);
            if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE)
                calc->rgrc[0] = monitor_info.rcWork;
        }
        // Returning 0 with the requested rect untouched (apart from the clamp) is
        // what makes the client area cover the whole window — i.e. the borderless
        // frame. Everything else in this class exists to compensate for it.
        store(0);
        return true;
    }

    case WM_GETMINMAXINFO: {
        auto* minmax = reinterpret_cast<MINMAXINFO*>(msg->lParam);
        if (minmax != nullptr && !target_.isNull()) {
            // Same live source as the hit test: this message also arrives while a
            // DPI transition is in flight, and a minimum track size scaled by the
            // previous monitor's ratio is a minimum the user cannot resize past.
            const double dpr = windowScaleFactor(hwnd, target_.data());
            // Windows honours a minimum size during the native resize drag ONLY
            // through this message — QWindow::minimumWidth alone is ignored once
            // WM_NCCALCSIZE made the frame ours.
            minmax->ptMinTrackSize.x = static_cast<LONG>(std::lround(target_->minimumWidth() * dpr));
            minmax->ptMinTrackSize.y = static_cast<LONG>(std::lround(target_->minimumHeight() * dpr));

            MONITORINFO monitor_info{};
            monitor_info.cbSize = sizeof(monitor_info);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor != nullptr && GetMonitorInfoW(monitor, &monitor_info) != FALSE) {
                const RECT& monitor_rect = monitor_info.rcMonitor;
                const RECT& work_rect = monitor_info.rcWork;
                minmax->ptMaxPosition.x = work_rect.left - monitor_rect.left;
                minmax->ptMaxPosition.y = work_rect.top - monitor_rect.top;
                minmax->ptMaxSize.x = work_rect.right - work_rect.left;
                minmax->ptMaxSize.y = work_rect.bottom - work_rect.top;
                minmax->ptMaxTrackSize = minmax->ptMaxSize;
            }
        }
        store(0);
        return true;
    }

    case WM_NCACTIVATE: {
        applyBorderColor("WM_NCACTIVATE");
        if (!non_client_activation_workaround_)
            return false;
        // UNVERIFIED FOR QQuickWindow. In the Widgets shell, passing -1 as lParam
        // let Windows update the activation state without repainting the default
        // non-client visuals (a grey caption ghost flashing on focus loss). With
        // WM_NCCALCSIZE having removed the non-client area entirely there may be
        // nothing left to repaint, in which case this branch can be dropped —
        // toggle `nonClientActivationWorkaround` at runtime and watch a focus
        // in/out cycle before deciding.
        store(DefWindowProcW(hwnd, msg->message, msg->wParam, -1));
        return true;
    }

    case WM_ACTIVATE:
    case WM_SETFOCUS:
        applyBorderColor("focus-transition");
        return false;

    case WM_SIZE:
        // Only the border refresh survives the port: the maximized state is read in
        // QML from Window.visibility, so nothing here has to track it.
        applyBorderColor("WM_SIZE");
        refreshWindowMaximized();
        traceWindowState(msg->wParam == SIZE_MAXIMIZED   ? "WM_SIZE/maximized"
                         : msg->wParam == SIZE_RESTORED  ? "WM_SIZE/restored"
                         : msg->wParam == SIZE_MINIMIZED ? "WM_SIZE/minimized"
                                                         : "WM_SIZE/other",
                         hwnd, target_.data());
        return false;

    case WM_SYSCOMMAND:
        // Observed for every command -- SC_MAXIMIZE/SC_RESTORE are how every
        // native gesture (double-click, Win+arrow, the window menu) asks for a
        // state change, so their presence or absence separates "Windows was never
        // told" from "Windows was told and declined".
        traceWindowState(sysCommandName(msg->wParam), hwnd, target_.data());
        // SC_MINIMIZE is the one command this filter also ANSWERS. Every native
        // minimize route arrives here -- the window menu, Win+Down, and a click on
        // the taskbar button of the active window -- so taking it over is what
        // makes them share the title bar button's policy instead of quietly
        // minimizing past it.
        //
        // Win+D and "show desktop" are deliberately NOT covered: they are a
        // shell-wide desktop toggle whose windows come back with the same gesture,
        // and a window hidden to the tray would not.
        if (handleSysCommand(static_cast<quint64>(msg->wParam))) {
            store(0);
            return true;
        }
        return false;

    case WM_ENTERSIZEMOVE:
        traceWindowState("enter-size-move", hwnd, target_.data());
        return false;

    case WM_EXITSIZEMOVE:
        traceWindowState("exit-size-move", hwnd, target_.data());
        return false;

    case WM_WINDOWPOSCHANGED:
        // WM_SIZE alone is not enough: a restore that changes no size (a maximized
        // window snapped to the same extents) never sends one.
        refreshWindowMaximized();
        traceWindowState("window-pos-changed", hwnd, target_.data());
        return false;

    // -----------------------------------------------------------------------
    // Snap Layouts support — NOT PROVEN IN THIS REPO, MEASURE BEFORE TRUSTING.
    //
    // Returning HTMAXBUTTON is what makes Windows 11 show the Snap Layouts
    // flyout, but it also reclassifies that rectangle as non-client: QML stops
    // receiving ANY mouse event there, so its HoverHandler and TapHandler go
    // dead. Hover and click therefore have to be reconstructed from the NC
    // message stream and pushed back into QML. Kept in one contiguous block so
    // it can be excised wholesale (set snapLayoutsEnabled false) if measurement
    // shows the trade is not worth it.
    // -----------------------------------------------------------------------
    case WM_NCMOUSEMOVE: {
        const bool over = snap_layouts_enabled_ && msg->wParam == static_cast<WPARAM>(HTMAXBUTTON);
        if (over && !non_client_leave_tracked_) {
            // Without TME_NONCLIENT there is no WM_NCMOUSELEAVE and the hover
            // highlight sticks forever once the cursor leaves the button.
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE | TME_NONCLIENT;
            track.hwndTrack = hwnd;
            track.dwHoverTime = HOVER_DEFAULT;
            non_client_leave_tracked_ = (TrackMouseEvent(&track) != FALSE);
        }
        setMaximizeButtonHovered(over);
        // Deliberately not consumed: DefWindowProc still owns caption feedback and
        // the DWM-side flyout timing.
        return false;
    }

    case WM_NCMOUSELEAVE:
        non_client_leave_tracked_ = false;
        setMaximizeButtonHovered(false);
        setMaximizeButtonPressed(false);
        return false;

    case WM_MOUSEMOVE:
        // Safety net: a cursor that jumps from the button straight into the client
        // area (fast move, or a programmatic warp) can outrun WM_NCMOUSELEAVE.
        if (maximize_button_hovered_ || maximize_button_pressed_) {
            setMaximizeButtonHovered(false);
            setMaximizeButtonPressed(false);
        }
        return false;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK: {
        if (!snap_layouts_enabled_ || msg->wParam != static_cast<WPARAM>(HTMAXBUTTON))
            return false;
        // Must be consumed: DefWindowProc would otherwise start its own non-client
        // button-tracking loop for HTMAXBUTTON and swallow the release.
        // The DBLCLK case is folded in so the second click of a fast double-click
        // is not silently dropped.
        setMaximizeButtonPressed(true);
        store(0);
        return true;
    }

    case WM_NCLBUTTONUP: {
        if (!snap_layouts_enabled_ || msg->wParam != static_cast<WPARAM>(HTMAXBUTTON))
            return false;
        const bool was_pressed = maximize_button_pressed_;
        setMaximizeButtonPressed(false);
        if (was_pressed)
            emit maximizeButtonClicked();
        store(0);
        return true;
    }

    default:
        break;
    }

    return false;
#else
    Q_UNUSED(event_type);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

} // namespace exosnap::quick
