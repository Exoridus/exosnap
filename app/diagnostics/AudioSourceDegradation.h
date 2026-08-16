#pragma once

// Mid-recording honesty for an audio capture source that lost its endpoint
// (ADR 0046, product-spec "Audio source"). The engine already survives the loss:
// the affected source degrades to honest silence, the recording keeps running,
// and the source is reactivated with the same identity every 500 ms. What the
// user has to be told is that the gap exists while it is happening.
//
// The detection is NOT here and is not new. `AudioDiagnostics::source_degraded` /
// `degraded_sources` are produced by the audio threads
// (`IAudioCaptureSource::DegradedSourceCount()` -> `OnAudioSourceHealth` ->
// `PipelineDiagnosticsAggregator`) and ride the diagnostics snapshots the
// pipeline already publishes at ~5 Hz. This file owns only the part the
// Widgets frontend used to own inline in `MainWindow`: WHEN a standing
// notification is raised, replaced, or cleared from that stream.
//
// It is deliberately not level-based. Silence is legitimate content and is never
// a degraded state; the only fact that raises the notice is the pipeline
// reporting a source whose device is currently lost.
//
// PURE (no Qt, no Win32, no wall clock) so the whole latching contract is
// unit-pinned without a live recording, an audio device, or a sleep — the same
// shape as WindowCaptureStallMonitor (QCR-804).

#include <cstdint>

#include <recorder_core/pipeline_diagnostics.h>

namespace exosnap::diagnostics {

// One diagnostics snapshot reduced to the facts the latch reasons over, plus the
// session identity that keeps two recordings apart. Assembled by the caller from
// RecordingDiagnosticsSnapshot; the monitor never sees the engine type.
struct AudioDegradationSample {
    // RecordingDiagnosticsSnapshot::session_generation. A higher value is a new
    // recording (the latch is dropped); a lower one is a stale callback from a
    // finished session and is ignored outright.
    uint64_t session_generation = 0;
    // RecordingDiagnosticsSnapshot::valid — false means "idle / no data", which
    // is not a measurement of health and must never raise anything.
    bool valid = false;
    recorder_core::DiagnosticsLifecycle lifecycle = recorder_core::DiagnosticsLifecycle::Idle;
    // AudioDiagnostics::source_degraded / degraded_sources.
    bool source_degraded = false;
    uint32_t degraded_sources = 0;
};

// What the monitor asks the caller to do after one sample.
enum class AudioDegradationSignal : uint8_t {
    None,  // nothing changed — never re-announce a condition already standing
    Raise, // raise or replace the standing notice with degraded_sources()
    Clear, // every source is capturing again, or the session left the air
};

// PURE. Only a live recording can report a lost endpoint. Paused counts: the
// audio threads keep the endpoints open across a pause, so a device unplugged
// while paused is a real outage the user will hit on resume. Initializing,
// Stopping, Completed, Failed and Idle report no health at all.
[[nodiscard]] bool AudioDegradationObservable(recorder_core::DiagnosticsLifecycle lifecycle) noexcept;

// PURE. Owns the whole latching contract:
//
//   healthy                  -> nothing
//   first degraded source    -> one Raise
//   still degraded, same set -> nothing (no per-tick repeat)
//   degraded count changes   -> one Raise (the caller replaces in place)
//   every source recovers    -> one Clear
//   session ends / new one   -> one Clear, latch dropped
//   later, degraded again    -> a new Raise, counted as a second episode
//
// Not thread-safe by design: it lives on, and is only ever driven from, the Qt
// main thread — the diagnostics snapshots reach it through the coordinator's
// queued connection, so no capture-thread state is ever dereferenced here.
class AudioSourceDegradationMonitor {
  public:
    // Feed one snapshot. Returns what the caller must act on.
    [[nodiscard]] AudioDegradationSignal Observe(const AudioDegradationSample& sample) noexcept;

    // Drop the latch and the per-session episode count. Called on a fresh
    // recording start; a session-generation change does the same thing on its
    // own. The generation high-water mark is NOT dropped — see the .cpp.
    void Reset() noexcept;

    // Size of the currently standing degraded set, 0 when nothing is standing.
    // This is the count the caller puts in the notification body.
    [[nodiscard]] uint32_t degraded_sources() const noexcept {
        return degraded_sources_;
    }

    // How many independent outages were reported this session (a count change
    // within one outage is the same episode). Exposed for logging and tests.
    [[nodiscard]] uint32_t reported_episodes() const noexcept {
        return reported_episodes_;
    }

  private:
    bool have_generation_ = false;
    uint64_t generation_ = 0;
    uint32_t degraded_sources_ = 0;
    uint32_t reported_episodes_ = 0;
};

} // namespace exosnap::diagnostics
