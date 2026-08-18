#pragma once

// The non-deterministic Windows boundary of the present session -- and nothing else.
//
// This seam exists because of a specific failure. PresentMonEtwSession used to be one
// translation unit split by `#ifdef EXOSNAP_HAS_PRESENTMON`: the real ETW consumer on
// one side, a no-op on the other. `present_provider_tests` compiled the no-op side, so
// every contract the session owns -- attribution boundaries, accumulator resets, the
// process handle, what `available` means -- was verified against an implementation that
// did nothing, while the shipping side was verified by nobody. That is the same shape
// as the original ADR 0033 defect (the sources were compiled ONLY by a test target and
// the product carried the no-op), and finding it twice is what turned it into a seam.
//
// The split is deliberately drawn at what a test genuinely cannot reproduce -- opening
// a real-time ETW session needs elevation, and `ProcessTrace` blocks on the kernel --
// and NOT one line further. Everything above this interface is ordinary logic and lives
// in PresentMonEtwSession, compiled identically for the product and for the tests.
//
// This is not a second PresentMon. `MakePresentTraceBackend()` has exactly one real
// implementation; without the vendored consumer it returns nullptr, which is the same
// graceful degrade the old no-op branch provided.

#include <cstdint>
#include <memory>
#include <vector>

namespace exosnap::diagnostics {

// One completed present, in OUR vocabulary. The real backend maps PresentMon's
// `PresentEvent` onto this; a test backend produces them directly. Deliberately raw:
// classification, the inter-present interval and the accumulators are the session's
// job, so a backend cannot get them subtly different from the shipping path.
struct TracePresentEvent {
    unsigned long process_id = 0;
    uint64_t present_qpc = 0;  // QPC ticks, in TimestampFrequency() units
    int present_mode_code = 0; // PresentMon's PresentMode enum, as an integer
    int sync_interval = 1;     // DXGI present sync interval; 0 == tearing-capable
    bool tearing_flag = false;
    bool discarded = false; // the compositor discarded this present
};

class IPresentTraceBackend {
  public:
    virtual ~IPresentTraceBackend() = default;

    // Opens the real-time trace. `false` is the ordinary answer on an unelevated
    // process (ERROR_ACCESS_DENIED), never an error worth reporting to a user.
    [[nodiscard]] virtual bool Open() = 0;

    // Consumes until Close() unblocks it. Runs on the session's consumer thread and
    // must block for the whole life of the trace -- returning early is precisely the
    // event the session reports as "the trace ended without a stop request".
    virtual void Consume() = 0;

    // Unblocks Consume(). Called from another thread, and safe to call more than once.
    virtual void Close() = 0;

    // QPC ticks per second. Valid after Open(); 0 means "not known", and the session
    // then reports no interval rather than a fabricated one.
    [[nodiscard]] virtual int64_t TimestampFrequency() const = 0;

    // Everything decoded since the last call, oldest first. Called from the reader
    // side; the real implementation is thread-safe against Consume().
    [[nodiscard]] virtual std::vector<TracePresentEvent> Drain() = 0;
};

// The one real backend, or nullptr when the vendored PresentMon consumer is not part
// of this build. A null backend makes PresentMonEtwSession::Start() return false and
// everything downstream report unavailable -- truthfully, and with no second code path.
[[nodiscard]] std::shared_ptr<IPresentTraceBackend> MakePresentTraceBackend();

} // namespace exosnap::diagnostics
