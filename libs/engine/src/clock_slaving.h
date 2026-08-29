#pragma once

// ClockSlavingController — the pure control law behind gentle A/V clock slaving.
//
// A/V drift arises because video is paced on the QPC timeline while audio PTS
// ticks with the sound device's own crystal (see audio_clock_drift.h). Once the
// measured drift crosses an engage threshold, this controller nudges the audio
// output timeline back onto the QPC axis by asking the resampler for a tiny,
// sub-audible rate change (ppm) — never a sample drop/insert, never a PTS jump.
//
// Measured feed-forward plus a proportional term on the residual, with an engage
// latch. Still not a PI controller: the standing rate comes from the device's own
// measured rate error, not from an integral, so there is no windup state to
// unwind and nothing to tune for it.
//
// The feed-forward term is what removes the ramp. A P-only controller reacts to
// the error a rate difference has already produced, so it can do nothing until
// that error is large enough to act on: at 30 ppm the drift climbs for eight
// minutes before crossing the engage threshold, and every clip cut out of those
// eight minutes carries the offset. The rate itself is measurable long before
// that -- drift divided by elapsed -- and applying it directly holds the drift
// near zero from the moment the estimate is trustworthy.
//
// The P term still earns its place: feed-forward alone freezes the residual
// wherever engagement found it (A tracks D, so D - A stops moving). Together the
// fixed point is residual = 0, which a P-only design could not reach -- it parked
// at R_ss = r * kControlHorizonS / 1000.
//
// The latch (engage once, never disengage) makes the behaviour monotone and
// explainable instead of a sawtooth of engage/disengage.
//
// Sign convention (consistent across the three layers estimator -> controller ->
// swresample):
//   - drift_ms D: positive = audio device clock runs slow vs QPC => audio events
//     land on earlier PTS => audio leads video (AudioClockDriftEstimator).
//   - applied_ms A: positive = the output timeline was stretched (more output
//     frames per input; events pushed later) — corrects a positive D. Measured
//     from the decorator's real frame accounting, not from an integral of p.
//   - residual R = D - A: the misalignment that actually lands in the file.
//   - ppm p: positive = stretch the output (more output frames per input) =>
//     corrects a positive residual. A sign inversion anywhere here would DOUBLE
//     the drift instead of correcting it, so the mapping is pinned by tests.
//
// Pure and hardware-free (no Qt, no FFmpeg, no I/O); unit-tested with a synthetic
// clock and a modelled plant.

#include <cmath>
#include <cstdint>

namespace exosnap::engine {

// Largest |drift| a pair of real clocks can have produced over `elapsed_s`.
// Anything beyond it is a broken observation rather than a fast crystal, and
// both the controller and the diagnostics path have to agree on that -- a drift
// figure that is reported as a measurement on one path and rejected on the other
// is worse than either answer alone. The constants live in the controller
// because they are the correction envelope it is built around.
[[nodiscard]] double PlausibleDriftBoundMs(double elapsed_s) noexcept;

class ClockSlavingController {
  public:
    // --- Fixed control parameters (constants, not settings) ---------------
    // There is no meaningful user choice between "14 ms" and "16 ms"; exposing
    // these as knobs would be pseudo-control. The only user switch is slaving
    // on/off (an expert opt-out for bit-exact capture), handled by the caller.

