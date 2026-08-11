#pragma once

// QuickWindowGeometry -- persisted main-window placement for the Quick frontend.
//
// The policy itself is not new: ui/WindowGeometryPolicy holds the two pure
// clamps the Widgets shell already used, and they are reused verbatim here. What
// this adds is the part that cannot be pure, and the part a QWindow does not
// give for free:
//
//  - QWidget has normalGeometry(), which reports the restore rect even while
//    maximized. QWindow has no equivalent, so the last windowed rect has to be
//    tracked as it happens -- otherwise closing a maximized window persists the
//    maximized rect as the restore rect and the window never un-maximizes to
//    anything sensible again.
//  - A minimized window reports meaningless geometry on Windows. Sampling it
//    would persist that, so only Windowed visibility is ever recorded.
//  - Writes are debounced: a single user drag emits hundreds of position
//    changes, and each one would otherwise be a settings file write.

#include "settings/AppSettingsStore.h"

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <functional>

class QQuickWindow;

namespace exosnap::quick {

// Concrete placement to create the window at. Unlike the Widgets path this is
// always resolved -- there is no "let the toolkit decide" case. A first launch
// centres the default size on the primary screen instead, which is deterministic
// and, unlike Qt's cascade placement, cannot land partly under the taskbar.
struct ResolvedWindowGeometry {
    QRect rect;
    bool maximized = false;
};

// `saved` may be the all -1 first-launch value; `fallback_size` is then centred
// on the primary screen. The result is always inside a connected screen's work
// area, so a monitor that has been unplugged since the last run cannot strand
// the window off-screen.
[[nodiscard]] ResolvedWindowGeometry ResolveWindowGeometry(const PersistedWindowGeometry& saved, const QSize& minimum,
                                                           const QSize& fallback_size);

// Tracks a live window and reports the geometry worth persisting. Owns nothing
// but its timer; the sink decides where the value goes.
class QuickWindowGeometry : public QObject {
    Q_OBJECT

  public:
    // The window is tracked, never owned. A QPointer rather than a raw pointer
    // because teardown order is not guaranteed: the engine (and with it the
    // window) can be destroyed while this object is still alive, and a flush on
    // the way out must then be a no-op rather than a use-after-free.
    QuickWindowGeometry(QQuickWindow* window, PersistedWindowGeometry initial,
                        std::function<void(const PersistedWindowGeometry&)> sink, QObject* parent = nullptr);

    // Writes immediately if a debounced change is still owed. Called on the way
    // out so a quit inside the debounce window does not lose the last move.
    void flush();

    [[nodiscard]] const PersistedWindowGeometry& current() const noexcept {
        return current_;
    }

  private:
    void sampleAndSchedule();
    void sampleVisibility();

    QPointer<QQuickWindow> window_;
    PersistedWindowGeometry current_;
    std::function<void(const PersistedWindowGeometry&)> sink_;
    QTimer persist_timer_;
    bool dirty_ = false;
};

} // namespace exosnap::quick
