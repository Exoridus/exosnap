#include "QuickWindowGeometry.h"

#include "ui/WindowGeometryPolicy.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMargins>
#include <QPointer>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QtEnvironmentVariables>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace exosnap::quick {
namespace {

// The band of the title bar that must stay reachable for the window to be
// draggable. Same rule the Widgets restore used: a saved position counts as
// "on this screen" when this strip intersects the screen's work area.
constexpr int kTitleStripWidth = 200;
constexpr int kTitleStripHeight = 40;

QScreen* screenForSavedPosition(const PersistedWindowGeometry& saved) {
    const QRect title_strip(saved.x, saved.y, std::min(saved.width, kTitleStripWidth), kTitleStripHeight);
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen->availableGeometry().intersects(title_strip))
            return screen;
    }
    return nullptr;
}

// ── Native geometry ─────────────────────────────────────────────────────────
//
// MEASURED, and the reason this exists: the shell's window is frameless and its
// WM_NCCALCSIZE handler gives the client area the whole window, so what the user
// sees IS the native window rect. Qt only agrees once Qt::FramelessWindowHint
// has been applied to the QWindow — before that it creates the HWND with a full
// native frame (WS_CAPTION|WS_THICKFRAME|...) and offsets every geometry it is
// handed by that frame's 8/31/8/8 margins. Restoring a persisted rect in that
// window produced, over three consecutive starts of an earlier build,
//
//     696,68 1168x760  ->  688,37 1184x760  ->  680,6 1200x760
//
// i.e. 8 px further left and 16 px wider every launch, because each inflated
// rect was persisted in turn and inflated again on the next start. Left alone
// the window creeps off the top-left of the screen and takes its own title bar
// with it.
//
// The ordering half of that is fixed where it belongs, in the startup lifecycle
// (QuickApplication::load): the window is placed and shown only after the flags
// are final, so Qt's margins are zero and nothing inflates. What stays here is
// the round trip: persisting the rect the window actually occupies, and reading
// it back the same way, so the two ends can never disagree about which rectangle
// the saved numbers describe.

// The window's placement, as Windows itself keeps it.
//
// WINDOWPLACEMENT is the documented mechanism for saving and restoring a window,
// and it answers three questions the previous GetWindowRect/SetWindowPos pair
// could not:
//
//  * `rcNormalPosition` is the RESTORE rect -- the rectangle the window returns
//    to -- and the system maintains it. GetWindowRect reports where the window
//    IS, so while maximized or minimized it reports the screen or an off-screen
//    placeholder, which is why sampling used to be refused in those states. The
//    restore rect is meaningful in all of them.
//  * `showCmd` carries normal/maximized/minimized in the same structure, so the
//    state and the rect can never be persisted out of step with each other.
//  * The coordinates are WORKSPACE coordinates, which already exclude the
//    taskbar and any app bars. Microsoft documents that passing them to
//    screen-coordinate functions such as SetWindowPos makes a window "creep"
//    across the desktop -- so nothing here converts them, and both ends of the
//    round trip go through the same pair of calls.
struct NativePlacement {
    // Workspace coordinates, physical pixels, exactly as Windows stores them.
    QRect normal;
    bool maximized = false;
    bool minimized = false;
    bool valid = false;
};

NativePlacement nativePlacement(const QQuickWindow* window) {
    NativePlacement out;
    if (window == nullptr || window->handle() == nullptr)
        return out;
    auto hwnd = reinterpret_cast<HWND>(const_cast<QQuickWindow*>(window)->winId());
    if (hwnd == nullptr)
        return out;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) == FALSE)
        return out;
    const RECT& r = placement.rcNormalPosition;
    out.normal = QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    out.minimized = placement.showCmd == SW_SHOWMINIMIZED;
    // A minimized window is not zoomed whatever it was before; WPF_RESTORETOMAXIMIZED
    // is where Windows keeps "comes back maximized", and it is the only place that
    // survives the minimize.
    out.maximized =
        placement.showCmd == SW_SHOWMAXIMIZED || (out.minimized && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
    out.valid = true;
    return out;
}

