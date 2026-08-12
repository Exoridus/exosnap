#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QRectF>
#include <QtQmlIntegration/qqmlintegration.h>

#include <functional>

namespace exosnap::quick {

// Capture exclusion (WDA_EXCLUDEFROMCAPTURE) for a Qt Quick overlay window.
//
// The five capture-excluded overlays are on screen while ExoSnap records that
// same screen. Their whole reason to exist is that the recording must not show
// them. That makes exclusion a correctness property, not a nicety: when the
// platform call fails there is no degraded mode — the window stays hidden for
// the rest of the session, without a retry. A retry that succeeded on the
// second attempt would still leave the first attempt's frames unaccounted for,
// and nothing downstream can prove the overlay never made it into the file.
//
// QML usage — the gate is a binding, never imperative show/hide code:
//
//     Window {
//         id: overlay
//         visible: exclusion.granted && someBusinessCondition
//         CaptureExclusion { id: exclusion; target: overlay }
//     }
//
// `granted` starts false, so every ordering of property assignment and binding
// evaluation resolves to "hidden" first and can only open up after a proven
// successful call. That is what makes the fail-closed property independent of
// QML's creation order.
class CaptureExclusion : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickWindow* target READ target WRITE setTarget NOTIFY targetChanged FINAL)
    Q_PROPERTY(bool granted READ granted NOTIFY grantedChanged FINAL)
    Q_PROPERTY(bool resolved READ resolved NOTIFY resolvedChanged FINAL)

  public:
    // Platform-neutral signature for the test seam: `hwnd` is the native window
    // handle as an opaque pointer, `affinity` the WDA_* constant. Deliberately
    // free of windows.h types so the seam compiles wherever the module does.
    using AffinityFunction = std::function<bool(void* hwnd, quint32 affinity)>;

    explicit CaptureExclusion(QObject* parent = nullptr);

    [[nodiscard]] QQuickWindow* target() const noexcept;

    // Forces the platform window into existence and applies the exclusion
    // immediately — see the .cpp for why this cannot wait for a later round.
    void setTarget(QQuickWindow* window);

    // True only after a platform call actually reported success. Monotone
    // downward: once an attempt has resolved to false it never returns to true,
    // not even for a different target window.
    [[nodiscard]] bool granted() const noexcept;

    // True once an attempt has been made, whatever its outcome. Lets QML and
    // tests distinguish "not tried yet" from "tried and refused".
    [[nodiscard]] bool resolved() const noexcept;

    // Restricts the window's input region to the union of `rects` (window
    // coordinates), so clicks in the transparent gaps fall through to whatever
    // is behind. An empty list clears the mask. Used by the notification toast,
    // whose one window spans a stack of separate cards; the four fully
    // click-through overlays use Qt::WindowTransparentForInput instead.
    Q_INVOKABLE void setClickThroughRegion(const QList<QRectF>& rects);

    // ── Test seam ────────────────────────────────────────────────────────────
    // Replaces the platform call for the duration of a test. A static hook was
    // chosen over an injected strategy object or an environment variable
    // because: (a) the QML side must stay a bare `CaptureExclusion { }` with no
    // extra wiring, which rules out constructor injection; (b) an env-gated
    // simulation would put a branch that can disable a correctness guarantee
    // into the shipping binary, readable by anything that can set the
    // environment. This symbol only exists for callers that link the class
    // directly, and production code never touches it.
    static void SetAffinityFunctionForTest(AffinityFunction fn);
    static void ResetAffinityFunctionForTest();

  signals:
    void targetChanged();
    void grantedChanged();
    void resolvedChanged();

  private:
    void setGranted(bool granted);
    void markResolved();
    [[nodiscard]] static bool applyAffinity(QQuickWindow* window);

    // QPointer: the window may outlive or predecease this helper depending on
    // which side QML tears down first, and setClickThroughRegion() can be
    // called from a queued adapter signal after the window is gone.
    QPointer<QQuickWindow> target_;
    bool granted_ = false;
    bool resolved_ = false;
    // The WS_EX_LAYERED correction runs on every show; the log line for it runs
    // once, so a session with twenty recordings does not carry twenty copies.
    bool composition_logged_ = false;
};

} // namespace exosnap::quick
