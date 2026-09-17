#pragma once

#include <exosnap/engine/hdr_native.h>

#include "frame_luminance.h"
#include "hdr_tonemap.h"

// ---------------------------------------------------------------------------
// What a running session may adopt from its display without being rebuilt.
//
// A session resolves its colour parameters from the captured display's facts
// once, at start, and hands them to consumers that hold them as GPU constants:
// the encode tone-map, the overlay compositor and the preview tap. Windows lets
// the user move the SDR-content-brightness slider at any time, which changes the
// scRGB values the desktop is composed at -- so those constants describe a
// display state that no longer exists, and every consumer holding one drifts
// away from the picture in the same direction.
//
// The parameters below can be replaced on a live session: they are scalars in a
// shader's constant buffer, and changing one costs an UpdateSubresource. What
// cannot is the HDR colour space itself -- the frame-pool format, the native vs
// tone-map decision, the bit depth and the colour description committed into the
// bitstream are all fixed when the session starts, and a half-switched session
// would write frames the file's own metadata misdescribes.
//
// That is the whole reason this is a classification rather than a bool: the two
// kinds of change have different answers, and only one of them is cheap.
// ---------------------------------------------------------------------------

namespace exosnap::engine {

// The display-derived scalars a live session can adopt. Resolved together from
// one set of facts so no consumer can be updated from a different snapshot than
// its neighbour.
struct SessionHdrDynamicState {
    // scRGB->SDR tone-map knee, in reference-white multiples (HdrPeakScale).
    float peak_scale = 1.0f;
    // The OS SDR reference white in the same units (SdrPaperWhiteScale).
    float paper_white_scale = 1.0f;
    // The same reference white in cd/m^2, which is the unit the overlay
    // compositor takes (EffectiveOverlayReferenceWhiteNits).
    float overlay_reference_white_nits = kDefaultSdrWhiteLevelNits;
    // The knee the encode tone-map runs on. Separate from peak_scale because the
    // two answer different questions: the encode target is an SDR file, so its
    // roll-off belongs on the luminance the CONTENT actually reaches, while the
    // preview draws onto the panel in front of the user and keeps rolling off
    // against what the panel can show. Equal to peak_scale until a measurement
    // exists.
    float tone_map_peak_scale = 1.0f;
};

// `measured_content_peak_nits` is the smoothed content peak from the per-frame
// luminance pass, or a non-positive value while none has been measured yet --
// the first frames of every session, and every session that does not run the
// pass at all. Only the tone-map knee reads it; the display-derived scalars are
// resolved from the facts either way, so a session without the pass resolves
// exactly the state it resolved before the pass existed.
[[nodiscard]] inline SessionHdrDynamicState
ResolveSessionHdrDynamicState(const HdrDisplayFacts& facts, float measured_content_peak_nits = 0.0f) noexcept {
    SessionHdrDynamicState state;
    state.peak_scale = HdrPeakScale(facts.hdr_active, facts.max_luminance_nits, facts.sdr_white_level_nits);
    state.paper_white_scale = SdrPaperWhiteScale(facts.sdr_white_level_nits);
    state.overlay_reference_white_nits = EffectiveOverlayReferenceWhiteNits(facts.sdr_white_level_nits);
    state.tone_map_peak_scale = measured_content_peak_nits > 0.0f
                                    ? ContentPeakScale(measured_content_peak_nits, facts.sdr_white_level_nits)
                                    : state.peak_scale;
    return state;
}

enum class DisplayFactsChange : uint8_t {
    // Nothing a consumer would draw differently.
    None,
    // Scalars only: every consumer takes the new SessionHdrDynamicState and the
    // recording continues.
    LiveParameters,
    // The display entered or left HDR. The session's colour pipeline was
    // committed at start and cannot follow, so this is not something to patch.
    SessionMode,
};

// Pure: what the difference between the facts a session is running on and the
// facts the display reports now obliges it to do.
//
// The comparison is over the RESOLVED state rather than the raw nits, and that
// is deliberate: an unknown or implausible level resolves to the OS default, so
// two different readings can mean the identical picture. Comparing the raw
// numbers would republish shared textures and rewrite constant buffers for a
// change no consumer could draw.
[[nodiscard]] inline DisplayFactsChange ClassifyDisplayFactsChange(const HdrDisplayFacts& published,
                                                                   const HdrDisplayFacts& fresh) noexcept {
    if (published.hdr_active != fresh.hdr_active) {
        return DisplayFactsChange::SessionMode;
    }
    const SessionHdrDynamicState before = ResolveSessionHdrDynamicState(published);
    const SessionHdrDynamicState after = ResolveSessionHdrDynamicState(fresh);
    if (before.peak_scale != after.peak_scale || before.paper_white_scale != after.paper_white_scale ||
        before.overlay_reference_white_nits != after.overlay_reference_white_nits) {
        return DisplayFactsChange::LiveParameters;
    }
    return DisplayFactsChange::None;
}

} // namespace exosnap::engine