// `show` is passed through to WINDOWPLACEMENT::showCmd. SW_HIDE is the value the
// startup path wants: it records the restore rect on a window that is not on
// screen yet WITHOUT showing it, so the first show stays where the lifecycle put
// it and no frame reaches the desktop early.
bool applyNativePlacement(QQuickWindow* window, const QRect& normal, UINT show) {
    if (window == nullptr || normal.width() <= 0 || normal.height() <= 0)
        return false;
    auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr)
        return false;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    // Read first so the minimized/maximized corner points Windows already keeps
    // are carried over rather than zeroed.
    if (GetWindowPlacement(hwnd, &placement) == FALSE)
        return false;
    placement.rcNormalPosition =
        RECT{normal.left(), normal.top(), normal.left() + normal.width(), normal.top() + normal.height()};
    placement.showCmd = show;
    // WPF_SETMINPOSITION is the only flag that would make ptMinPosition
    // authoritative, and nothing here sets that point.
    placement.flags = 0;
    return SetWindowPlacement(hwnd, &placement) != FALSE;
}

// ── Startup geometry trace ──────────────────────────────────────────────────

bool g_trace_enabled = false;

// Reads the three facts a trace line needs from Windows directly. Deliberately
// not routed through Qt: the point of the line is to show where Qt's belief and
// the operating system's reality diverge, so both sides have to be sampled at
// their own source.
struct NativeFacts {
    RECT window{};
    RECT client{};
    // WINDOWPLACEMENT::rcNormalPosition — the rect Windows will un-maximize to.
    // The only way to check a maximized restore without maximizing or restoring
    // the window by hand, and it is a different fact from the window rect: while
    // the window is maximized the two deliberately disagree.
    RECT normal{};
    bool valid = false;
    bool visible = false;
    // Whether Windows has put this window in the foreground. The only honest
    // automated answer to "did the no-activate start steal focus?" -- Qt's own
    // isActive() reports activation, which is not the same question.
    bool foreground = false;
    unsigned long long style = 0;
};

NativeFacts nativeFacts(const QQuickWindow* window) {
    NativeFacts facts;
    // handle() rather than winId(): winId() CREATES the platform window, and a
    // probe that brings the thing it is measuring into existence would report a
    // startup order it caused itself.
    if (window == nullptr || window->handle() == nullptr)
        return facts;
    auto hwnd = reinterpret_cast<HWND>(const_cast<QQuickWindow*>(window)->winId());
    if (hwnd == nullptr)
        return facts;
    facts.valid = GetWindowRect(hwnd, &facts.window) != FALSE && GetClientRect(hwnd, &facts.client) != FALSE;
    facts.visible = IsWindowVisible(hwnd) != FALSE;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(hwnd, &placement) != FALSE)
        facts.normal = placement.rcNormalPosition;
    facts.foreground = GetForegroundWindow() == hwnd;
    facts.style = static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    return facts;
}

// Hooks the three milestones that have no call site: the first expose, the
// window becoming visible, and the first swapped frame. Only the FIRST of each
// is logged -- a resize storm during a live drag would otherwise bury the
// startup sequence the trace exists for.
class WindowTraceProbe : public QObject {
  public:
    explicit WindowTraceProbe(QQuickWindow* window) : QObject(window), window_(window) {
        window->installEventFilter(this);
        connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible || visible_logged_)
                return;
            visible_logged_ = true;
            TraceWindowGeometry("first-visible", window_);
        });
        // frameSwapped fires on the render thread; queued so the trace is
        // written from the GUI thread like every other line.
        connect(
            window, &QQuickWindow::frameSwapped, this,
            [this]() {
                if (frame_logged_)
                    return;
                frame_logged_ = true;
                TraceWindowGeometry("first-frame", window_);
                // The startup negotiation is over; everything after this is the
                // user moving the window, which the message trace must not follow.
                StopStartupMessageTrace();
            },
            Qt::QueuedConnection);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == window_ && event->type() == QEvent::Expose && !expose_logged_) {
            expose_logged_ = true;
            TraceWindowGeometry("first-expose", window_);
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QPointer<QQuickWindow> window_;
    bool expose_logged_ = false;
    bool visible_logged_ = false;
    bool frame_logged_ = false;
};

