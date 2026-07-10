#include <gtest/gtest.h>

#include "services/DisplayNumbering.h"

// Pins the sequential "Display N" label (DisplayNumbering.h) that the source
// picker, the Record header and the output filename share. The defect this
// guards against: after plug/unplug cycles the GDI names skip numbers
// (\\.\DISPLAY6, \\.\DISPLAY7), and the picker said "Display 1/2" while the
// header and filename said "Display 6/7".

using exosnap::SequentialDisplayLabel;

namespace {

std::unordered_map<std::wstring, int> TwoDisplayTopology() {
    // A real-world post-hot-plug topology: the numbers skip, the sequence must not.
    return {{L"\\\\.\\DISPLAY6", 1}, {L"\\\\.\\DISPLAY7", 2}};
}

} // namespace

TEST(DisplayNumbering, ResequencesTheRawGdiName) {
    const auto seq = TwoDisplayTopology();
    EXPECT_EQ(SequentialDisplayLabel("\\\\.\\DISPLAY6", seq), "Display 1");
    EXPECT_EQ(SequentialDisplayLabel("\\\\.\\DISPLAY7", seq), "Display 2");
}

TEST(DisplayNumbering, UnknownDeviceFallsBackToItsRawNumber) {
    // A display that just left the topology keeps its honest raw number
    // instead of being assigned one that now belongs to another display.
    const auto seq = TwoDisplayTopology();
    EXPECT_EQ(SequentialDisplayLabel("\\\\.\\DISPLAY3", seq), "Display 3");
}

TEST(DisplayNumbering, ForwardSlashPrefixAlsoParses) {
    EXPECT_EQ(SequentialDisplayLabel("//./DISPLAY4", {}), "Display 4");
}

TEST(DisplayNumbering, NonDeviceDescriptionsPassThroughTrimmed) {
    EXPECT_EQ(SequentialDisplayLabel("  Some Monitor  ", {}), "Some Monitor");
    EXPECT_EQ(SequentialDisplayLabel("\\\\.\\DISPLAYX", {}), "DISPLAYX"); // non-numeric suffix
    EXPECT_EQ(SequentialDisplayLabel("", {}), "Display");
}
