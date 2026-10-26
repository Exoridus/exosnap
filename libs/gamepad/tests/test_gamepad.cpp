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

TEST(DeviceHistory, KeepsIdenticalModelsSeparateAndManualSelectionStable) {
    DeviceHistory history;
    auto first = At(100, 1);
    auto second = At(110, 2);
    first.device.id[0] = 1;
    second.device.id[0] = 2;
    history.Push(first);
    history.Push(second);
    EXPECT_EQ(history.Devices().size(), 2U);
    EXPECT_EQ(history.StateAt(120)->state.buttons, 1U);
    history.Select(second.device.id);
    EXPECT_EQ(history.StateAt(120)->state.buttons, 2U);
    first.capture_100ns = 130;
    first.state.buttons = 4;
    history.Push(first);
    EXPECT_EQ(history.StateAt(140)->state.buttons, 2U);
}

TEST(DeviceHistory, BusyDeviceCannotEvictQuietDeviceState) {
    DeviceHistory history;
    auto quiet = At(100, 1);
    quiet.device.id[0] = 1;
    history.Push(quiet);
    for (uint64_t time = 101; time < 1000; ++time) {
        auto busy = At(time, 2);
        busy.device.id[0] = 2;
        history.Push(busy);
    }
    history.Select(quiet.device.id);
    ASSERT_TRUE(history.StateAt(1000));
    EXPECT_EQ(history.StateAt(1000)->state.buttons, 1U);
}

TEST(DeviceHistory, DisconnectRestsAndReconnectDoesNotRestoreOldPress) {
    DeviceHistory history;
    auto reading = At(100, 1);
    reading.device.id[0] = 1;
    history.Push(reading);
    reading.capture_100ns = 200;
    reading.connected = false;
    history.Push(reading);
    EXPECT_TRUE(history.Devices().empty());
    ASSERT_TRUE(history.StateAt(200));
    EXPECT_FALSE(history.StateAt(200)->connected);
    EXPECT_EQ(history.StateAt(200)->state.buttons, 0U);
    EXPECT_EQ(history.StateAt(150)->state.buttons, 1U);
    reading.capture_100ns = 300;
    reading.connected = true;
    reading.state = {};
    history.Push(reading);
    EXPECT_TRUE(history.StateAt(300)->connected);
    EXPECT_EQ(history.StateAt(300)->state.buttons, 0U);
}

TEST(DeviceHistory, MissingManualDeviceNeverFallsBackToAnotherPad) {
    DeviceHistory history;
    auto reading = At(100, 1);
    reading.device.id[0] = 1;
    history.Push(reading);
    DeviceId missing{};
    missing[0] = 2;
    history.Select(missing);
    EXPECT_FALSE(history.StateAt(200));
    history.Select(std::nullopt);
    EXPECT_EQ(history.StateAt(200)->device.id, reading.device.id);
}

TEST(DeviceHistory, AutomaticSelectionPrefersFirstActivePadThenStaysWithIt) {
    DeviceHistory history;
    auto quiet = At(100);
    quiet.device.id[0] = 1;
    history.Push(quiet);
    auto active = At(200, 4);
    active.device.id[0] = 2;
    history.Push(active);
    EXPECT_EQ(history.StateAt(200)->device.id, active.device.id);
    quiet.capture_100ns = 300;
    quiet.state.buttons = 8;
    history.Push(quiet);
    EXPECT_EQ(history.StateAt(300)->device.id, active.device.id);
}

TEST(DeviceHistory, PartialReadingLookupIgnoresManualSelectionAndFutureStates) {
    DeviceHistory history;
    auto first = At(100, 4);
    first.device.id[0] = 1;
    first.state.home = true;
    history.Push(first);
    auto second = At(200, 8);
    second.device.id[0] = 2;
    history.Push(second);
    history.Select(second.device.id);
    const auto previous = history.StateFor(first.device.id, 150);
    ASSERT_TRUE(previous);
    EXPECT_EQ(previous->state.buttons, 4U);
    EXPECT_EQ(previous->state.home, true);
    EXPECT_FALSE(history.StateFor(second.device.id, 150));
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
