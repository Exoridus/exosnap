#include "encoder_packet_dispatch.h"
#include "fakes/fake_video_encoder.h"
#include "mux_queue.h"

#include <exosnap/engine/packet_types.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace exosnap::engine;
using exosnap::engine::testutil::FakeVideoEncoder;
using namespace std::chrono_literals;

enum class EncoderCall { Encode, Reap, Flush };

EncodedVideoPacket Packet(uint8_t byte, uint64_t pts, int64_t dts, bool keyframe = false) {
    EncodedVideoPacket packet;
    packet.bytes = {byte};
    packet.pts_ns = pts;
    packet.dts_ns = dts;
    packet.keyframe = keyframe;
    packet.encode_latency_ms = 2.5;
    packet.keyframe_prediction_mismatch = true;
    return packet;
}

bool Push(MuxQueue& queue, EncodedVideoPacket packet, std::string& error) {
    const auto result =
        queue.Push(MuxItem{std::move(packet)}, [] { return false; }, [] { return false; }, [](auto, double) {});
    if (result == MuxQueueWait::Ready)
        return true;
    error = "output queue timed out";
    return false;
}

class EncoderPacketDispatchFailure : public testing::TestWithParam<EncoderCall> {
  protected:
    FakeVideoEncoder encoder;
    std::vector<EncodedVideoPacket> packets;
    std::string error;

    bool CompleteThenFail() {
        std::vector<EncodedVideoPacket> completed{Packet(0x11, 0, -10, true), Packet(0x22, 30, 0)};
        encoder.forced_error_message = "encoder lost its device";
        switch (GetParam()) {
        case EncoderCall::Encode:
            encoder.force_encode_fail = true;
            encoder.encode_completions = std::move(completed);
            return encoder.EncodeFrame(encoder.AcquireFreeSlot(), 40, 1280, 720, packets, error);
        case EncoderCall::Reap:
            encoder.force_reap_fail = true;
            encoder.reap_completions = std::move(completed);
            return encoder.ReapCompleted(packets, error, 4);
        case EncoderCall::Flush:
            encoder.force_flush_fail = true;
            encoder.flush_completions = std::move(completed);
            return encoder.Flush(packets, error);
        }
        return false;
    }
};

TEST_P(EncoderPacketDispatchFailure, RoutesCompletedPacketsOnceInDecodeOrderBeforeReturningFailure) {
    MuxQueue queue;
    const auto route = [&](EncodedVideoPacket packet, std::string& route_error) {
        return Push(queue, std::move(packet), route_error);
    };

    const bool encoder_ok = CompleteThenFail();
    EXPECT_FALSE(RouteEncoderPackets(encoder_ok, packets, error, route));
    EXPECT_EQ(error, "encoder lost its device");
    EXPECT_TRUE(packets.empty());
    ASSERT_EQ(queue.Size(), 2u);

    // A subsequent bounded drain appends into the same caller-owned buffer.
    // Already transferred packets must not be routed again.
    encoder.force_flush_fail = false;
    encoder.flush_completions = {Packet(0x33, 10, 10)};
    const bool flushed = encoder.Flush(packets, error);
    EXPECT_TRUE(RouteEncoderPackets(flushed, packets, error, route));
    EXPECT_TRUE(packets.empty());
    ASSERT_EQ(queue.Size(), 3u);

    for (const auto& expected : {Packet(0x11, 0, -10, true), Packet(0x22, 30, 0), Packet(0x33, 10, 10)}) {
        auto item = queue.Pop();
        ASSERT_TRUE(item.has_value());
        const auto* actual = std::get_if<EncodedVideoPacket>(&item->payload);
        ASSERT_NE(actual, nullptr);
        EXPECT_EQ(actual->bytes, expected.bytes);
        EXPECT_EQ(actual->pts_ns, expected.pts_ns);
        EXPECT_EQ(actual->dts_ns, expected.dts_ns);
        EXPECT_EQ(actual->keyframe, expected.keyframe);
        EXPECT_EQ(actual->encode_latency_ms, 2.5);
        EXPECT_TRUE(actual->keyframe_prediction_mismatch);
    }
    EXPECT_FALSE(queue.Pop().has_value());
}

TEST_P(EncoderPacketDispatchFailure, KeepsEncoderErrorWhenBoundedMuxWaitAlsoFails) {
    MuxQueue queue;
    queue.Configure({1, 1024, 1ms});
    ASSERT_TRUE(Push(queue, Packet(0x44, 0, -20), error));
    const bool encoder_ok = CompleteThenFail();
    size_t attempted = 0;
    const auto started = std::chrono::steady_clock::now();
    const bool result =
        RouteEncoderPackets(encoder_ok, packets, error, [&](EncodedVideoPacket packet, std::string& route_error) {
            ++attempted;
            return Push(queue, std::move(packet), route_error);
        });
    EXPECT_FALSE(result);
    EXPECT_EQ(attempted, 1u);
    EXPECT_EQ(error, "encoder lost its device");
    EXPECT_TRUE(packets.empty());
    EXPECT_EQ(queue.Size(), 1u);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

INSTANTIATE_TEST_SUITE_P(EncoderOperations, EncoderPacketDispatchFailure,
                         testing::Values(EncoderCall::Encode, EncoderCall::Reap, EncoderCall::Flush),
                         [](const testing::TestParamInfo<EncoderCall>& info) {
                             switch (info.param) {
                             case EncoderCall::Encode:
                                 return "Encode";
                             case EncoderCall::Reap:
                                 return "Reap";
                             case EncoderCall::Flush:
                                 return "Flush";
                             }
                             return "Unknown";
                         });

TEST(EncoderPacketDispatch, MuxFailureAfterSuccessfulEncodeDoesNotReplayAcceptedPackets) {
    FakeVideoEncoder encoder;
    encoder.encode_completions = {Packet(0x11, 0, -10, true), Packet(0x22, 30, 0)};
    std::vector<EncodedVideoPacket> packets;
    std::string error;
    const bool encoder_ok = encoder.EncodeFrame(encoder.AcquireFreeSlot(), 40, 1280, 720, packets, error);
    MuxQueue queue;
    queue.Configure({1, 1024, 1ms});
    const auto route = [&](EncodedVideoPacket packet, std::string& route_error) {
        return Push(queue, std::move(packet), route_error);
    };
    EXPECT_FALSE(RouteEncoderPackets(encoder_ok, packets, error, route));
    EXPECT_EQ(error, "output queue timed out");
    EXPECT_TRUE(packets.empty());
    ASSERT_EQ(queue.Size(), 1u);
    auto first = queue.Pop();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(std::get<EncodedVideoPacket>(first->payload).bytes, std::vector<uint8_t>{0x11});

    const bool flushed = encoder.Flush(packets, error);
    EXPECT_TRUE(RouteEncoderPackets(flushed, packets, error, route));
    EXPECT_EQ(queue.Size(), 0u);
}

TEST(EncoderPacketDispatch, EmptyBatchesKeepTheEncoderStatusWithoutCallingTheSink) {
    std::vector<EncodedVideoPacket> packets;
    std::string error;
    const auto route = [](EncodedVideoPacket, std::string&) {
        ADD_FAILURE() << "A buffered or failed empty encoder call has no packet to route";
        return false;
    };
    EXPECT_TRUE(RouteEncoderPackets(true, packets, error, route));
    EXPECT_TRUE(error.empty());
    error = "encoder rejected EOS";
    EXPECT_FALSE(RouteEncoderPackets(false, packets, error, route));
    EXPECT_EQ(error, "encoder rejected EOS");
}

} // namespace
