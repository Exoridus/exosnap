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
//  - A notification is TIMED when it reports an EVENT that already happened
//    (saved, update available, frames dropped, recording stopped unexpectedly, …)
//    and STANDING when it reports a CONDITION that is true right now and that
//    will clear itself when it stops being true (low storage, audio source lost,
//    capture stalled). Standing == DismissIntervalMs(type) == 0.
//    Timed comes in two dwells — see the constants below.
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
    // Per-type dwell durations in milliseconds. There are exactly three values, and
    // which one a type gets follows one question: WHAT IS THE NOTIFICATION ABOUT?
    //
    //   kDwellBrief (5 s)   — an event that already happened and asks nothing of the
    //                         user. A glance is the whole interaction.
    //   kDwellAction (10 s) — an event that already happened and offers a way to act
    //                         on it, or reports a problem worth noticing. Long enough
    //                         to read the body, decide, and reach the button --
    //                         including when the user is still alt-tabbing back from
    //                         whatever was being recorded.
    //   0 == STANDING       — a CONDITION that is true right now and will CLEAR
    //                         ITSELF when it stops being true. Never auto-dismisses,
    //                         and must carry an explicit way out beyond the ✕.
    //
    // That last line is the rule the standing set is drawn by, and it is narrower
    // than it used to be. UnexpectedStop and RecoveryAvailable were standing, but
    // neither is a condition: they describe an event that is over and that nothing
    // will ever come along and clear, so they stood forever. Both are now timed --
    // and neither loses anything by it, because the hub keeps every entry and the
    // recovery surface offers itself again at startup.
    //
    // Nothing here is longer than 10 s on purpose. Past that a toast starts reading
    // as standing, the user learns that toasts get stuck, and the reflex to dismiss
    // them unread is exactly what costs the real standing notices their effect.

    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDwellBrief = 5000;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDwellAction = 10000;

    // --- Standing: a live condition that clears itself -----------------------
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_LowStorage = 0; // standing: the drive is still full
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_AudioSourceDegraded = 0; // standing: the source is still gone
    // Standing: it reports a condition that still holds while the recording runs.
    // The composition root dismisses it the moment capture frames resume, and
    // again when the session ends — the body says "the recording is still
    // running", which stops being true then.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_WindowCaptureStalled = 0; // standing

    // --- Actionable / problem ------------------------------------------------
    // Two actions (Edit, Show in folder) and a filename to read, shown the moment a
    // recording ends -- when the user is most likely to still be looking elsewhere.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_Saved = kDwellAction;
    // Brief despite carrying an action, unlike Saved above. A frame capture is
    // something the user pressed a moment ago and is still looking at the app
    // for; Saved arrives when a recording ends, which is exactly when attention
    // has already moved elsewhere.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_FrameCaptured = kDwellBrief;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UnexpectedStop = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryAvailable = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_UpdateAvailable = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_FramesDropped = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_HotkeyConflict = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsSaveFailed = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_CaptureActionFailed = kDwellAction;
    // Timed, not standing: it reports a write that already failed, matching the
    // treatment of the other completed-failure reports.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_RecoveryProtectionUnavailable = kDwellAction;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsLoadFailed = kDwellAction;

    // --- Brief: happened, nothing to do --------------------------------------
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_SettingsRepaired = kDwellBrief;
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_OverlayOmitted = kDwellBrief;
    // Recorded in the hub only — never a toast (see class note).
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr int kDismissMs_PresetSwitched = kDwellBrief;

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

    // ---- test seams ------------------------------------------------------
    // Runs the dismiss handler directly. Called before anything has expired it
    // reproduces an early wake-up, which a coarse timer delivers routinely and
    // which must leave the timer armed rather than disarmed.
    void FireDismissTimerForTest();
    [[nodiscard]] bool DismissTimerArmedForTest() const;

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
