#pragma once

#include <windows.h>

#include <atomic>
#include <memory>

// Notification that one display's advanced colour state changed -- its HDR mode,
// its colour kind, or the white level Windows composes SDR content at.
//
// Every consumer of those facts (see display_color_recheck.h) otherwise finds out
// by re-reading them on a timer. This turns that into a signal, and keeps a
// backstop underneath it, because the notification has three structural limits
// the caller must not have to reason about:
//
//   * It needs Windows 11 build 22621 (IDisplayInformationStaticsInterop). Older
//     Windows has no equivalent for a process without a CoreWindow.
//   * It is scoped to one monitor handle. A capture that reopens after a
//     hot-plug comes back on a new HMONITOR, so the subscription has to follow.
//   * Registration snaps the DispatcherQueue of the registering thread, and the
//     handler only runs while that thread pumps. No capture or encode thread
//     does, which is why this owns a thread rather than borrowing one.
//
// Nothing about a failure is the caller's problem: Watch() cannot fail, Signalled()
// simply never becomes true, and the backstop keeps the old cadence. Notified()
// exists so a log line can say which of the two is in force rather than leaving
// it to be inferred.
//
// Threading: Watch/Stop/Signalled/Notified are callable from one owning thread.
// The notification itself runs on the thread this class owns.

namespace exosnap::engine {

class DisplayColorWatch {
  public:
    DisplayColorWatch();
    ~DisplayColorWatch();

    DisplayColorWatch(const DisplayColorWatch&) = delete;
    DisplayColorWatch& operator=(const DisplayColorWatch&) = delete;

    // Watch `monitor`, replacing whatever was watched before. A null handle stops
    // watching without tearing the worker down, which is what a capture between
    // two monitors needs.
    void Watch(HMONITOR monitor);

    // Stops the worker and unsubscribes. Idempotent; the destructor calls it.
    void Stop() noexcept;

    // True when a change has been signalled since the last call, and clears it.
    // A burst of notifications for one change collapses into a single true --
    // Windows raises several, and the caller re-reads the facts either way.
    [[nodiscard]] bool TakeSignalled() noexcept {
        return signalled_.exchange(false, std::memory_order_acquire);
    }

    // Whether a subscription is currently live. False before the worker has
    // registered, on Windows without the interop, and while no monitor is set.
    [[nodiscard]] bool Notified() const noexcept {
        return notified_.load(std::memory_order_relaxed);
    }

  private:
    struct Worker;

    std::atomic<bool> signalled_{false};
    std::atomic<bool> notified_{false};
    std::unique_ptr<Worker> worker_;
};

} // namespace exosnap::engine
