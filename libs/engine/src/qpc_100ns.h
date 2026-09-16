#pragma once

#include <cstdint>

namespace exosnap::engine {

// A performance-counter reading in 100 ns units.
//
// Split into whole seconds and a remainder rather than multiplied first. The
// direct form, `ticks * 10'000'000 / frequency`, overflows a 64-bit integer once
// `ticks` passes about 9.2e11 -- roughly a day of uptime at a 10 MHz counter, and
// less on faster ones. Past that it does not saturate or fail; it wraps, and
// returns a plausible-looking time that is wrong by hundreds of thousands of
// seconds. A value used as a timeline anchor has no way to notice.
//
// `frequency_hz` must be the counter's frequency and non-zero; zero returns zero
// rather than dividing.
[[nodiscard]] constexpr uint64_t QpcTicksTo100ns(uint64_t ticks, uint64_t frequency_hz) noexcept {
    if (frequency_hz == 0) {
        return 0;
    }
    return (ticks / frequency_hz) * 10'000'000ULL + ((ticks % frequency_hz) * 10'000'000ULL) / frequency_hz;
}

} // namespace exosnap::engine
