#include "CaptureExclusion.h"

#include "diagnostics/AppLog.h"

#include <QRegion>

#if defined(Q_OS_WIN)
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was introduced in Windows 10 2004 (build 19041) and is
// missing from older SDK headers.
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::quick {

namespace {

// The WDA_EXCLUDEFROMCAPTURE value, repeated platform-neutrally so the test
// seam can assert on it without pulling windows.h into the test translation
// unit.
constexpr quint32 kExcludeFromCapture = 0x00000011u;

CaptureExclusion::AffinityFunction& affinityOverride() {
    static CaptureExclusion::AffinityFunction fn;
    return fn;
}

} // namespace

CaptureExclusion::CaptureExclusion(QObject* parent) : QObject(parent) {
}

QQuickWindow* CaptureExclusion::target() const noexcept {
    return target_.data();
}

void CaptureExclusion::setTarget(QQuickWindow* window) {
    if (target_ == window)
        return;

    target_ = window;
    emit targetChanged();

    if (window == nullptr)
        return;

    // A previous attempt in this session already failed. Do not try again:
    // flipping `granted` back to true would un-hide a window we have already
    // decided must stay hidden, and the second call's success says nothing
    // about the window's state during the first.
    const bool latched_failure = resolved_ && !granted_;

    // create() allocates the platform window (the HWND) synchronously without
    // showing it. This has to happen here rather than being left to the window
    // itself, because a QML binding is only re-evaluated on the next round: if
    // we waited for the window to hand us a handle of its own accord, the
    // window could already be on screen by the time we call
    // SetWindowDisplayAffinity. With no handle there is nothing to apply the
    // affinity to — and equally, nothing that could have been captured yet.
    window->create();

    const bool ok = latched_failure ? false : applyAffinity(window);

    markResolved();
    setGranted(ok);

    // Logged unconditionally, at info level, because this is the moment a
    // correctness property is decided and there is no later evidence of it: a
    // refused exclusion produces a window that simply never appears, which is
    // indistinguishable from a disabled setting when reading a support bundle.
    // It is also the only positive control for a capture-exclusion test -- "the
    // overlay is absent from the recording" means nothing unless the overlay was
    // proven to be on screen.
    diagnostics::AppLog::info(
        QStringLiteral("overlay"),
        QStringLiteral("capture exclusion %1 for %2")
            .arg(ok ? QStringLiteral("granted") : QStringLiteral("REFUSED"),
                 window->objectName().isEmpty() ? QStringLiteral("(unnamed)") : window->objectName()));

    if (!ok) {
        // Second safeguard, independent of the QML `visible` binding: with the
        // platform window destroyed there is nothing for the compositor to put
        // on screen even if a later edit accidentally makes `visible` true.
        window->destroy();
    }
}

bool CaptureExclusion::granted() const noexcept {
    return granted_;
}

bool CaptureExclusion::resolved() const noexcept {
    return resolved_;
}

void CaptureExclusion::setClickThroughRegion(const QList<QRectF>& rects) {
    if (target_.isNull())
        return;

    if (rects.isEmpty()) {
        // An empty region is Qt's "no mask" state — the whole window takes input.
        target_->setMask(QRegion());
        return;
    }

    QRegion region;
    for (const QRectF& rect : rects)
        region += rect.toAlignedRect();
    target_->setMask(region);
}

void CaptureExclusion::SetAffinityFunctionForTest(AffinityFunction fn) {
    affinityOverride() = std::move(fn);
}

void CaptureExclusion::ResetAffinityFunctionForTest() {
    affinityOverride() = AffinityFunction();
}

void CaptureExclusion::setGranted(bool granted) {
    if (granted_ == granted)
        return;
    granted_ = granted;
    emit grantedChanged();
}

void CaptureExclusion::markResolved() {
    if (resolved_)
        return;
    resolved_ = true;
    emit resolvedChanged();
}

bool CaptureExclusion::applyAffinity(QQuickWindow* window) {
    // winId() does not create anything here — setTarget() called create() first.
    const WId handle = window->winId();
    if (handle == 0)
        return false;

    void* native = reinterpret_cast<void*>(handle);

    // The seam is consulted on every platform so the failure path stays
    // testable in a headless run. Without a hook installed the behaviour is the
    // one the Widgets overlays have: the real call on Windows, refusal
    // everywhere else.
    if (const AffinityFunction& fn = affinityOverride(); fn)
        return fn(native, kExcludeFromCapture);

#if defined(Q_OS_WIN)
    return ::SetWindowDisplayAffinity(reinterpret_cast<HWND>(native), WDA_EXCLUDEFROMCAPTURE) != FALSE;
#else
    // No capture exclusion outside Windows — refuse, and the overlay stays hidden.
    return false;
#endif
}

} // namespace exosnap::quick
