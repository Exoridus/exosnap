#include "QuickWindowChrome.h"

#include "QuickWindowGeometry.h"

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

// Resource ids and .ico paths for the three window-icon variants, kept in the same
// order as QuickWindowChrome::IconState.
struct IconVariant {
    const char* qrc_path;
    WORD resource_id;
};

IconVariant iconVariantFor(QuickWindowChrome::IconState state) {
    switch (state) {
    case QuickWindowChrome::Recording:
        return {":/brand/exosnap-logo-recording.ico", IDI_EXOSNAP_APP_ICON_RECORDING};
    case QuickWindowChrome::Paused:
        return {":/brand/exosnap-logo-paused.ico", IDI_EXOSNAP_APP_ICON_PAUSED};
    case QuickWindowChrome::Idle:
        break;
    }
    return {":/brand/exosnap-logo-idle.ico", IDI_EXOSNAP_APP_ICON};
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
    TraceWindowGeometry("chrome-hwnd-created", window);

    // Qt recreates the platform window for some flag and DPI transitions, which
    // invalidates the cached HWND; a screen change is the cheapest observable
    // proxy for that and also the moment the DWM border wants re-applying.
    QObject::connect(window, &QWindow::screenChanged, this, [this]() { refreshHandle(); });
    QObject::connect(window, &QObject::destroyed, this, [this]() { detach(); });

    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->installNativeEventFilter(this);

    ensureResizableStyle();
    applyBorderColor("attach");
    TraceWindowGeometry("chrome-style-applied", window);
    emit targetChanged();
}

void QuickWindowChrome::detach() {
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeNativeEventFilter(this);
    if (!target_.isNull()) {
        // Broad form on purpose: QML property bindings that read this object's
        // properties are owned by binding objects, never by `this`, so nothing a
        // QML author writes can be caught by this disconnect.
        QObject::disconnect(target_, nullptr, this, nullptr);
    }
    target_ = nullptr;
    hwnd_ = nullptr;
    non_client_leave_tracked_ = false;
    setMaximizeButtonHovered(false);
    setMaximizeButtonPressed(false);
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
    ensureResizableStyle();
    applyBorderColor("handle-recreated");
}

void QuickWindowChrome::applyNativeWindowStyle() {
    if (target_.isNull())
        return;
    // The handle first: applying the window flags is exactly the kind of change
    // that can make Qt recreate the platform window, and a stale HWND here would
    // style a window that no longer exists.
    refreshHandle();
    ensureResizableStyle();
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

void QuickWindowChrome::applyWindowIcon(IconState state) {
#if defined(Q_OS_WIN)
    const IconVariant variant = iconVariantFor(state);

    const QIcon icon(QString::fromLatin1(variant.qrc_path));
    if (icon.isNull()) {
        qWarning().noquote() << "QuickWindowChrome: icon load failed from" << variant.qrc_path;
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
    // resources. It contributes only if exosnap.rc is compiled into this target;
    // when it is not, LoadImageW fails and the Qt path above is the whole story.
    // LR_SHARED is safe because the OS caches per (instance, id, size) tuple and
    // the three variants use distinct ids.
    HICON small_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(variant.resource_id), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON big_icon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(variant.resource_id), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    if (small_icon != nullptr)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (big_icon != nullptr)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
#else
    Q_UNUSED(state);
#endif
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------

void QuickWindowChrome::ensureResizableStyle() const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr)
        return;

    // Aero Snap, Win+Arrow and the native resize drag are all gated on
    // WS_THICKFRAME. A frameless Qt window does not get it, so it is re-added and
    // committed with SWP_FRAMECHANGED — without that flag Windows keeps using the
    // cached frame metrics and the style change has no observable effect.
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_THICKFRAME) != 0)
        return;
    style |= WS_THICKFRAME;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
#endif
}

void QuickWindowChrome::applyBorderColor(const char* reason) const {
#if defined(Q_OS_WIN)
    HWND hwnd = static_cast<HWND>(hwnd_);
    if (hwnd == nullptr || !border_color_.isValid())
        return;

    const COLORREF colorref = RGB(border_color_.red(), border_color_.green(), border_color_.blue());
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
    }
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
    // zones matter most.
    double dpr = target_->devicePixelRatio();
    if (!(dpr > 0.0))
        dpr = 1.0;
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

    switch (msg->message) {
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
            double dpr = target_->devicePixelRatio();
            if (!(dpr > 0.0))
                dpr = 1.0;
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
