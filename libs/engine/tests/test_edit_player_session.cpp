#include <gtest/gtest.h>

#include "exosnap/engine/edit_player_engine.h"
#include "exosnap/engine/edit_player_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace {

using exosnap::engine::EditPlayerSession;

TEST(EditPlayerSession, OpenNonexistentFileFails) {
    EditPlayerSession session;
    std::string err;
    EXPECT_FALSE(session.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
}

TEST(EditPlayerSession, ClosedSessionReportsNoAudioStream) {
    EditPlayerSession session;
    EXPECT_FALSE(session.HasAudioStream());
}

TEST(EditPlayerSession, PlayPauseSeekWithoutOpenAreSafeNoOps) {
    EditPlayerSession session;
    session.Play();
    session.Pause();
    session.SeekTo(0);
    SUCCEED();
}

TEST(EditPlayerSession, CloseWithoutOpenIsSafeNoOp) {
    EditPlayerSession session;
    session.Close();
    SUCCEED();
}

TEST(EditPlayerSession, CurrentPositionMsWithoutAudioStreamIsZero) {
    EditPlayerSession session;
    EXPECT_EQ(session.CurrentPositionMs(), 0);
}

TEST(EditPlayerSession, ClosedSessionReportsUnknownFrameRate) {
    EditPlayerSession session;
    EXPECT_DOUBLE_EQ(session.VideoFrameRate(), 0.0);
}

TEST(EditPlayerSession, VideoOnlyClockAdvances) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerSession session;
    std::string error;
    ASSERT_TRUE(session.Open(fixture, error)) << error;
    ASSERT_FALSE(session.HasAudioStream());
    session.Play(500'000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session.CurrentPositionMs() <= 520 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_GT(session.CurrentPositionMs(), 520);
    EXPECT_GE(session.ClockSnapshotUs(), 520'000);
    session.Pause();
    const auto paused = session.CurrentPositionMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(session.CurrentPositionMs(), paused);
    EXPECT_LT(session.ClockSnapshotUs(), 0);
}

TEST(EditPlayerSession, CloseWakesVideoPresentationWait) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerSession session;
    std::string error;
    ASSERT_TRUE(session.Open(fixture, error)) << error;
    session.Play();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start = std::chrono::steady_clock::now();
    session.Close();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
}

TEST(EditPlayerSession, NewestSeekCarriesItsPresentationGeneration) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerSession session;
    std::string error;
    ASSERT_TRUE(session.Open(fixture, error)) << error;
    std::atomic<uint64_t> presented_generation{0};
    session.SetOnFrameReadyWithGeneration(
        [&](exosnap::engine::RawDecodedVideoFrame, uint64_t generation) { presented_generation.store(generation); });
    for (uint64_t generation = 1; generation <= 100; ++generation)
        session.SeekTo(static_cast<int64_t>(generation) * 10'000, generation);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (presented_generation.load() != 100 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_EQ(presented_generation.load(), 100u);
    session.Close();
}

TEST(EditPlayerSession, ExactSeekPositionRetainsMicroseconds) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "reordered_h264.mp4";
    EditPlayerSession session;
    std::string error;
    ASSERT_TRUE(session.Open(fixture, error)) << error;
    session.SeekTo(1'000'123);
    EXPECT_EQ(session.CurrentPositionUs(), 1'000'123);
    EXPECT_EQ(session.CurrentPositionMs(), 1000);
    session.Close();
}
} // namespace
