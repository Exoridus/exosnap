#include "fakes/fake_video_encoder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace exosnap::engine;
using testutil::FakeVideoEncoder;

TEST(EncoderInputSlots, OutputCompletionDoesNotGrantInputReuseOrAdvanceABusySubmission) {
    FakeVideoEncoder encoder(1);
    encoder.retain_input_after_encode = true;
    const auto first = encoder.AcquireInputSlot();
    ASSERT_EQ(first.status, EncoderInputSlotStatus::Available);
    ASSERT_EQ(first.index, 0);
    std::string error;
    std::vector<EncodedVideoPacket> packets;
    ASSERT_TRUE(encoder.EncodeFrame(first.index, 100, 1280, 720, packets, error));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(encoder.PendingFrames(), 0u);
    encoder.RequestKeyframe();

    const auto busy = encoder.AcquireInputSlot();
    ASSERT_EQ(busy.status, EncoderInputSlotStatus::Busy);
    EXPECT_EQ(busy.index, -1);
    EXPECT_EQ(encoder.EncodeFrameCallCount(), 1);
    EXPECT_EQ(encoder.LastPtsNs(), 100u);

    // Models a backend's input-release notification, independent of its output.
    encoder.CompleteInput(first.index);
    const auto reusable = encoder.AcquireInputSlot();
    ASSERT_EQ(reusable.status, EncoderInputSlotStatus::Available);
    ASSERT_EQ(reusable.index, 0);
    packets.clear();
    ASSERT_TRUE(encoder.EncodeFrame(reusable.index, 200, 1280, 720, packets, error));
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets.front().pts_ns, 200u);
    EXPECT_TRUE(packets.front().keyframe);
    EXPECT_EQ(encoder.EncodeFrameCallCount(), 2);
}

class InvalidSlotEncoder : public FakeVideoEncoder {
  public:
    explicit InvalidSlotEncoder(int32_t index) : FakeVideoEncoder(4), index_(index) {
    }
    int32_t AcquireFreeSlot() override {
        return index_;
    }

  private:
    int32_t index_;
};

TEST(EncoderInputSlots, InvalidBackendSlotIsNeitherWritableNorOrdinaryBackpressure) {
    for (const int32_t index : {-2, 4, std::numeric_limits<int32_t>::max()}) {
        SCOPED_TRACE(index);
        InvalidSlotEncoder encoder(index);
        const auto slot = encoder.AcquireInputSlot();
        EXPECT_EQ(slot.status, EncoderInputSlotStatus::Invalid);
        EXPECT_EQ(slot.index, -1);
    }
}

} // namespace
