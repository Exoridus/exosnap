#include "MainWindowAffinity.h"

#include "diagnostics/AppLog.h"
#include "models/WindowPresencePolicy.h"

#include <QString>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace exosnap::quick {

namespace {

bool ApplyPlatformAffinity(void* hwnd, quint32 affinity) {
#if defined(Q_OS_WIN)
    return ::SetWindowDisplayAffinity(reinterpret_cast<HWND>(hwnd), affinity) != FALSE;
#else
    // No display affinity outside Windows. Reported as a refusal, which for this
    // class means "the window stays visible" -- the correct fallback here.
    (void)hwnd;
    (void)affinity;
    return false;
#endif
}

} // namespace

void MainWindowAffinity::setAffinityFunctionForTest(AffinityFunction fn) {
    affinity_function_ = std::move(fn);
}

void MainWindowAffinity::setExcludedFromCapture(bool excluded) {
    if (excluded_ == excluded)
        return;
    excluded_ = excluded;
    apply("setting");
}

bool MainWindowAffinity::excludedFromCapture() const noexcept {
    return excluded_;
}

void MainWindowAffinity::setHandle(void* hwnd) {
    if (hwnd_ == hwnd)
        return;
    hwnd_ = hwnd;
    if (hwnd_ == nullptr) {
        // Nothing carries the affinity any more, so nothing has it applied.
        // Reporting otherwise would claim a window is excluded when there is no
        // window at all.
        applied_ = false;
        return;
    }
    apply("handle");
}

void* MainWindowAffinity::handle() const noexcept {
    return hwnd_;
}

bool MainWindowAffinity::applied() const noexcept {
    return applied_;
}

void MainWindowAffinity::apply(const char* reason) {
    if (hwnd_ == nullptr)
        return;

    const quint32 affinity = DesiredWindowCaptureAffinity(excluded_);
    const bool ok = affinity_function_ ? affinity_function_(hwnd_, affinity) : ApplyPlatformAffinity(hwnd_, affinity);
    applied_ = ok;

    if (ok) {
        // Info rather than debug, and on both edges: "the window is excluded" and
        // "the window is no longer excluded" are the two facts a support bundle
        // needs to explain a recording that does or does not contain the shell.
        diagnostics::AppLog::info(
            QStringLiteral("shell"),
            QStringLiteral("window capture affinity %1 (%2)")
                .arg(excluded_ ? QStringLiteral("excluded") : QStringLiteral("none"), QString::fromLatin1(reason)));
        return;
    }

    // Fail-open, and the log line is the ONLY evidence of it: the window carries
    // on visible and usable, so from the outside a refused exclusion is
    // indistinguishable from the setting being off.
    diagnostics::AppLog::warning(QStringLiteral("shell"),
                                 QStringLiteral("window capture affinity REFUSED (%1) -- the window stays visible")
                                     .arg(QString::fromLatin1(reason)));
}

} // namespace exosnap::quick
