#pragma once

// Per-monitor DPI awareness for the duration of a scope, restored on every exit.
//
// The engine is deliberately not per-monitor aware: changing that would move every
// Win32 geometry call in the process at once -- window picking, capture bounds,
// crop, preview. What needs it is one pair of reads, and only because they have to
// agree with each other. A pointer position and the window bounds it is subtracted
// from are virtualised independently, so reading them in different awareness
// contexts puts the result in neither.
//
// Internal to the engine. It is a header only so a test can assert the one
// property that matters here: the thread's context afterwards is the one it had
// before, because a scope that leaks would quietly make the whole thread aware.

#include <windows.h>

namespace exosnap::engine {

class ScopedThreadDpiAwareness {
  public:
    explicit ScopedThreadDpiAwareness(DPI_AWARENESS_CONTEXT next) noexcept
        : previous_(SetThreadDpiAwarenessContext(next)) {
    }

    ~ScopedThreadDpiAwareness() {
        if (previous_ != nullptr) {
            SetThreadDpiAwarenessContext(previous_);
        }
    }

    ScopedThreadDpiAwareness(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness& operator=(const ScopedThreadDpiAwareness&) = delete;
    ScopedThreadDpiAwareness(ScopedThreadDpiAwareness&&) = delete;
    ScopedThreadDpiAwareness& operator=(ScopedThreadDpiAwareness&&) = delete;

    // False when Windows refused the context. A caller must then skip the work the
    // scope was for rather than do it in mixed spaces, which is the defect the
    // scope exists to prevent.
    [[nodiscard]] bool active() const noexcept {
        return previous_ != nullptr;
    }

  private:
    DPI_AWARENESS_CONTEXT previous_{};
};

} // namespace exosnap::engine
