#include "exosnap/engine/codec_types.h"
#include "exosnap/engine/error_types.h"
#include "exosnap/engine/packet_types.h"
#include "premux_state.h"
#include "session_failure.h"
#include "split_sentinel_policy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace {

using namespace exosnap::engine;
using namespace std::chrono_literals;

TEST(PremuxState, ReadinessWaitRequiresVideoAndEveryAudioTrack) {
    PremuxState state;
    std::atomic<bool> stopped{false};
    auto waiter = std::async(std::launch::async,
                             [&] { return state.WaitReady(VideoCodec::Av1, 2, [&] { return stopped.load(); }); });
    EXPECT_TRUE(state.WaitForWaiter(1s));
    state.PublishVideo(VideoCodec::Av1, {1, 2});
    state.PublishAudio(0, {{}, 13});
    EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
    state.PublishAudio(1, {{}, 17});
    const auto ready = waiter.wait_for(1s);
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready != std::future_status::ready) {
        stopped.store(true);
        state.NotifyStop();
    }
    EXPECT_TRUE(waiter.get());
    const auto codec = state.CodecSnapshot();
    EXPECT_EQ(codec.av1_codec_private, (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(codec.audio_codec_private[1].codec_delay_samples, 17u);
}

TEST(PremuxState, StopReleasesAnActuallyWaitingMuxWithoutInventingReadiness) {
    PremuxState state;
    std::atomic<bool> stopped{false};
    auto waiter = std::async(std::launch::async,
                             [&] { return state.WaitReady(VideoCodec::H264, 1, [&] { return stopped.load(); }); });
    EXPECT_TRUE(state.WaitForWaiter(1s));
    stopped.store(true);
    state.NotifyStop();
    EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
}

TEST(PremuxState, RoutesPreserveBoundsPacketOwnershipAndResetReadiness) {
    PremuxState state;
    EncodedVideoPacket video;
    video.bytes = {1};
    for (size_t i = 0; i < PremuxState::kVideoLimit; ++i) {
        video.bytes = {1};
        ASSERT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Buffered);
    }
    video.bytes = {2};
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Full);
    EXPECT_EQ(video.bytes, (std::vector<uint8_t>{2}));
    auto pending = state.TakePending();
    EXPECT_EQ(pending.video.size(), PremuxState::kVideoLimit);
    EXPECT_TRUE(state.PendingSnapshot().video.empty());
    state.PublishVideo(VideoCodec::Av1, {3});
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Mux);
    EXPECT_EQ(video.bytes, (std::vector<uint8_t>{2}));
    state.Reset();
    EXPECT_FALSE(state.CodecSnapshot().VideoReady(VideoCodec::Av1));
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Buffered);

    EncodedAudioPacket audio;
    uint32_t depth = 0;
    for (size_t i = 0; i < PremuxState::kAudioLimit; ++i) {
        audio.bytes = {4};
        ASSERT_EQ(state.Route(audio, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
        EXPECT_EQ(depth, i + 1);
    }
    audio.bytes = {5};
    EXPECT_EQ(state.Route(audio, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(audio.bytes, (std::vector<uint8_t>{5}));
    state.Reset();
    EXPECT_TRUE(state.PendingSnapshot().audio.empty());
}

TEST(SessionFailureState, ConcurrentWritersPublishOneWholeFailure) {
    for (int iteration = 0; iteration < 128; ++iteration) {
        SessionFailureState state;
        std::barrier start(3);
        std::thread audio([&] {
            start.arrive_and_wait();
            state.RecordFirst({1, ErrorPhase::AudioCapture, "audio"});
        });
        std::thread mux([&] {
            start.arrive_and_wait();
            state.RecordFirst({2, ErrorPhase::Mux, "mux"});
        });
        start.arrive_and_wait();
        audio.join();
        mux.join();
        const auto first = state.Snapshot();
        ASSERT_TRUE(first);
        if (first->error_code == 1) {
            EXPECT_EQ(first->error_phase, ErrorPhase::AudioCapture);
            EXPECT_EQ(first->error_detail, "audio");
        } else {
            EXPECT_EQ(first->error_code, 2);
            EXPECT_EQ(first->error_phase, ErrorPhase::Mux);
            EXPECT_EQ(first->error_detail, "mux");
        }
        state.RecordFirst({3, ErrorPhase::Shutdown, "late"});
        EXPECT_EQ(state.Snapshot()->error_code, first->error_code);
        state.Reset();
        EXPECT_FALSE(state.HasFailure());
    }
}

TEST(SplitRequest, ConcurrentPublicationNeverTearsSequenceAndTrigger) {
    std::atomic<uint64_t> request{0};
    std::barrier step(2);
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 4096; ++i) {
            step.arrive_and_wait();
            request.store(PackSplitRequest({i, i % 4, 0}), std::memory_order_release);
            step.arrive_and_wait();
        }
    });
    for (uint32_t i = 1; i <= 4096; ++i) {
        step.arrive_and_wait();
        const auto racing = UnpackSplitRequest(request.load(std::memory_order_acquire));
        EXPECT_EQ(racing.primary_trigger, racing.sequence % 4);
        step.arrive_and_wait();
        const auto published = UnpackSplitRequest(request.load(std::memory_order_acquire));
        EXPECT_EQ(published.sequence, i);
        EXPECT_EQ(published.primary_trigger, i % 4);
    }
    writer.join();
}

} // namespace
