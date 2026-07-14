#include <gtest/gtest.h>

#include "recorder_core/edit_player_engine.h"

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

} // namespace
