#include "exosnap/engine/packet_types.h"
#include "exosnap/engine/split_trigger_source.h"
#include "mux_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <utility>
#include <variant>

namespace {

using namespace exosnap::engine;
using namespace std::chrono_literals;

MuxItem Packet(size_t bytes = 16) {
    EncodedAudioPacket packet;
    packet.bytes.resize(bytes);
    return MuxItem{std::move(packet)};
}

MuxQueueWait Push(MuxQueue& queue, MuxItem item = Packet()) {
    return queue.Push(std::move(item), [] { return false; }, [] { return false; }, [](auto, double) {});
}

TEST(MuxQueue, PacketAndByteBoundsPreserveAccounting) {
    MuxQueue queue;
    queue.Configure({2, 32, 1ms});
    EXPECT_EQ(Push(queue), MuxQueueWait::Ready);
    EXPECT_EQ(Push(queue), MuxQueueWait::Ready);
    EXPECT_EQ(Push(queue), MuxQueueWait::TimedOut);
    EXPECT_EQ(queue.Size(), 2u);
    EXPECT_EQ(queue.Bytes(), 32u);
    ASSERT_TRUE(queue.Pop());
    EXPECT_EQ(queue.Bytes(), 16u);
    EXPECT_EQ(Push(queue, Packet(32)), MuxQueueWait::Ready);
    EXPECT_EQ(queue.Bytes(), 48u);
    EXPECT_EQ(Push(queue), MuxQueueWait::TimedOut);
    queue.Reset();
    EXPECT_EQ(queue.Size(), 0u);
    EXPECT_EQ(queue.Bytes(), 0u);
    EXPECT_EQ(Push(queue), MuxQueueWait::Ready);
}

TEST(MuxQueue, ByteBoundAppliesBeforePacketBound) {
    MuxQueue queue;
    queue.Configure({1000, 32, 1ms});
    ASSERT_EQ(Push(queue, Packet(32)), MuxQueueWait::Ready);
    EXPECT_EQ(Push(queue), MuxQueueWait::TimedOut);
    EXPECT_EQ(queue.Size(), 1u);
}

TEST(MuxQueue, ConsumerMakesRoomForAnActuallyWaitingProducer) {
    MuxQueue queue;
    queue.Configure({1, 1024, 2s});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    auto producer = std::async(std::launch::async, [&] { return Push(queue); });
    EXPECT_TRUE(queue.WaitForBlockedProducer(1s));
    EXPECT_EQ(producer.wait_for(0ms), std::future_status::timeout);
    EXPECT_TRUE(queue.Pop());
    EXPECT_EQ(producer.get(), MuxQueueWait::Ready);
    EXPECT_EQ(queue.Size(), 1u);
    EXPECT_EQ(queue.Bytes(), 16u);
}

TEST(MuxQueue, FailureInterruptsAnActuallyWaitingProducer) {
    MuxQueue queue;
    queue.Configure({1, 1024, 2s});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    std::atomic<bool> failed{false};
    auto producer = std::async(std::launch::async, [&] {
        return queue.Push(Packet(), [&] { return failed.load(); }, [] { return false; }, [](auto, double) {});
    });
    EXPECT_TRUE(queue.WaitForBlockedProducer(1s));
    failed.store(true);
    queue.NotifyStop();
    EXPECT_EQ(producer.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(producer.get(), MuxQueueWait::Failed);
    EXPECT_EQ(queue.Size(), 1u);
}

TEST(MuxQueue, StopContinuesTheBoundedTailDrainWhenRoomAppears) {
    MuxQueue queue;
    queue.Configure({1, 1024, 2s});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    std::atomic<bool> stopped{false};
    auto producer = std::async(std::launch::async, [&] {
        return queue.Push(Packet(), [] { return false; }, [&] { return stopped.load(); }, [](auto, double) {});
    });
    EXPECT_TRUE(queue.WaitForBlockedProducer(1s));
    stopped.store(true);
    queue.NotifyStop();
    EXPECT_EQ(producer.wait_for(0ms), std::future_status::timeout);
    EXPECT_TRUE(queue.Pop());
    EXPECT_EQ(producer.get(), MuxQueueWait::Ready);
}

TEST(MuxQueue, ExhaustedTailDrainReportsStoppingInsteadOfBackpressure) {
    MuxQueue queue;
    queue.Configure({1, 1024, 1ms});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    EXPECT_EQ(
        queue.Push(Packet(), [] { return false; }, [] { return true; }, [](auto, double) {}), MuxQueueWait::Stopping);
}

TEST(MuxQueue, EosBypassesTheBoundWithoutAddingPayloadBytes) {
    MuxQueue queue;
    queue.Configure({1, 16, 1ms});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    queue.PushSentinel(VideoEosSentinel{});
    queue.PushSentinel(AudioEosSentinel{2});
    EXPECT_EQ(queue.Size(), 3u);
    EXPECT_EQ(queue.Bytes(), 16u);
    ASSERT_TRUE(queue.Pop());
    const auto video = queue.Pop();
    ASSERT_TRUE(video);
    EXPECT_TRUE(std::holds_alternative<VideoEosSentinel>(video->payload));
    const auto audio = queue.Pop();
    ASSERT_TRUE(audio);
    EXPECT_EQ(std::get<AudioEosSentinel>(audio->payload).track_id, 2u);
    EXPECT_EQ(queue.Bytes(), 0u);
}

TEST(MuxQueue, SplitAndKeyframeAreAdjacentEvenAtThePacketBound) {
    MuxQueue queue;
    queue.Configure({1, 1024, 1ms});
    EncodedVideoPacket keyframe;
    keyframe.keyframe = true;
    keyframe.bytes.resize(32);
    EXPECT_EQ(queue.Push(
                  MuxItem{std::move(keyframe)}, [] { return false; }, [] { return false; }, [](auto, double) {},
                  SplitSentinel{7, SplitTriggerSource::Hotkey}),
              MuxQueueWait::Ready);
    EXPECT_EQ(queue.Size(), 2u);
    const auto split = queue.Pop();
    ASSERT_TRUE(split);
    EXPECT_EQ(std::get<SplitSentinel>(split->payload).new_segment_index, 7u);
    const auto video = queue.Pop();
    ASSERT_TRUE(video);
    EXPECT_TRUE(std::get<EncodedVideoPacket>(video->payload).keyframe);
}

TEST(MuxQueue, RejectedPacketDoesNotLeaveAnOrphanSplit) {
    MuxQueue queue;
    queue.Configure({1, 1024, 1ms});
    ASSERT_EQ(Push(queue), MuxQueueWait::Ready);
    EXPECT_EQ(queue.Push(
                  Packet(), [] { return false; }, [] { return false; }, [](auto, double) {},
                  SplitSentinel{1, SplitTriggerSource::ManualButton}),
              MuxQueueWait::TimedOut);
    EXPECT_EQ(queue.Size(), 1u);
    EXPECT_EQ(queue.Bytes(), 16u);
}

TEST(MuxQueue, ConcurrentAudioCannotSeparateSplitFromItsKeyframe) {
    MuxQueue queue;
    std::barrier start(3);
    std::thread audio([&] {
        start.arrive_and_wait();
        for (uint32_t i = 0; i < 256; ++i) {
            EXPECT_EQ(Push(queue), MuxQueueWait::Ready);
        }
    });
    std::thread video([&] {
        start.arrive_and_wait();
        for (uint32_t i = 0; i < 256; ++i) {
            EncodedVideoPacket keyframe;
            keyframe.keyframe = true;
            keyframe.pts_ns = i;
            keyframe.bytes.resize(32);
            EXPECT_EQ(queue.Push(
                          MuxItem{std::move(keyframe)}, [] { return false; }, [] { return false; }, [](auto, double) {},
                          SplitSentinel{i, SplitTriggerSource::Hotkey}),
                      MuxQueueWait::Ready);
        }
    });
    start.arrive_and_wait();
    audio.join();
    video.join();
    size_t splits = 0;
    while (auto item = queue.Pop()) {
        if (const auto* split = std::get_if<SplitSentinel>(&item->payload)) {
            const auto keyframe = queue.Pop();
            ASSERT_TRUE(keyframe);
            const auto* packet = std::get_if<EncodedVideoPacket>(&keyframe->payload);
            ASSERT_NE(packet, nullptr);
            EXPECT_EQ(packet->pts_ns, split->new_segment_index);
            ++splits;
        }
    }
    EXPECT_EQ(splits, 256u);
    EXPECT_EQ(queue.Bytes(), 0u);
}

} // namespace
