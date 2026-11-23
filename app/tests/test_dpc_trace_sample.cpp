#include "diagnostics/DpcTraceSample.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace exosnap::diagnostics {
namespace {

std::array<uint8_t, 20> Payload(int64_t begin_qpc, uint64_t routine) {
    std::array<uint8_t, 20> bytes{};
    std::memcpy(bytes.data(), &begin_qpc, sizeof(begin_qpc));
    std::memcpy(bytes.data() + sizeof(begin_qpc), &routine, sizeof(routine));
    return bytes;
}

TEST(DpcTraceSample, DecodesRawQpcAcrossVariantsAndPointerWidths) {
    constexpr uint64_t routine = 0x1234abcd9876fedc;
    const auto payload = Payload(10000, routine);
    const auto threaded = DecodeDpcTraceSample(std::span(payload).first(12), 66, 4, 10300, 3200000);
    ASSERT_TRUE(threaded);
    EXPECT_DOUBLE_EQ(threaded->elapsed_us, 93.75);
    EXPECT_EQ(threaded->routine, 0x9876fedcu);

    const auto interrupt = DecodeDpcTraceSample(payload, 67, 8, 11000, 10000000);
    ASSERT_TRUE(interrupt);
    EXPECT_DOUBLE_EQ(interrupt->elapsed_us, 100.0);
    EXPECT_EQ(interrupt->routine, routine);

    for (const uint8_t opcode : std::array<uint8_t, 2>{68, 69}) {
        const auto sample = DecodeDpcTraceSample(std::span(payload).first(16), opcode, 8, 10007, 1000000);
        ASSERT_TRUE(sample);
        EXPECT_DOUBLE_EQ(sample->elapsed_us, 7.0);
        EXPECT_EQ(sample->routine, routine);
    }
}

TEST(DpcTraceSample, RejectsTruncationAndInvalidClockFacts) {
    const auto valid = Payload(100, 0xabcd);
    EXPECT_FALSE(DecodeDpcTraceSample(std::span(valid).first(15), 68, 8, 200, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(std::span(valid).first(19), 67, 8, 200, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 70, 8, 200, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 68, 2, 200, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 68, 8, 200, 0));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 68, 8, 200, -1));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 68, 8, 99, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(valid, 68, 8, -1, 10000000));
    EXPECT_FALSE(DecodeDpcTraceSample(Payload(0, 0xabcd), 68, 8, 200, 10000000));
}

} // namespace
} // namespace exosnap::diagnostics
