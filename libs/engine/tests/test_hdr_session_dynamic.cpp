// What a running session may adopt from its display, and what obliges it to
// stop instead. Pure, GPU-free.

#include "../src/hdr_session_dynamic.h"

#include <gtest/gtest.h>

using namespace exosnap::engine;

namespace {

// An HDR desktop as the capture path resolves one: actively HDR, a real peak,
// and the SDR content brightness the user has the slider on.
HdrDisplayFacts HdrDisplay(float sdr_white_nits, float max_luminance_nits = 1000.0f) {
    HdrDisplayFacts facts;
    facts.hdr_active = true;
    facts.max_luminance_nits = max_luminance_nits;
    facts.sdr_white_level_nits = sdr_white_nits;
    return facts;
}

} // namespace

TEST(SessionHdrDynamicState, ResolvesEveryConsumerScalarFromOneSetOfFacts) {
    const SessionHdrDynamicState state = ResolveSessionHdrDynamicState(HdrDisplay(280.0f, 1000.0f));

    EXPECT_FLOAT_EQ(state.peak_scale, HdrPeakScale(true, 1000.0f));
    EXPECT_FLOAT_EQ(state.paper_white_scale, 280.0f / 80.0f);
    EXPECT_FLOAT_EQ(state.overlay_reference_white_nits, 280.0f);
}

TEST(ClassifyDisplayFactsChange, IdenticalFactsObligeNothing) {
    const HdrDisplayFacts facts = HdrDisplay(280.0f);
    EXPECT_EQ(ClassifyDisplayFactsChange(facts, facts), DisplayFactsChange::None);
}

// The defect this classification exists for: the user moves the Windows SDR
// content brightness slider during a recording. The desktop is composed at a
// new reference white from that moment, and every consumer still holding the
// old paper white draws the same frame at the wrong brightness.
TEST(ClassifyDisplayFactsChange, ANewSdrWhiteLevelIsALiveParameterChange) {
    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(280.0f), HdrDisplay(120.0f)), DisplayFactsChange::LiveParameters);
}

TEST(ClassifyDisplayFactsChange, ANewDisplayPeakIsALiveParameterChange) {
    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(280.0f, 1000.0f), HdrDisplay(280.0f, 600.0f)),
              DisplayFactsChange::LiveParameters);
}

// Raw nits are not the question; what a consumer would draw is. An unknown and
// an implausible reading both resolve to the OS default, so neither is a change
// anything could act on -- and acting would rewrite constant buffers and
// republish shared textures for an identical picture.
TEST(ClassifyDisplayFactsChange, ReadingsThatResolveToTheSameStateObligeNothing) {
    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(0.0f), HdrDisplay(100000.0f)), DisplayFactsChange::None);
    // Below the scene-referred identity both clamp to 1.0 paper white.
    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(0.0f), HdrDisplay(-5.0f)), DisplayFactsChange::None);
}

// The colour description is committed into the bitstream when the session
// starts, so this one is never a parameter update however the facts differ
// otherwise.
TEST(ClassifyDisplayFactsChange, AnHdrToggleIsASessionModeChange) {
    HdrDisplayFacts sdr = HdrDisplay(280.0f);
    sdr.hdr_active = false;

    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(280.0f), sdr), DisplayFactsChange::SessionMode);
    EXPECT_EQ(ClassifyDisplayFactsChange(sdr, HdrDisplay(280.0f)), DisplayFactsChange::SessionMode);
}

TEST(ClassifyDisplayFactsChange, AnHdrToggleOutranksAParameterChange) {
    HdrDisplayFacts sdr = HdrDisplay(120.0f, 600.0f);
    sdr.hdr_active = false;

    EXPECT_EQ(ClassifyDisplayFactsChange(HdrDisplay(280.0f, 1000.0f), sdr), DisplayFactsChange::SessionMode);
}
