#include <gtest/gtest.h>

#include "session_internal.h"

namespace {

using recorder_core::CodecPrivateData;

TEST(CodecPrivateReadinessTest, AudioAllReady_ZeroTracks_IsTrue) {
    const CodecPrivateData data{};
    EXPECT_TRUE(data.AudioAllReady(0));
}

TEST(CodecPrivateReadinessTest, AudioAllReady_OneTrack_FalseUntilTrackZeroReady) {
    CodecPrivateData data{};
    EXPECT_FALSE(data.AudioAllReady(1));

    data.aac_track_ready[0] = true;
    EXPECT_TRUE(data.AudioAllReady(1));
}

TEST(CodecPrivateReadinessTest, AudioAllReady_TwoTracks_RequiresBoth) {
    CodecPrivateData data{};

    data.aac_track_ready[0] = true;
    EXPECT_FALSE(data.AudioAllReady(2));

    data.aac_track_ready[1] = true;
    EXPECT_TRUE(data.AudioAllReady(2));
}

TEST(CodecPrivateReadinessTest, AudioAllReady_ThreeTracks_RequiresAll) {
    CodecPrivateData data{};

    data.aac_track_ready[0] = true;
    data.aac_track_ready[1] = true;
    EXPECT_FALSE(data.AudioAllReady(3));

    data.aac_track_ready[2] = true;
    EXPECT_TRUE(data.AudioAllReady(3));
}

TEST(CodecPrivateReadinessTest, CodecPrivateData_Reset_StartsNotReady) {
    const CodecPrivateData data{};

    EXPECT_FALSE(data.av1_ready);
    for (bool track_ready : data.aac_track_ready) {
        EXPECT_FALSE(track_ready);
    }

    EXPECT_FALSE(data.AudioAllReady(1));
    EXPECT_FALSE(data.AudioAllReady(2));
    EXPECT_FALSE(data.AudioAllReady(3));
}

} // namespace