// Reports the rect-deciding messages for whichever window is coming up. Qt's
// window procedure runs every message -- including the SENT ones -- through the
// installed native event filters, which is what makes WM_NCCALCSIZE and
// WM_WINDOWPOSCHANGING observable here at all.
class StartupMessageTrace : public QAbstractNativeEventFilter {
  public:
    void stop() {
        stopped_ = true;
    }

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* /*result*/) override {
        if (stopped_ || message == nullptr)
            return false;
        if (event_type != QByteArrayLiteral("windows_generic_MSG") &&
            event_type != QByteArrayLiteral("windows_dispatcher_MSG")) {
            return false;
        }
        auto* msg = static_cast<MSG*>(message);
        const auto handle = reinterpret_cast<quintptr>(msg->hwnd);
        const auto style = [msg]() { return static_cast<unsigned long long>(GetWindowLongPtrW(msg->hwnd, GWL_STYLE)); };
        switch (msg->message) {
        case WM_WINDOWPOSCHANGING:
        case WM_WINDOWPOSCHANGED: {
            const auto* pos = reinterpret_cast<const WINDOWPOS*>(msg->lParam);
            if (pos == nullptr)
                break;
            qInfo("window-msg: %s hwnd=0x%llx %ld,%ld %ldx%ld flags=0x%08x style=0x%08llx",
                  msg->message == WM_WINDOWPOSCHANGING ? "WINDOWPOSCHANGING" : "WINDOWPOSCHANGED",
                  static_cast<unsigned long long>(handle), pos->x, pos->y, pos->cx, pos->cy,
                  static_cast<unsigned>(pos->flags), style());
            break;
        }
        case WM_NCCALCSIZE: {
            const auto* params = msg->wParam == static_cast<WPARAM>(TRUE)
                                     ? reinterpret_cast<const NCCALCSIZE_PARAMS*>(msg->lParam)
                                     : nullptr;
            if (params == nullptr) {
                qInfo("window-msg: NCCALCSIZE hwnd=0x%llx wparam=0 style=0x%08llx",
                      static_cast<unsigned long long>(handle), style());
                break;
            }
            const RECT& proposed = params->rgrc[0];
            qInfo("window-msg: NCCALCSIZE hwnd=0x%llx proposed=%ld,%ld %ldx%ld style=0x%08llx",
                  static_cast<unsigned long long>(handle), proposed.left, proposed.top, proposed.right - proposed.left,
                  proposed.bottom - proposed.top, style());
            break;
        }
        case WM_GETMINMAXINFO: {
            const auto* minmax = reinterpret_cast<const MINMAXINFO*>(msg->lParam);
            if (minmax == nullptr)
                break;
            qInfo("window-msg: GETMINMAXINFO hwnd=0x%llx mintrack=%ldx%ld maxtrack=%ldx%ld style=0x%08llx",
                  static_cast<unsigned long long>(handle), minmax->ptMinTrackSize.x, minmax->ptMinTrackSize.y,
                  minmax->ptMaxTrackSize.x, minmax->ptMaxTrackSize.y, style());
            break;
        }
        case WM_SHOWWINDOW:
            qInfo("window-msg: SHOWWINDOW hwnd=0x%llx show=%d style=0x%08llx", static_cast<unsigned long long>(handle),
                  msg->wParam != 0 ? 1 : 0, style());
            break;
        case WM_STYLECHANGED: {
            const auto* styles = reinterpret_cast<const STYLESTRUCT*>(msg->lParam);
            if (styles == nullptr)
                break;
            qInfo("window-msg: STYLECHANGED hwnd=0x%llx which=0x%08llx old=0x%08llx new=0x%08llx",
                  static_cast<unsigned long long>(handle), static_cast<unsigned long long>(msg->wParam),
                  static_cast<unsigned long long>(styles->styleOld), static_cast<unsigned long long>(styles->styleNew));
            break;
        }
        default:
            break;
        }
        return false;
    }

  private:
    bool stopped_ = false;
};

