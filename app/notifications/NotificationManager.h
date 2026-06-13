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

    // PLACEHOLDER dismiss durations in milliseconds.
    // Final per-type timings are deferred to NOTIFY-DESIGN-R1.
    // Sticky types use 0 (no auto-dismiss).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_Saved = 5000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_LowStorage = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UnexpectedStop = 0; // sticky
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryAvailable = 0; // sticky

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

  signals:
    // Emitted when the visible set changes. Receivers (toast window) should
    // re-render based on VisibleEvents().
    void visibleSetChanged();

  private:
    // Returns the auto-dismiss interval for the given type.
    // Returns 0 for sticky types.
    [[nodiscard]] static int DismissIntervalMs(NotificationType type) noexcept;

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
