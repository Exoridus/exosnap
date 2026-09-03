#include <gtest/gtest.h>

#include <exosnap/engine/od_output_signature.h>

using exosnap::engine::OutputModeChanged;
using exosnap::engine::OutputModeSignature;

namespace {
OutputModeSignature Opened() {
    OutputModeSignature s;
    s.width = 2560;
    s.height = 1440;
    s.refresh_hz = 144;
    s.orientation = 0;
    s.hdr_active = false;
    s.known = true;
    return s;
}
} // namespace

// The signals IDXGIFactory1::IsCurrent() does not carry: a mode change on an
// adapter that stays.

TEST(OutputModeChanged, SameModeIsUnchanged) {
    EXPECT_FALSE(OutputModeChanged(Opened(), Opened()));
}

TEST(OutputModeChanged, RefreshRateSwitch) {
    auto now = Opened();
    now.refresh_hz = 60;
    EXPECT_TRUE(OutputModeChanged(Opened(), now));
}

TEST(OutputModeChanged, ResolutionSwitch) {
    auto now = Opened();
    now.width = 1920;
    now.height = 1080;
    EXPECT_TRUE(OutputModeChanged(Opened(), now));
}

TEST(OutputModeChanged, HdrToggle) {
    auto now = Opened();
    now.hdr_active = true;
    EXPECT_TRUE(OutputModeChanged(Opened(), now));
}

TEST(OutputModeChanged, OrientationSwitch) {
    auto now = Opened();
    now.orientation = 1;
    EXPECT_TRUE(OutputModeChanged(Opened(), now));
}

TEST(OutputModeChanged, AnUnreadableModeIsNotEvidence) {
    OutputModeSignature unknown; // known == false
    EXPECT_FALSE(OutputModeChanged(Opened(), unknown));
    EXPECT_FALSE(OutputModeChanged(unknown, Opened()));
    EXPECT_FALSE(OutputModeChanged(unknown, unknown));
}
