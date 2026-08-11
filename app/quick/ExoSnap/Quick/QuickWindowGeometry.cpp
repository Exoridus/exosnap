#include "QuickWindowGeometry.h"

#include "ui/WindowGeometryPolicy.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>

#include <algorithm>

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

    connect(window_, &QQuickWindow::xChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::yChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::widthChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::heightChanged, this, [this]() { sampleAndSchedule(); });
    connect(window_, &QQuickWindow::visibilityChanged, this, [this]() { sampleVisibility(); });

    sampleVisibility();
}

void QuickWindowGeometry::sampleAndSchedule() {
    if (window_ == nullptr)
        return;
    // Only a Windowed window has a meaningful restore rect. While maximized,
    // minimized or full-screen the reported geometry is the screen (or, when
    // minimized on Windows, an off-screen placeholder) -- persisting either
    // would destroy the rect the window has to un-maximize back to.
    if (window_->visibility() != QWindow::Windowed)
        return;
    const QRect frame = window_->geometry();
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
    if (window_ == nullptr)
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
    if (!dirty_)
        return;
    persist_timer_.stop();
    dirty_ = false;
    if (sink_)
        sink_(current_);
}

} // namespace exosnap::quick
