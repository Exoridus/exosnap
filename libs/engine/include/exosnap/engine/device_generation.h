#pragma once

// Which D3D11 device a GPU resource, a producer, or a piece of evidence belongs
// to.
//
// A shared texture, a keyed mutex, a duplication, an accumulated measurement:
// each is valid only on the device it was created against. When that device is
// replaced -- a DEVICE_REMOVED, a hot-plug that moves the output to another
// adapter, an adapter-matched reopen -- everything derived from the old one is
// dead, whatever it still looks like.
//
// What it looks like is the problem this exists to solve. The properties a
// consumer naturally compares -- width, height, DXGI_FORMAT, HDR facts -- are
// all unchanged across a reopen onto an equivalent display, so a cache keyed on
// those alone reuses a surface belonging to a device that no longer exists. The
// pointer is no help either: the allocator is free to hand the same address back
// for the new device.
//
// So identity is a number the owner bumps, once, every time it creates a device,
// and every dependent records the generation it was built for. The rule, for
// every consumer:
//
//     generation changed
//     -> the resource is invalid, not stale-but-usable
//     -> the producer is invalid
//     -> evidence derived from it is invalid
//     -> rebuild on the new device, and only then resume publishing
//
// An honest "unavailable" during that rebuild is correct. An old snapshot that
// reads like a fresh measurement is not.

#include <cstdint>

namespace exosnap::engine {

// A device's identity for the lifetime of the process. Monotonic so "newer" is
// answerable, and comparable so "same device" is answerable without holding a
// reference to the device itself.
//
// kNoDevice is the value before any device exists and after one is released. It
// never compares equal to a real generation, so a dependent that was built
// without a device is always rebuilt rather than mistaken for current.
class DeviceGeneration {
  public:
    static constexpr uint64_t kNoDevice = 0;

    constexpr DeviceGeneration() noexcept = default;
    constexpr explicit DeviceGeneration(uint64_t value) noexcept : value_(value) {
    }

    [[nodiscard]] constexpr uint64_t value() const noexcept {
        return value_;
    }
    [[nodiscard]] constexpr bool Valid() const noexcept {
        return value_ != kNoDevice;
    }

    friend constexpr bool operator==(DeviceGeneration a, DeviceGeneration b) noexcept {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(DeviceGeneration a, DeviceGeneration b) noexcept {
        return !(a == b);
    }

  private:
    uint64_t value_ = kNoDevice;
};

// Hands out generations to the device owners in one process.
//
// Process-wide rather than per-owner: two producers must never be able to mint
// the same number for different devices, because a dependent that follows a
// source change from one to the other compares the two and has to see a
// difference. The counter is 64-bit and incremented once per device creation, so
// it cannot realistically wrap.
//
// Thread-safe without a mutex through an atomic fetch-add: device creation
// happens on whichever pump thread owns the producer, and there are several.
[[nodiscard]] DeviceGeneration NextDeviceGeneration() noexcept;

// Whether a dependent built for `built_for` may still be used with `current`.
//
// Spelled out as its own function because the wrong version of this check reads
// perfectly reasonable -- "the device is there and the size matches, carry on" --
// and every caller needs the same answer. A dependent built without a device
// (kNoDevice) is never current, including when there is no device now either:
// "nothing was ever built" and "what was built is still good" are different
// states, and only the second may skip a rebuild.
[[nodiscard]] constexpr bool DeviceResourceIsCurrent(DeviceGeneration built_for, DeviceGeneration current) noexcept {
    return built_for.Valid() && current.Valid() && built_for == current;
}

// ---------------------------------------------------------------------------
// Rebuilding a producer whose device died, under a bound.
//
// The retry has to be bounded in a way the unbounded reopen of a live device is
// not. A display that is renegotiating comes back; a device that is gone comes
// back only if a new one can be created, and when that keeps failing -- the
// adapter is being reset, the GPU was removed for good -- an unbounded loop
// recreates a device on every tick for the rest of the session, at a cost no
// preview is worth.
//
// While the rebuild is in progress the consumer must see "unavailable", never a
// value carried over from the dead device: a stale snapshot that reads like a
// fresh measurement is worse than no measurement.
// ---------------------------------------------------------------------------
enum class DeviceRebuildStep {
    Rebuild,   // Try now: create a device and rebuild everything derived from it.
    WaitRetry, // Too soon after the last attempt; leave it for a later tick.
    GiveUp,    // Out of attempts. Report the source as unavailable and stop trying.
};

struct DeviceRebuildPolicy {
    // Attempts allowed per loss. Four covers a driver reset (TDR recovery takes
    // a few seconds) without turning a removed GPU into a permanent retry loop.
    uint32_t max_attempts = 4;
    // Minimum gap between attempts, in milliseconds. Creating a device is not
    // cheap and an adapter mid-reset fails fast, so a per-tick retry would be
    // all cost.
    uint64_t retry_interval_ms = 500;
};

// Pure: time and the attempt count are parameters, so the exhaustion path is
// reachable in a test without a GPU and without waiting.
[[nodiscard]] constexpr DeviceRebuildStep NextDeviceRebuildStep(uint32_t attempts_made, uint64_t ms_since_last_attempt,
                                                                const DeviceRebuildPolicy& policy) noexcept {
    if (attempts_made >= policy.max_attempts)
        return DeviceRebuildStep::GiveUp;
    // The first attempt is immediate: the loss has just been noticed, and making
    // the user wait out an interval before the first try adds latency to the
    // common case (a transient reset) for no benefit.
    if (attempts_made > 0 && ms_since_last_attempt < policy.retry_interval_ms)
        return DeviceRebuildStep::WaitRetry;
    return DeviceRebuildStep::Rebuild;
}

} // namespace exosnap::engine
