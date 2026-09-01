#pragma once

#include <cmath>
#include <limits>

namespace exosnap::models {

// The one definition of the audio meter's scale.
//
// Both meters in the product read the same number: the Record dock's ring and
// the Settings source rows. The scale was previously a lambda beside one of
// them, which meant the 0..1 position a bar drew and the dB value a label could
// have shown were two independent statements about the same signal.
inline constexpr double kMeterFloorDb = -60.0;

// Silence is not "the bottom of the scale": a source that is producing nothing
// and one sitting at the floor are different facts, and a reading of -60 dB for
// the first is a measurement that was never taken.
[[nodiscard]] inline double MeterDbfsFromRms(float rms) noexcept {
    if (rms <= 0.0f)
        return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(static_cast<double>(rms));
}

// Position on the meter, 0 at the floor and 1 at full scale.
[[nodiscard]] inline double MeterLevelFromDbfs(double dbfs) noexcept {
    if (!(dbfs > kMeterFloorDb))
        return 0.0;
    const double level = (dbfs - kMeterFloorDb) / -kMeterFloorDb;
    return level > 1.0 ? 1.0 : level;
}

} // namespace exosnap::models
