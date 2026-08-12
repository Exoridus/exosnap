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

// Places `window` on `logical` BEFORE it is shown for the first time.
//
// Once Qt::FramelessWindowHint has been applied Qt reports zero frame margins,
// so QWindow::setGeometry reaches SetWindowPos unchanged and is exact. That is a
// Qt implementation detail, though, and this is the one placement the user must
// never see go wrong -- so the result is measured against Windows and corrected
// if the two disagree. The correction is invisible by construction: it happens
// while nothing is on screen. Correcting AFTER the first frame, which is what
// this replaces, is a frame the user has already seen in the wrong place.
void ApplyStartupWindowGeometry(QQuickWindow* window, const QRect& logical);

// ── Startup geometry trace ──────────────────────────────────────────────────
//
// The measurement seam for this window's startup. "The window ends up in the
// right place" is not the property that matters -- the window used to end up in
// the right place while visibly jumping there -- so what has to be observable is
// the ORDER: which rect the window holds at each lifecycle step, and at which
// step it first becomes visible to the user.
//
// Every line reports both spaces at once, because the whole class of defect here
// is Qt and Windows disagreeing about what the rect means: Qt's logical
// geometry, Qt's believed frame margins, and the native window/client rects.
//
// Off unless explicitly enabled, so a normal run's log is unchanged.
[[nodiscard]] bool WindowGeometryTraceEnabled();
void SetWindowGeometryTraceEnabled(bool enabled);

// One `window-trace: <stage> ...` line. `window` may be null -- the earliest
// stages happen before any window exists -- and the line then carries the stage
// alone.
void TraceWindowGeometry(const char* stage, const QQuickWindow* window);

// Attaches the trace points that cannot be reached from a call site: the first
// expose, the first show and the first swapped frame. No-op while tracing is
// off; the probe is parented to the window and dies with it.
void InstallWindowGeometryTrace(QQuickWindow* window);

// Logs the Win32 messages that decide a window's rect (WM_NCCALCSIZE,
// WM_WINDOWPOSCHANGING/CHANGED, WM_GETMINMAXINFO, WM_SHOWWINDOW, style changes)
// while the window is coming up.
//
// This is the only way to see the ORDER of the negotiation: those messages are
// SENT, not posted, so they never appear in a log written from Qt's own signals
// -- by the time xChanged arrives the decision has already been made and its
// cause is gone. Installed before the window exists, and it stops itself after
// the first frame so a live resize drag cannot flood the log.
//
// No-op while tracing is off.
void InstallStartupMessageTrace();
void StopStartupMessageTrace();

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

    // Hands the window over to a caller that owns its size for the rest of the
    // process: the deferred one-shot restore is abandoned and nothing further is
    // persisted.
    //
    // This exists for the render harness, which resizes the window to the size
    // it was asked to photograph. That resize happens before the first frame,
    // and the restore fires after it — so without this the harness silently
    // photographed whatever rect happened to be persisted, and then persisted
    // the harness's own size over the developer's real window in the bargain.
    // Both halves are one decision: a run that dictates the size is also a run
    // whose size means nothing to the next launch.
    void detach();

  private:
    void sampleAndSchedule();
    void sampleVisibility();

    QPointer<QQuickWindow> window_;
    PersistedWindowGeometry current_;
    std::function<void(const PersistedWindowGeometry&)> sink_;
    QTimer persist_timer_;
    bool dirty_ = false;
    // Set by detach(): the window's size belongs to someone else now, so neither
    // the pending restore nor any sample may act.
    bool detached_ = false;
    // False until the window has produced a frame and been placed on its
    // restored rect. Geometry changes before that are Qt creating the window,
    // not the user moving it, and persisting them made the window grow on every
    // launch.
    bool armed_ = false;
};

} // namespace exosnap::quick
