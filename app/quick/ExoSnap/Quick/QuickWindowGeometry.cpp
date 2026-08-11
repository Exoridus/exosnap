#include "QuickWindowGeometry.h"

#include "ui/WindowGeometryPolicy.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <cmath>

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
// sees IS the native window rect. Qt does not agree during window creation — it
// treats the requested x/y/width/height as a client rect and expands it by a
// frame that this window does not have. Measured over three consecutive starts
// of the shipped build, restoring a persisted rect produced
//
//     696,68 1168x760  ->  688,37 1184x760  ->  680,6 1200x760
//
// i.e. 8 px further left and 16 px wider every launch, and each grown rect was
// persisted in turn. Left alone the window creeps off the top-left of the screen
// and takes its own title bar with it. (Qt reports frameMargins of 0,0,0,0 in
// the steady state, which is why this cannot be corrected by subtracting them.)
//
// So the round trip is made native on both ends: what is persisted is the rect
// the window actually occupies, and the restore is applied with SetWindowPos
// rather than through Qt's frame arithmetic.

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

} // namespace

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

    ResolvedWindowGeometry resolved;
    resolved.maximized = has_saved_rect && saved.maximized;

    if (target == nullptr) {
        // No screens at all (headless/offscreen platform plugin). Nothing to
        // clamp against; hand back the saved or default size unchanged.
        resolved.rect =
            has_saved_rect ? QRect(saved.x, saved.y, saved.width, saved.height) : QRect(QPoint(0, 0), fallback_size);
        return resolved;
    }

    const QRect available = target->availableGeometry();
    const QSize size = has_saved_rect ? QSize(saved.width, saved.height) : fallback_size;
    const QRect requested = has_saved_rect
                                ? QRect(saved.x, saved.y, saved.width, saved.height)
                                : QRect(available.center() - QPoint(size.width() / 2, size.height() / 2), size);

    // Two stages, exactly as the Widgets restore did. The first preserves a
    // deliberately-placed window while guaranteeing a reachable title strip; the
    // second is the full containment clamp, which is what stops a bottom edge
    // ending up under the taskbar. Skipped when the window will be maximized,
    // because the maximized state fills the work area by itself and the rect
    // being carried here is only the restore rect.
    QRect rect = ui::ClampRestoredWindowGeometry(requested, available, minimum, center_on_primary);
    if (!resolved.maximized)
        rect = ui::ClampWindowToWorkArea(rect, available);
    resolved.rect = rect;
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
    // the rect it was restored to. Everything before that is Qt bringing the
    // window into existence, and one of those steps expands the rect by a frame
    // this window does not have (see nativeLogicalGeometry). Persisting any of
    // it is what made the geometry grow from launch to launch.
    connect(window_, &QQuickWindow::xChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::yChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::widthChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::heightChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::visibilityChanged, this, [this]() { sampleVisibility(); });

    sampleVisibility();

    // Correct the placement once, after the window exists and Qt has finished
    // creating it. Deferred rather than immediate: the expansion happens inside
    // window creation, so a correction issued in the same turn is overwritten by
    // the thing it is correcting. A maximized restore is left alone — the rect
    // carried here is only its un-maximize target.
    if (current_.width <= 0 || current_.height <= 0) {
        armed_ = true;
        return;
    }

    // The first swapped frame is the first moment the window is definitively up
    // — Qt's creation-time geometry work, including the expanding setGeometry,
    // is behind it. frameSwapped fires on the render thread, so the correction
    // is queued back onto the GUI thread.
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
            if (actual == intended)
                return;
            qInfo("window-geometry: restoring %d,%d %dx%d (came up as %d,%d %dx%d)", intended.x(), intended.y(),
                  intended.width(), intended.height(), actual.x(), actual.y(), actual.width(), actual.height());
            applyNativeLogicalGeometry(window_, intended);
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