std::unique_ptr<StartupMessageTrace> g_message_trace;
bool g_message_trace_persistent = false;

} // namespace

void ApplyStartupWindowGeometry(QQuickWindow* window, const QRect& logical) {
    if (window == nullptr || logical.width() <= 0 || logical.height() <= 0)
        return;
    // Qt is told as well: it keeps its own geometry and the scene is laid out
    // against it, so leaving it behind would put QML and Windows on different
    // rectangles.
    window->setGeometry(logical);

    // SW_HIDE, deliberately. This records the restore rect on a window that is
    // not on screen yet; the lifecycle's own show() is what makes it visible,
    // and a showCmd that showed it here would put a frame on the desktop before
    // anything has been rendered into it.
    if (!applyNativePlacement(window, logical, SW_HIDE))
        return;
    if (!WindowGeometryTraceEnabled())
        return;
    const NativePlacement placed = nativePlacement(window);
    if (placed.valid && placed.normal != logical) {
        qInfo("window-geometry: pre-show placement asked for %d,%d %dx%d, Windows kept %d,%d %dx%d", logical.x(),
              logical.y(), logical.width(), logical.height(), placed.normal.x(), placed.normal.y(),
              placed.normal.width(), placed.normal.height());
    }
}

bool WindowGeometryTraceEnabled() {
    return g_trace_enabled || qEnvironmentVariableIntValue("EXOSNAP_WINDOW_TRACE") != 0;
}

void SetWindowGeometryTraceEnabled(bool enabled) {
    g_trace_enabled = enabled;
}

void TraceWindowGeometry(const char* stage, const QQuickWindow* window) {
    if (!WindowGeometryTraceEnabled())
        return;
    if (window == nullptr) {
        qInfo("window-trace: %s (no window)", stage);
        return;
    }
    const QRect qt_geometry = window->geometry();
    const QRect qt_frame = window->frameGeometry();
    const QMargins margins = window->frameMargins();
    const NativeFacts facts = nativeFacts(window);
    qInfo("window-trace: %s qt=%d,%d %dx%d qt_frame=%d,%d %dx%d qt_margins=%d,%d,%d,%d native_window=%ld,%ld %ldx%ld "
          "native_client=%ldx%ld native_normal=%ld,%ld %ldx%ld qt_visible=%d native_visible=%d visibility=%d "
          "qt_active=%d foreground=%d style=0x%08llx dpr=%.2f",
          stage, qt_geometry.x(), qt_geometry.y(), qt_geometry.width(), qt_geometry.height(), qt_frame.x(),
          qt_frame.y(), qt_frame.width(), qt_frame.height(), margins.left(), margins.top(), margins.right(),
          margins.bottom(), facts.window.left, facts.window.top, facts.window.right - facts.window.left,
          facts.window.bottom - facts.window.top, facts.client.right - facts.client.left,
          facts.client.bottom - facts.client.top, facts.normal.left, facts.normal.top,
          facts.normal.right - facts.normal.left, facts.normal.bottom - facts.normal.top, window->isVisible() ? 1 : 0,
          facts.visible ? 1 : 0, static_cast<int>(window->visibility()), window->isActive() ? 1 : 0,
          facts.foreground ? 1 : 0, facts.style, window->devicePixelRatio());
}

void InstallWindowGeometryTrace(QQuickWindow* window) {
    if (window == nullptr || !WindowGeometryTraceEnabled())
        return;
    new WindowTraceProbe(window);
}

void InstallStartupMessageTrace() {
    if (!WindowGeometryTraceEnabled() || g_message_trace || QCoreApplication::instance() == nullptr)
        return;
    g_message_trace = std::make_unique<StartupMessageTrace>();
    QCoreApplication::instance()->installNativeEventFilter(g_message_trace.get());
}

