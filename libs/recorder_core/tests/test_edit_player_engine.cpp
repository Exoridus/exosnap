#include <gtest/gtest.h>

#include "recorder_core/edit_player_engine.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace {

using recorder_core::EditPlayerEngine;

TEST(EditPlayerEngine, OpenNonexistentFileFails) {
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
    EXPECT_FALSE(err.empty());
}

TEST(EditPlayerEngine, OpenGarbageFileFails) {
    // A file that exists but is not a container libavformat can probe.
    const auto path = std::filesystem::temp_directory_path() / "edit_player_engine_garbage_test.bin";
    {
        std::ofstream f(path, std::ios::binary);
        f << "not a media file, just some bytes 1234567890";
    }
    EditPlayerEngine engine;
    std::string err;
    EXPECT_FALSE(engine.Open(path, err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove(path);
}

TEST(EditPlayerEngine, ClosedEngineReportsNoStreams) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.HasVideoStream());
    EXPECT_FALSE(engine.HasAudioStream());
}

TEST(EditPlayerEngine, DecodeFrameAtWithoutOpenReturnsNullopt) {
    EditPlayerEngine engine;
    EXPECT_FALSE(engine.DecodeFrameAt(0).has_value());
}

TEST(EditPlayerEngine, StartStopPlaybackDecodeWithoutOpenIsSafeNoOp) {
    EditPlayerEngine engine;
    std::atomic<int> video_calls{0};
    engine.StartPlaybackDecode(
        0, [&](recorder_core::DecodedVideoFrame) { ++video_calls; }, [](recorder_core::DecodedAudioBlock) {});
    engine.StopPlaybackDecode();
    EXPECT_EQ(video_calls.load(), 0);
}

TEST(EditPlayerEngine, StopPlaybackDecodeWithoutStartIsSafeNoOp) {
    EditPlayerEngine engine;
    engine.StopPlaybackDecode(); // must not crash / hang
    SUCCEED();
}

TEST(EditPlayerEngine, RepeatedStartStopPlaybackDecodeWithoutOpenIsSafe) {
    // Start/stop are lifecycle-idempotent while nothing is open: repeated
    // starts, double-stops, and a Close in the middle must all be safe no-ops.
    EditPlayerEngine engine;
    for (int i = 0; i < 3; ++i) {
        engine.StartPlaybackDecode(0, [](recorder_core::DecodedVideoFrame) {}, [](recorder_core::DecodedAudioBlock) {});
        engine.StopPlaybackDecode();
        engine.StopPlaybackDecode(); // double-stop must be safe
    }
    engine.Close();
    SUCCEED();
}

} // namespace
