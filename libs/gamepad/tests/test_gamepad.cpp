#include "gamepad/gamepad.h"

#include <gtest/gtest.h>

#include <limits>

using namespace exosnap::gamepad;

namespace {

GamepadReading At(uint64_t capture_100ns, uint32_t buttons = 0) {
    GamepadReading reading;
    reading.capture_100ns = capture_100ns;
    reading.state.buttons = buttons;
    return reading;
}

} // namespace

TEST(ReadingRing, EmptyHasNoState) {
    const ReadingRing ring(4);
    EXPECT_FALSE(ring.StateAt(0).has_value());
    EXPECT_FALSE(ring.StateAt(std::numeric_limits<uint64_t>::max()).has_value());
}

TEST(ReadingRing, ReturnsNewestAtOrBefore) {
    ReadingRing ring(8);
    ring.Push(At(100, 1));
    ring.Push(At(200, 2));
    ring.Push(At(300, 3));

    EXPECT_FALSE(ring.StateAt(99).has_value());
    EXPECT_EQ(ring.StateAt(100)->state.buttons, 1U);
    EXPECT_EQ(ring.StateAt(199)->state.buttons, 1U);
    EXPECT_EQ(ring.StateAt(200)->state.buttons, 2U);
    EXPECT_EQ(ring.StateAt(1000)->state.buttons, 3U);
}

TEST(ReadingRing, OutOfOrderPushesResolveByTimestamp) {
    ReadingRing ring(8);
    ring.Push(At(300, 3));
    ring.Push(At(100, 1));
    EXPECT_EQ(ring.StateAt(250)->state.buttons, 1U);
    EXPECT_EQ(ring.StateAt(300)->state.buttons, 3U);
}

TEST(ReadingRing, EqualTimestampPrefersLaterPush) {
    ReadingRing ring(2);
    ring.Push(At(50, 1));
    ring.Push(At(100, 2));
    ring.Push(At(100, 3)); // wraps over slot 0
    EXPECT_EQ(ring.StateAt(100)->state.buttons, 3U);
}

TEST(ReadingRing, WraparoundEvictsOldest) {
    ReadingRing ring(3);
    for (uint64_t t = 1; t <= 5; ++t) {
        ring.Push(At(t * 10, static_cast<uint32_t>(t)));
    }
    // Readings 1 and 2 were overwritten: nothing retained is as old as 25.
    EXPECT_FALSE(ring.StateAt(25).has_value());
    EXPECT_EQ(ring.StateAt(30)->state.buttons, 3U);
    EXPECT_EQ(ring.StateAt(45)->state.buttons, 4U);
    EXPECT_EQ(ring.StateAt(50)->state.buttons, 5U);
}

TEST(ClockConversion, MapsThroughAnchor) {
    const ClockAnchor anchor{.gameinput_us = 1'000'000, .capture_100ns = 50'000'000};
    EXPECT_EQ(GameInputToCapture100ns(1'000'000, anchor), 50'000'000U);
    EXPECT_EQ(GameInputToCapture100ns(1'000'016, anchor), 50'000'160U);
    EXPECT_EQ(GameInputToCapture100ns(999'000, anchor), 49'990'000U);
}

TEST(ClockConversion, SaturatesBeforeCaptureOrigin) {
    const ClockAnchor anchor{.gameinput_us = 1'000'000, .capture_100ns = 100};
    EXPECT_EQ(GameInputToCapture100ns(0, anchor), 0U);
    EXPECT_EQ(GameInputToCapture100ns(999'990, anchor), 0U);
}

TEST(LayoutKind, MapsKnownControllers) {
    EXPECT_EQ(LayoutKindFor(0x054C, 0x0CE6), LayoutKind::DualSense);
    EXPECT_EQ(LayoutKindFor(0x054C, 0x0DF2), LayoutKind::DualSense);
    EXPECT_EQ(LayoutKindFor(0x045E, 0x0B12), LayoutKind::XboxSeries);
    EXPECT_EQ(LayoutKindFor(0x045E, 0x0B13), LayoutKind::XboxSeries);
    EXPECT_EQ(LayoutKindFor(0x045E, 0x02EA), LayoutKind::XboxOne);
    EXPECT_EQ(LayoutKindFor(0x045E, 0x02DD), LayoutKind::XboxOne);
    EXPECT_EQ(LayoutKindFor(0x057E, 0x2009), LayoutKind::SwitchPro);
}

TEST(LayoutKind, UnknownIsGeneric) {
    EXPECT_EQ(LayoutKindFor(0, 0), LayoutKind::Generic);
    EXPECT_EQ(LayoutKindFor(0x054C, 0x05C4), LayoutKind::Generic); // DualShock 4
    EXPECT_EQ(LayoutKindFor(0x045E, 0xFFFF), LayoutKind::Generic);
    EXPECT_EQ(LayoutKindFor(0x057E, 0x2006), LayoutKind::Generic); // Joy-Con
    EXPECT_EQ(LayoutKindFor(0x0E6F, 0x0B12), LayoutKind::Generic); // PID alone is not enough
}

TEST(Normalized, ClampsAndRestsNaN) {
    GamepadState raw;
    raw.buttons = 0x5;
    raw.left_x = 1.5F;
    raw.left_y = -2.0F;
    raw.right_x = std::numeric_limits<float>::quiet_NaN();
    raw.right_y = 0.25F;
    raw.left_trigger = -0.1F;
    raw.right_trigger = 3.0F;

    const GamepadState n = Normalized(raw);
    EXPECT_EQ(n.buttons, 0x5U);
    EXPECT_FLOAT_EQ(n.left_x, 1.0F);
    EXPECT_FLOAT_EQ(n.left_y, -1.0F);
    EXPECT_FLOAT_EQ(n.right_x, 0.0F);
    EXPECT_FLOAT_EQ(n.right_y, 0.25F);
    EXPECT_FLOAT_EQ(n.left_trigger, 0.0F);
    EXPECT_FLOAT_EQ(n.right_trigger, 1.0F);
}
