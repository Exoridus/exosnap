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

QRect nativeLogicalGeometry(QQuickWindow* window) {
    auto hwnd = reinterpret_cast<HWND>(window->winId());
    RECT rect{};
    if (hwnd == nullptr || GetWindowRect(hwnd, &rect) == FALSE)
        return window->geometry();
    double dpr = window->devicePixelRatio();
    if (!(dpr > 0.0))
        dpr = 1.0;
    return QRect(static_cast<int>(std::lround(rect.left / dpr)), static_cast<int>(std::lround(rect.top / dpr)),
                 static_cast<int>(std::lround((rect.right - rect.left) / dpr)),
                 static_cast<int>(std::lround((rect.bottom - rect.top) / dpr)));
}

void applyNativeLogicalGeometry(QQuickWindow* window, const QRect& logical) {
    auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr || logical.width() <= 0 || logical.height() <= 0)
        return;
    double dpr = window->devicePixelRatio();
    if (!(dpr > 0.0))
        dpr = 1.0;
    SetWindowPos(hwnd, nullptr, static_cast<int>(std::lround(logical.x() * dpr)),
                 static_cast<int>(std::lround(logical.y() * dpr)), static_cast<int>(std::lround(logical.width() * dpr)),
                 static_cast<int>(std::lround(logical.height() * dpr)),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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

} // namespace

void ApplyStartupWindowGeometry(QQuickWindow* window, const QRect& logical) {
    if (window == nullptr || logical.width() <= 0 || logical.height() <= 0)
        return;
    window->setGeometry(logical);
    const QRect actual = nativeLogicalGeometry(window);
    if (actual == logical)
        return;
    // Not a warning: a DPI-rounded edge is a legitimate one-pixel disagreement,
    // and the line is here so the correction is never silent, not to raise an
    // alarm. A LARGE disagreement is the interesting case and shows up in the
    // same line.
    qInfo("window-geometry: pre-show placement corrected to %d,%d %dx%d (Qt placed it at %d,%d %dx%d)", logical.x(),
          logical.y(), logical.width(), logical.height(), actual.x(), actual.y(), actual.width(), actual.height());
    applyNativeLogicalGeometry(window, logical);
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

void StopStartupMessageTrace() {
    if (!g_message_trace)
        return;
    g_message_trace->stop();
    if (QCoreApplication::instance() != nullptr)
        QCoreApplication::instance()->removeNativeEventFilter(g_message_trace.get());
    g_message_trace.reset();
}

ResolvedWindowGeometry ResolveWindowGeometry(const PersistedWindowGeometry& saved, const QSize& minimum,
                                             const QSize& fallback_size) {
    QScreen* target = nullptr;
    const bool has_saved_rect = saved.width > 0 && saved.height > 0;
    if (has_saved_rect)
        target = screenForSavedPosition(saved);

    // Saved position landed on no connected monitor -- or there is no saved
    // position at all. Either way the window is centred on the primary screen
    // rather than restored to coordinates nothing can display.
    const bool center_on_primary = target == nullptr;
    if (target == nullptr)
        target = QGuiApplication::primaryScreen();

    const QRect saved_rect = has_saved_rect ? QRect(saved.x, saved.y, saved.width, saved.height) : QRect();

    ResolvedWindowGeometry resolved;
    resolved.maximized = has_saved_rect && saved.maximized;

    if (target == nullptr) {
        // No screens at all (headless/offscreen platform plugin). Nothing to
        // clamp against; hand back the saved or default size unchanged.
        resolved.rect = has_saved_rect ? saved_rect : QRect(QPoint(0, 0), fallback_size);
        return resolved;
    }

    // Everything from here is screen-independent and therefore lives in the pure
    // policy, where it can be tested against work areas this machine does not
    // have. This function's own job is only to decide WHICH screen.
    const ui::StartupWindowPlacement placement = ui::ResolveStartupWindowPlacement(
        saved_rect, saved.maximized, target->availableGeometry(), minimum, fallback_size, center_on_primary);
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
            if (current_.maximized || window_->visibility() != QWindow::Windowed)
                return;
            const QRect actual = nativeLogicalGeometry(window_);
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
    // Only a Windowed window has a meaningful restore rect. While maximized,
    // minimized or full-screen the reported geometry is the screen (or, when
    // minimized on Windows, an off-screen placeholder) -- persisting either
    // would destroy the rect the window has to un-maximize back to.
    if (window_->visibility() != QWindow::Windowed)
        return;
    // The NATIVE rect, not window_->geometry(): for this frameless window they
    // are the same thing once it is on screen, but only the native one is what
    // the restore above can reproduce exactly. Persisting Qt's value is what let
    // the launch-to-launch expansion accumulate.
    const QRect frame = nativeLogicalGeometry(window_);
    if (frame.width() <= 0 || frame.height() <= 0)
        return;
    current_.x = frame.x();
    current_.y = frame.y();
    current_.width = frame.width();
    current_.height = frame.height();
    dirty_ = true;
    persist_timer_.start();
}

void QuickWindowGeometry::sampleVisibility() {
    if (window_ == nullptr || detached_)
        return;
    const QWindow::Visibility visibility = window_->visibility();
    // Minimized is not a persisted state: a window minimized at quit must come
    // back as whatever it was before, not as a window that cannot be seen.
    if (visibility == QWindow::Minimized || visibility == QWindow::Hidden)
        return;
    const bool maximized = visibility == QWindow::Maximized || visibility == QWindow::FullScreen;
    if (current_.maximized != maximized) {
        current_.maximized = maximized;
        dirty_ = true;
        persist_timer_.start();
    }
    if (visibility == QWindow::Windowed)
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
