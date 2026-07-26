#pragma once

#include <QList>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>

namespace exosnap {

// Ceiling used when no display refresh rate can be read at all (headless hosts,
// remote sessions, drivers reporting 0 Hz), and the floor the derived ceiling
// never drops below. Both roles land on the same number: the shipped default
// profile is CFR 60 fps, so 60 must stay both the safe assumption and always
// expressible.
inline constexpr int kFallbackMaxFrameRate = 60;

// Highest whole-fps frame rate worth offering in the Expert free-entry field.
// Capture never delivers more frames than the fastest attached display's
// compositor rate, so a CFR target above it is pure frame duplication. Rates are
// rounded to the nearest whole fps (143.96 -> 144, 59.94 -> 60); non-positive
// entries (a driver that does not report a rate) are ignored.
inline int MaxFrameRateForRefreshRates(const QList<qreal>& refresh_rates) {
    int best = 0;
    for (const qreal rate : refresh_rates) {
        if (rate > 0.0)
            best = (std::max)(best, qRound(rate));
    }
    return (std::max)(best, kFallbackMaxFrameRate);
}

// Clamps a configured frame rate into the usable [1, maximum] range.
inline uint32_t ClampFrameRate(uint32_t fps, int maximum) {
    const uint32_t top = static_cast<uint32_t>((std::max)(1, maximum));
    if (fps < 1u)
        return 1u;
    return fps > top ? top : fps;
}

} // namespace exosnap