    // Engage once |D| exceeds this. Well above estimator jitter (the drift window
    // smooths ~1.3 s) and well below the ~45 ms lip-sync perception threshold.
    static constexpr double kEngageThresholdMs = 15.0;
    // Correct the residual over this horizon: 15 ms residual -> 250 ppm; a gentle
    // pull over a minute rather than a jerk.
    static constexpr double kControlHorizonS = 60.0;
    // Rate cap: <= 0.87 cent pitch shift (JND ~5 cent) — inaudible; covers > 5x
    // typical crystal tolerance.
    static constexpr double kMaxPpm = 500.0;
    // Slew cap: the rate reaches kMaxPpm no sooner than 4 s, so there is no
    // audible rate step.
    static constexpr double kMaxSlewPpmPerS = 125.0;
    // Update cadence: the estimator window (~1.3 s) ~ this period << the horizon
    // (60 s), which keeps the loop stable and non-oscillatory.
    static constexpr double kUpdatePeriodS = 1.0;
    // Below this the swr adjustment is meaningless noise; quantizing avoids
    // perpetual tiny restamps and log/diag churn.
    static constexpr double kMinPpmStep = 10.0;
    // Plausibility bound on the measurement itself, as a multiple of the largest
    // rate error this controller can correct. Two clocks cannot diverge faster
    // than their rate difference allows, so a drift beyond kMaxPpm * this factor
    // over the elapsed span did not come from a crystal -- it came from a broken
    // observation (a device position that stopped advancing, a stale baseline).
    // Slaving on such a number would drive the resampler from something that
    // describes nothing.
    static constexpr double kImplausibleRateFactor = 10.0;
    // Floor for that bound, so an ordinary fixed offset early in a session is
    // never mistaken for a fault. Far above the ~45 ms lip-sync threshold, far
    // below any value a real device produces.
    static constexpr double kImplausibleFloorMs = 1000.0;
    // How long the rate is measured over. At the engage-relevant rates a minute
    // of accumulation is several ms against an estimator that smooths ~1.3 s, so
    // the slope is signal rather than jitter.
    static constexpr double kRateEstimateMinS = 60.0;
    // Drift at this point is the reference the slope is measured FROM. Late
    // enough that the device has settled and the estimator's window has filled,
    // early enough not to spend the measurement window on it.
    static constexpr double kRateReferenceAtS = 5.0;
    // Any non-zero rate crosses the engage threshold eventually, so "should this
    // device be corrected" is a question about a horizon, not about a rate. A
    // device whose measured rate would carry it past kEngageThresholdMs within
    // this long is corrected now instead of after the error has accumulated;
    // one that would not is left in its byte-identical passthrough.
    static constexpr double kProjectionHorizonS = 600.0;

    ClockSlavingController() = default;