void SetStartupMessageTracePersistent(bool persistent) {
    g_message_trace_persistent = persistent;
}

void StopStartupMessageTrace() {
    if (!g_message_trace || g_message_trace_persistent)
        return;
    g_message_trace->stop();
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeNativeEventFilter(g_message_trace.get());
    g_message_trace.reset();
}

ResolvedWindowGeometry ResolveWindowGeometry(const PersistedWindowGeometry& saved, const QSize& minimum,
                                             const QSize& fallback_size) {
    ResolvedWindowGeometry resolved;

    // A saved rect is handed back UNCHANGED, and deliberately so.
    //
    // It came out of WINDOWPLACEMENT and it goes back in through
    // SetWindowPlacement, which places into the workspace itself: a rect on a
    // monitor that is no longer connected, or one that would sit under the
    // taskbar, is Windows' problem to solve and Windows already solves it. The
    // clamp that used to live here compared those workspace coordinates against
    // QScreen::availableGeometry(), which is screen coordinates -- the mixing
    // Microsoft documents as the cause of windows creeping across the desktop.
    // On a taskbar-at-the-bottom setup the two happen to coincide, which is
    // exactly why a bug like that survives review.
    //
    // The one thing still decided here is the SIZE, because a rect smaller than
    // the shell's minimum is not something the window manager can know about.
    if (saved.width > 0 && saved.height > 0) {
        resolved.rect =
            QRect(saved.x, saved.y, std::max(saved.width, minimum.width()), std::max(saved.height, minimum.height()));
        resolved.maximized = saved.maximized;
        return resolved;
    }

    // No saved rect: the first-launch decision, which is a different problem and
    // still ours. It is computed from the primary screen's work area in SCREEN
    // coordinates and never travels through WINDOWPLACEMENT, so the two
    // coordinate systems still never meet.
    QScreen* primary = QGuiApplication::primaryScreen();
    if (primary == nullptr) {
        // No screens at all (offscreen platform plugin). Nothing to centre
        // against; hand back the default size at the origin.
        QSize size = fallback_size;
        size.setWidth(std::max(size.width(), minimum.width()));
        size.setHeight(std::max(size.height(), minimum.height()));
        resolved.rect = QRect(QPoint(0, 0), size);
        return resolved;
    }
    const ui::StartupWindowPlacement placement = ui::ResolveStartupWindowPlacement(
        QRect(), false, primary->availableGeometry(), minimum, fallback_size, /*center_on_primary=*/true);
    resolved.rect = placement.rect;
    resolved.maximized = placement.maximized;
    return resolved;
}

