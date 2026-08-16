#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>
#include <cstdint>

#include "NotificationEvent.h"

namespace exosnap::notifications {

// ---------------------------------------------------------------------------
// NotificationManager
// ---------------------------------------------------------------------------
// The hub is the record; the toast is a glance at it. Every enqueued event is
// announced via eventRecorded() so the notification hub can keep the full
// history. The visible set is only the transient glance:
//
//  - A notification is TIMED when it reports something that already finished
//    (saved, update available, frames dropped, …) and STANDING when it reports
//    a condition that still holds (low storage, unexpected stop, recovery
//    available). Standing == DismissIntervalMs(type) == 0.
//  - At most one timed toast is visible; a new timed toast replaces the
//    current one and never displaces a standing one.
//  - Standing toasts stack without limit and never auto-dismiss.
//  - The timed toast, when present, is always the LAST element of
//    VisibleEvents() — the card closest to the screen anchor.
//  - PresetSwitched is recorded but never shown as a toast: the combo box that
//    performed the switch already offers the way back.
//
// No Win32 / window code here — fully unit-testable.
class NotificationManager : public QObject {
    Q_OBJECT

  public:
    // Per-type dwell durations in milliseconds. 0 means STANDING: the
    // notification reports a condition that still holds, never auto-dismisses,
    // and must carry an explicit way out beyond the ✕.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_Saved = 5000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_LowStorage = 0; // standing
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UnexpectedStop = 0; // standing
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryAvailable = 0; // standing
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UpdateAvailable = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_FramesDropped = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsRepaired = 8000;
    // Recorded in the hub only — never a toast (see class note).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_PresetSwitched = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_OverlayOmitted = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_HotkeyConflict = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsSaveFailed = 8000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_AudioSourceDegraded = 0; // standing
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_CaptureActionFailed = 8000;
    // Timed, not standing: it reports a write that already failed, matching the
    // treatment of the other completed-failure reports.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryProtectionUnavailable = 8000;
    // Timed, like its sibling SettingsRepaired: the load already happened and
    // there is no action the toast could offer that the Settings page does not
    // already provide. The hub keeps the record for the whole session.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsLoadFailed = 8000;
    // Standing: it reports a condition that still holds while the recording runs.
    // The composition root dismisses it the moment capture frames resume, and
    // again when the session ends — the body says "the recording is still
    // running", which stops being true then.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_WindowCaptureStalled = 0; // standing

    explicit NotificationManager(QObject* parent = nullptr);

    // Enqueue a new notification event (Qt main thread only). Always emits
    // eventRecorded(); shows a toast unless the type is record-only
    // (PresetSwitched) or toasts are disabled. Returns the sequence number
    // assigned to this event — the stable identity a caller needs to later
    // Dismiss() a standing toast it raised programmatically (e.g. when the
    // condition it reports clears on its own, not via a user click).
    uint64_t Enqueue(NotificationEvent event);

    // Manually dismiss a visible event by its sequence number.
    // No-op when the sequence is not currently visible.
    void Dismiss(uint64_t sequence);

    // Master toast switch (the "Show notifications" setting). When disabled no
    // toast becomes visible, but every event is still recorded to the hub.
    void SetToastsEnabled(bool enabled);

    // Returns the currently visible events. Standing toasts first (insertion
    // order), the single timed toast — if any — last.
    [[nodiscard]] const QVector<NotificationEvent>& VisibleEvents() const noexcept;

    // Returns the auto-dismiss interval (ms) for the given type, or 0 for
    // standing types. Public so the toast window can drive its countdown bar
    // from the same per-type timings the manager uses.
    [[nodiscard]] static int DismissIntervalMs(NotificationType type) noexcept;

    // True when the type reports a condition that still holds: it never
    // auto-dismisses and stacks instead of being replaced.
    [[nodiscard]] static bool IsStanding(NotificationType type) noexcept;

    // Returns the qt-monotonic timestamp (ms since epoch) at which the visible
    // event with the given sequence was promoted into a slot, or -1 if it is not
    // currently visible. Lets the toast window compute remaining dwell fraction.
    [[nodiscard]] qint64 ShownAtMs(uint64_t sequence) const noexcept;

  signals:
    // Emitted exactly once per Enqueue(), after the sequence is assigned and
    // regardless of whether a toast is shown. The hub listens here.
    void eventRecorded(const exosnap::notifications::NotificationEvent& event);

    // Emitted when the visible set changes. Receivers (toast window) should
    // re-render based on VisibleEvents().
    void visibleSetChanged();

    // Emitted when an actionable event (hasAction() == true) becomes visible.
    // Receivers (tray badge) increment their unread count on this signal.
    void actionableEventShown();

  private:
    // Schedule (or cancel) the auto-dismiss timer for the next soonest expiry
    // among all visible events.
    void rescheduleTimer();

    // Timer fires → dismiss all visible events that have exceeded their duration.
    void onTimerFired();

    // Standing toasts first, then at most one timed toast as the last element.
    QVector<NotificationEvent> visible_;

    // Monotonic counter for stable event identity.
    uint64_t next_sequence_ = 1;

    // Timestamps when each visible event was shown (ms since epoch; qt monotonic).
    QVector<qint64> visible_shown_at_;

    // Single-shot timer that fires when the soonest auto-dismiss is due.
    QTimer* timer_ = nullptr;

    bool toasts_enabled_ = true;
};

} // namespace exosnap::notifications