    // Feed the latest measured drift D and the decorator's actually-applied
    // compensation A (both ms) plus the observation's QPC time (ns, the update
    // clock). Returns true when a new ppm value should be pushed to the decorator
    // (the caller then reads Ppm()). Called per capture packet; the 1 Hz update
    // gating lives here, driven by qpc_now_ns.
    bool Update(double drift_ms, double applied_ms, uint64_t qpc_now_ns) noexcept {
        residual_ms_ = drift_ms - applied_ms;

        if (!has_first_) {
            has_first_ = true;
            first_qpc_ns_ = qpc_now_ns;
        }

        // Plausibility gate, evaluated BEFORE the engage latch on purpose: the
        // latch is permanent, so a single impossible sample would otherwise arm
        // slaving for the whole session on a measurement fault. A faulted
        // measurement is latched too, because a metric that silently returns to
        // green hides the fault from every report that reads it afterwards.
        const double elapsed_s = static_cast<double>(qpc_now_ns - first_qpc_ns_) / 1e9;
        if (std::abs(drift_ms) > PlausibleDriftBoundMs(elapsed_s)) {
            measurement_faulted_ = true;
            return false;
        }

        // The device's own rate error, in ppm, as the SLOPE of the drift from a
        // reference sample -- never as drift/elapsed, which would read a constant
        // offset as a rate that merely decays. A fixed head start between the two
        // clocks is not a rate difference and must not be corrected as one.
        //
        // Unaffected by anything this controller has done: resampling shifts
        // neither the device-position axis nor the QPC axis, so the estimator's
        // drift stays a measurement of the two clocks and not of the correction.
        if (!has_rate_reference_ && elapsed_s >= kRateReferenceAtS) {
            has_rate_reference_ = true;
            rate_reference_ms_ = drift_ms;
            rate_reference_s_ = elapsed_s;
        }
        double rate_ppm = 0.0;
        if (has_rate_reference_) {
            const double span_s = elapsed_s - rate_reference_s_;
            if (span_s >= kRateEstimateMinS) {
                rate_ppm = (drift_ms - rate_reference_ms_) * 1000.0 / span_s;
            }
        }

        // Engage latch: once crossed, stays engaged for the rest of the session.
        // Either the error is already worth correcting, or the measured rate says
        // it will be within kProjectionHorizonS.
        if (!engaged_ && (std::abs(drift_ms) > kEngageThresholdMs ||
                          std::abs(rate_ppm) * kProjectionHorizonS / 1000.0 > kEngageThresholdMs)) {
            engaged_ = true;
        }
        if (!engaged_) {
            return false;
        }

        double dt_s;
        if (!has_eval_) {
            has_eval_ = true;
            last_eval_ns_ = qpc_now_ns;
            // First engaged evaluation: no interval history yet. Use the nominal
            // period so the first step is slew-limited like every other.
            dt_s = kUpdatePeriodS;
        } else {
            dt_s = static_cast<double>(qpc_now_ns - last_eval_ns_) / 1e9;
            if (dt_s < kUpdatePeriodS) {
                return false; // not time to re-evaluate yet
            }
            last_eval_ns_ = qpc_now_ns;
        }

        // Feed-forward + proportional. The first holds the rate, the second
        // closes whatever misalignment is already on the timeline.
        double p_target = rate_ppm + residual_ms_ / kControlHorizonS * 1000.0;
        if (p_target > kMaxPpm) {
            p_target = kMaxPpm;
        } else if (p_target < -kMaxPpm) {
            p_target = -kMaxPpm;
        }

        const double slew = kMaxSlewPpmPerS * dt_s;
        double delta = p_target - ppm_;
        if (delta > slew) {
            delta = slew;
        } else if (delta < -slew) {
            delta = -slew;
        }
        const double p_next = ppm_ + delta;

        // Quantize: skip sub-step nudges (steady state, or a residual already
        // parked). p stays where it is — no restamp, no diagnostics churn.
        if (std::abs(p_next - ppm_) < kMinPpmStep) {
            return false;
        }
        ppm_ = p_next;
        return true;
    }

    // Current compensation rate in ppm (>0 stretches the output). 0 until engaged
    // and past the first meaningful step.
    [[nodiscard]] double Ppm() const noexcept {
        return ppm_;
    }

    // True once the engage threshold has been crossed (latched for the session).
    [[nodiscard]] bool Engaged() const noexcept {
        return engaged_;
    }

    // True once a drift value arrived that no pair of clocks could produce
    // (latched). The controller ignored it; a reader must treat this track's
    // drift figures as invalid rather than as a measurement that happens to be
    // large.
    [[nodiscard]] bool MeasurementFaulted() const noexcept {
        return measurement_faulted_;
    }

    // Latest residual D - A (ms) — the misalignment that actually lands in the
    // file, refreshed on every Update() call regardless of the update gate.
    [[nodiscard]] double ResidualMs() const noexcept {
        return residual_ms_;
    }

  private:
    bool engaged_ = false;
    bool measurement_faulted_ = false;
    bool has_first_ = false;
    uint64_t first_qpc_ns_ = 0;
    bool has_rate_reference_ = false;
    double rate_reference_ms_ = 0.0;
    double rate_reference_s_ = 0.0;
    bool has_eval_ = false;
    uint64_t last_eval_ns_ = 0;
    double ppm_ = 0.0;
    double residual_ms_ = 0.0;
};

[[nodiscard]] inline double PlausibleDriftBoundMs(double elapsed_s) noexcept {
    const double rate_bound =
        ClockSlavingController::kMaxPpm * ClockSlavingController::kImplausibleRateFactor * elapsed_s / 1000.0;
    // Not std::max: this header is included from translation units that pull in
    // windows.h without NOMINMAX, where `max` is a function-like macro and the
    // call would expand into a parse error.
    return rate_bound > ClockSlavingController::kImplausibleFloorMs ? rate_bound
                                                                    : ClockSlavingController::kImplausibleFloorMs;
}

} // namespace exosnap::engine
