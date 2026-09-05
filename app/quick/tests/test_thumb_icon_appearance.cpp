#include <gtest/gtest.h>

#include <array>

#include "TaskbarPresence.h"
#include "exosnap_resource.h"
#include "models/ShellPresence.h"

using exosnap::ShellAction;
using exosnap::quick::ThumbIconResourceFor;

namespace {

// Every action the thumbnail strip can offer. ShellAction::None is excluded on
// purpose: it names a slot with nothing in it, and the resolver's fallback for it
// is not a claim about a button.
constexpr std::array<ShellAction, 5> kThumbActions{{
    ShellAction::Start,
    ShellAction::Pause,
    ShellAction::Resume,
    ShellAction::Stop,
    ShellAction::OpenOutputFolder,
}};

} // namespace

// The defect this pairing exists for: the glyphs are coloured for the appearance
// they are drawn against, and the strip's ground follows WINDOWS. A light-chrome
// entry that returned the dark glyph put an amber pause mark on light grey.
TEST(ThumbIconResourceFor, EveryActionHasItsOwnGlyphInBothAppearances) {
    for (const ShellAction action : kThumbActions) {
        const int dark = ThumbIconResourceFor(action, /*light_chrome=*/false);
        const int light = ThumbIconResourceFor(action, /*light_chrome=*/true);
        EXPECT_NE(dark, 0);
        EXPECT_NE(light, 0);
        EXPECT_NE(dark, light) << "action " << static_cast<int>(action) << " uses one glyph for both appearances";
    }
}

// Two actions deliberately SHARE a glyph within one appearance (record and stop
// are both coral marks), so uniqueness is asserted across the pair of sets rather
// than within one: no light id may collide with a dark id.
TEST(ThumbIconResourceFor, TheTwoSetsDoNotOverlap) {
    for (const ShellAction light_action : kThumbActions) {
        const int light = ThumbIconResourceFor(light_action, /*light_chrome=*/true);
        for (const ShellAction dark_action : kThumbActions) {
            EXPECT_NE(light, ThumbIconResourceFor(dark_action, /*light_chrome=*/false))
                << "a light glyph id is also used as a dark one";
        }
    }
}

TEST(ThumbIconResourceFor, ResolvesToTheDeclaredResources) {
    EXPECT_EQ(ThumbIconResourceFor(ShellAction::Pause, false), IDI_EXOSNAP_THUMB_PAUSE);
    EXPECT_EQ(ThumbIconResourceFor(ShellAction::Pause, true), IDI_EXOSNAP_THUMB_PAUSE_LIGHT);
    EXPECT_EQ(ThumbIconResourceFor(ShellAction::OpenOutputFolder, false), IDI_EXOSNAP_THUMB_FOLDER);
    EXPECT_EQ(ThumbIconResourceFor(ShellAction::OpenOutputFolder, true), IDI_EXOSNAP_THUMB_FOLDER_LIGHT);
}