QuickWindowGeometry::QuickWindowGeometry(QQuickWindow* window, PersistedWindowGeometry initial,
                                         std::function<void(const PersistedWindowGeometry&)> sink, QObject* parent)
    : QObject(parent), window_(window), current_(initial), sink_(std::move(sink)) {
    if (window_ == nullptr)
        return;

    persist_timer_.setSingleShot(true);
    // Long enough that a drag or a resize is one write rather than hundreds,
    // short enough that a normal close never has to rely on the flush path.
    persist_timer_.setInterval(400);
    connect(&persist_timer_, &QTimer::timeout, this, [this]() {
        if (!dirty_)
            return;
        dirty_ = false;
        if (sink_)
            sink_(current_);
    });

    // Sampling stays disarmed until the window is actually up and standing on
    // the rect it was placed on. Everything before that is Qt bringing the window
    // into existence rather than the user moving it, and persisting any of it is
    // what made the geometry grow from launch to launch.
    connect(window_, &QQuickWindow::xChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::yChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::widthChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::heightChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::visibilityChanged, this, [this]() { sampleVisibility(); });

    sampleVisibility();

    if (current_.width <= 0 || current_.height <= 0) {
        armed_ = true;
        return;
    }

    // The first swapped frame is the first moment the window is definitively up,
    // and under the current lifecycle it is also the first frame the user can
    // see. The window was placed on `intended` while it was still hidden and
    // nothing between that and here may move it, so this checks rather than
    // corrects: a correction applied at this point would be a frame the user has
    // already seen in the wrong place, which is the defect the ordering removed.
    //
    // frameSwapped fires on the render thread, so this is queued back onto the
    // GUI thread.
    const QRect intended(current_.x, current_.y, current_.width, current_.height);
    connect(
        window_, &QQuickWindow::frameSwapped, this,
        [this, intended]() {
            if (armed_ || detached_ || window_ == nullptr)
                return;
            armed_ = true;
            // One write the moment sampling is allowed, and unconditionally.
            //
            // Without it a window that is never moved persists nothing at all:
            // every later write is triggered by a CHANGE, and `current_` was
            // constructed from the geometry the window was just placed on, so the
            // first sample matches it and reports nothing to do. The sink compares
            // against what is on DISK, which is a different question and the one
            // that matters here -- so "restore where I left it" used to work only
            // for someone who had dragged the window at least once.
            sampleAndSchedule();
            dirty_ = true;
            persist_timer_.start();
            if (current_.maximized || window_->visibility() != QWindow::Windowed)
                return;
            const NativePlacement placed = nativePlacement(window_);
            const QRect actual = placed.valid ? placed.normal : window_->geometry();
            if (actual == intended) {
                if (WindowGeometryTraceEnabled())
                    qInfo("window-geometry: first frame on %d,%d %dx%d as placed", intended.x(), intended.y(),
                          intended.width(), intended.height());
                return;
            }
            // Reaching this means the startup ordering broke: something moved the
            // window between the pre-show placement and its first frame. Reported
            // loudly and left alone, so the next such regression is a log line
            // rather than a jump nobody can attribute.
            qWarning("window-geometry: first frame at %d,%d %dx%d but the window was placed at %d,%d %dx%d", actual.x(),
                     actual.y(), actual.width(), actual.height(), intended.x(), intended.y(), intended.width(),
                     intended.height());
        },
        Qt::QueuedConnection);
}

void QuickWindowGeometry::detach() {
    detached_ = true;
    armed_ = true;
    dirty_ = false;
    persist_timer_.stop();
}

void QuickWindowGeometry::sampleAndSchedule() {
    if (window_ == nullptr || !armed_ || detached_)
        return;
    const NativePlacement placement = nativePlacement(window_);
    if (!placement.valid)
        return;

    // Both facts come out of the SAME WINDOWPLACEMENT read, so the rect and the
    // maximized flag can never be persisted describing different moments.
    //
    // And both are sampled in every state. The restore rect is what Windows will
    // un-maximize or un-minimize back to, so it stays meaningful while the window
    // is maximized or minimized -- which is exactly when the old GetWindowRect
    // reported the screen instead and sampling had to be refused.
    bool changed = false;
    if (current_.maximized != placement.maximized) {
        current_.maximized = placement.maximized;
        changed = true;
    }
    const QRect& normal = placement.normal;
    if (normal.width() > 0 && normal.height() > 0 &&
        (current_.x != normal.x() || current_.y != normal.y() || current_.width != normal.width() ||
         current_.height != normal.height())) {
        current_.x = normal.x();
        current_.y = normal.y();
        current_.width = normal.width();
        current_.height = normal.height();
        changed = true;
    }
    if (!changed)
        return;
    dirty_ = true;
    persist_timer_.start();
}

void QuickWindowGeometry::sampleVisibility() {
    if (window_ == nullptr || detached_)
        return;
    // Hidden is not a state to persist anything from: it is the window on its way
    // up or on its way out, and neither carries a placement the next launch wants.
    if (window_->visibility() == QWindow::Hidden)
        return;
    sampleAndSchedule();
}

void QuickWindowGeometry::flush() {
    if (!dirty_ || detached_)
        return;
    persist_timer_.stop();
    dirty_ = false;
    if (sink_)
        sink_(current_);
}

} // namespace exosnap::quick
