#pragma once

// The native ETW boundary of the present session.
// Opening a real-time session requires elevation and ProcessTrace blocks on
// the kernel. Keep only those operations behind this interface. Attribution,
// lifetime, reset and availability logic stay in PresentMonEtwSession and are
// compiled identically for product and tests. Testing a no-op substitute for
// that logic would not verify the shipped implementation.
// MakePresentTraceBackend has one real implementation; without the vendored
// consumer it returns nullptr and the caller reports unavailable.

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
    uint64_t hwnd = 0;
    std::vector<unsigned long> related_process_ids;
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
