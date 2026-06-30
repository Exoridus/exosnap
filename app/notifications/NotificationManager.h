#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>
#include <cstdint>
#include <deque>

#include "NotificationEvent.h"

namespace exosnap::notifications {

// ---------------------------------------------------------------------------
// NotificationManager — NOTIFY-TOASTS-R1
// ---------------------------------------------------------------------------
// Pure-ish QObject queue manager for transient notification toasts. Drives the
// lifecycle of NotificationEvents independently of any window implementation.
//
// Design rules:
//  - FIFO queue; at most kMaxVisible events shown concurrently.
//  - Per-type auto-dismiss timers (PLACEHOLDERS — final timings from NOTIFY-DESIGN-R1).
//  - Sticky types (UnexpectedStop, RecoveryAvailable) do not auto-dismiss.
//  - When a visible slot frees up the front of the queue is promoted.
//  - No Win32 / window code here — fully unit-testable.
//
// Wiring:
//  - Call Enqueue() for each new notification event.
//  - Connect toastShowRequested() → NotificationToastWindow::showEvent().
//  - Connect toastHideRequested() → NotificationToastWindow::hideEvent().
//
class NotificationManager : public QObject {
    Q_OBJECT

  public:
    // Maximum concurrently visible toasts.
    static constexpr int kMaxVisible = 3;

    // Per-type dwell durations in milliseconds — exact from Mappe spec (NOTIFY-SKIN-R1).
    // Sticky types use 0 (no auto-dismiss).
    // success / "Recording saved" — auto-dismiss 5 s (glanceable; file already written).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_Saved = 5000;
    // caution / "Storage running low" — sticky (demands a decision before space runs out).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_LowStorage = 0; // sticky
    // error / "Recording stopped unexpectedly" — sticky (failure; never vanish before seen).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UnexpectedStop = 0; // sticky
    // info / "Recover last session?" — sticky (pending choice on relaunch).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryAvailable = 0; // sticky
    // info / "Update available" — auto-dismiss 8 s (non-urgent; the card stays in Settings).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UpdateAvailable = 8000;
    // caution / "Frames dropped" — auto-dismiss 8 s (informational; the full per-stage
    // drop breakdown lives on the Diagnostics page, not in the toast). DROP-NOTIFY.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_FramesDropped = 8000;

    explicit NotificationManager(QObject* parent = nullptr);

    // Enqueue a new notification event. Thread-safe with respect to the Qt
    // event loop (must be called on the owning thread / Qt main thread).
    void Enqueue(NotificationEvent event);

    // Manually dismiss a visible event by its sequence number.
    // No-op when the sequence is not currently visible.
    void Dismiss(uint64_t sequence);

    // Returns the currently visible events (ordered front-to-back).
    [[nodiscard]] const QVector<NotificationEvent>& VisibleEvents() const noexcept;

    // Returns the number of events in the pending queue (not yet visible).
    [[nodiscard]] int PendingCount() const noexcept;

    // Returns the auto-dismiss interval (ms) for the given type, or 0 for sticky
    // types. Public so the toast window can drive its countdown bar from the same
    // per-type timings the manager uses to schedule auto-dismiss.
    [[nodiscard]] static int DismissIntervalMs(NotificationType type) noexcept;

    // Returns the qt-monotonic timestamp (ms since epoch) at which the visible
    // event with the given sequence was promoted into a slot, or -1 if it is not
    // currently visible. Lets the toast window compute remaining dwell fraction.
    [[nodiscard]] qint64 ShownAtMs(uint64_t sequence) const noexcept;

  signals:
    // Emitted when the visible set changes. Receivers (toast window) should
    // re-render based on VisibleEvents().
    void visibleSetChanged();

    // Emitted when an actionable event (hasAction() == true) becomes visible.
    // Receivers (tray badge) increment their unread count on this signal.
    void actionableEventShown();

  private:
    // Pull events from the queue into visible slots until kMaxVisible or queue empty.
    void drainQueue();

    // Schedule (or cancel) the auto-dismiss timer for the next soonest expiry
    // among all visible events.
    void rescheduleTimer();

    // Timer fires → dismiss all visible events that have exceeded their duration.
    void onTimerFired();

    std::deque<NotificationEvent> pending_queue_;
    QVector<NotificationEvent> visible_;

    // Monotonic counter for stable event identity.
    uint64_t next_sequence_ = 1;

    // Timestamps when each visible event was shown (ms since epoch; qt monotonic).
    QVector<qint64> visible_shown_at_;

    // Single-shot timer that fires when the soonest auto-dismiss is due.
    QTimer* timer_ = nullptr;
};

} // namespace exosnap::notifications
