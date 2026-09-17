#pragma once

#include <chrono>

// When a session should re-read the colour facts of the display it is capturing.
//
// Those facts -- whether the display is in HDR, its peak luminance, and the white
// level Windows composes SDR content at -- decide the tone-map knee, the overlay
// reference white and the preview transform. Windows changes the SDR content
// brightness whenever the user moves a slider, and that is not a mode change, so
// nothing reopens and nothing fails: a consumer holding the old value simply
// draws every later frame at the wrong brightness.
//
// Two mechanisms answer the same question, and the gate below is what lets them
// share one call site:
//
//   * A notification (DisplayColorWatch) when the OS offers one. It is the whole
//     point -- it turns "up to one interval late" into "as soon as Windows knows"
//     -- but it is not available everywhere, it is scoped to one monitor, and a
//     session can outlive the monitor handle it was registered for.
//   * A periodic re-read, which needs nothing and cannot miss anything.
//
// The periodic re-read therefore stays in both cases and only changes cadence:
// it is the primary trigger without a notification and a backstop with one. That
// is the reason this is a gate rather than a timer -- the decision is "is a
// re-read owed", not "has the interval elapsed".
//
// Pure and clock-injected, so the cadence is pinned without waiting in real time.

namespace exosnap::engine {

// Re-read cadence without a notification. Two seconds is a compromise that has
// no better answer: the cost is two cheap queries, and the exposure error lasts
// until the next one.
inline constexpr std::chrono::milliseconds kDisplayColorPolledBackstop{2000};

// Re-read cadence with a notification. Long, because it is no longer how a change
// is found -- it only has to cover the gaps the notification structurally cannot:
// the moments around a monitor change, when the subscription still names the
// display the session has just stopped capturing.
inline constexpr std::chrono::milliseconds kDisplayColorNotifiedBackstop{30000};

class DisplayColorRecheckGate {
  public:
    // Arms the backstop. `notified` says whether a notification is live; the first
    // backstop re-read is due one interval from here, not immediately, because the
    // caller has just read the facts to start the session with.
    void Start(std::chrono::steady_clock::time_point now, bool notified) noexcept {
        last_read_ = now;
        notified_ = notified;
    }

    // Called when the subscription comes or goes mid-session.
    //
    // A read that was already owed under the old cadence stays owed under the new
    // one. Without that, taking the longer backstop would silently defer it by the
    // difference between the two -- and the moment this transition happens is
    // exactly the moment a re-read matters most, because the session has just
    // changed monitor or just found out it can be notified at all.
    void SetNotified(std::chrono::steady_clock::time_point now, bool notified) noexcept {
        if (notified_ == notified) {
            return;
        }
        const bool was_due = now - last_read_ >= Backstop();
        notified_ = notified;
        if (was_due) {
            last_read_ = now - Backstop();
        }
    }

    [[nodiscard]] bool Notified() const noexcept {
        return notified_;
    }

    [[nodiscard]] std::chrono::milliseconds Backstop() const noexcept {
        return notified_ ? kDisplayColorNotifiedBackstop : kDisplayColorPolledBackstop;
    }

    // True when the caller should re-read the display's facts now, and records
    // that it did. `signalled` is a notification the caller has picked up since
    // the last call; it is consumed here rather than deferred to the next
    // backstop, which is the entire latency the notification buys.
    //
    // A signal does not reset the backstop's phase any differently from a
    // backstop read: both are a read, and the next one is due an interval later.
    [[nodiscard]] bool TakeDue(std::chrono::steady_clock::time_point now, bool signalled) noexcept {
        if (!signalled && now - last_read_ < Backstop()) {
            return false;
        }
        last_read_ = now;
        return true;
    }

  private:
    std::chrono::steady_clock::time_point last_read_{};
    bool notified_ = false;
};

} // namespace exosnap::engine
