#pragma once

#include <QtGlobal>

#include <functional>

namespace exosnap::quick {

// SetWindowDisplayAffinity for the SHELL window.
//
// WHY THIS IS NOT CaptureExclusion
// --------------------------------
// The two classes call the same Win32 function and have opposite contracts.
// CaptureExclusion serves the five overlays, which exist to be on screen while
// that screen is recorded: an overlay in the file is a corrupt recording, so a
// refused call there means the window stays hidden for the whole session and is
// never retried. Applying that rule to the shell would mean a failed Win32 call
// makes the application's only window disappear -- a far worse outcome than the
// one it is guarding against, which is that the user sees their own window in
// their own recording.
//
// So this one is fail-OPEN: a refused call leaves the window visible, leaves
// recording possible, and is logged. `applied()` reports what actually happened
// rather than what was asked for, because the difference is otherwise invisible
// -- a window that is present in a capture looks exactly like the setting being
// off.
//
// Owned by whoever owns the shell HWND (QuickWindowChrome). It holds no window
// type and no windows.h types, which is what keeps the decision testable without
// a platform window: a test installs its own affinity function and reads back
// the handle and constant it was called with.
class MainWindowAffinity {
  public:
    // `hwnd` is the native handle as an opaque pointer, `affinity` a WDA_*
    // constant (models/WindowPresencePolicy.h). Returns whether the platform
    // call succeeded.
    using AffinityFunction = std::function<bool(void* hwnd, quint32 affinity)>;

    // Replaces the platform call. An instance seam rather than the static hook
    // CaptureExclusion uses: nothing constructs this from QML, so the owner can
    // simply hand one in, and two tests can then run concurrently without
    // sharing a global.
    void setAffinityFunctionForTest(AffinityFunction fn);

    // The persisted preference. Applied immediately when a handle is known, so a
    // toggle in Settings reaches the window without waiting for anything.
    void setExcludedFromCapture(bool excluded);
    [[nodiscard]] bool excludedFromCapture() const noexcept;

    // The handle the affinity belongs to. Display affinity is per-HWND and does
    // not survive Qt destroying and recreating the native window, so a handle
    // that differs from the last one re-applies the desired state. The SAME
    // handle does nothing -- this is called from the chrome's ordinary refresh
    // path, which runs on events that are usually not recreations at all.
    void setHandle(void* hwnd);
    [[nodiscard]] void* handle() const noexcept;

    // Whether the last platform call reported success. False both before the
    // first call and after a refused one; a refusal never hides the window.
    [[nodiscard]] bool applied() const noexcept;

  private:
    // Pushes the desired affinity at the current handle. No-op without one.
    void apply(const char* reason);

    AffinityFunction affinity_function_;
    void* hwnd_ = nullptr;
    bool excluded_ = false;
    bool applied_ = false;
};

} // namespace exosnap::quick
