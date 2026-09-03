#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "codec_private.h"

namespace {

// Sequence Header OBU as NVENC emits it for a 2560x1440 8-bit 4:2:0 Main
// profile stream: header byte (type 1, has_size), size field 15, payload.
const std::vector<uint8_t> kSequenceHeaderObu = {0x0a, 0x0f, 0x00, 0x00, 0x00, 0x62, 0xea, 0x7f, 0xec,
                                                 0xf8, 0x04, 0x33, 0x20, 0x20, 0x20, 0x20, 0x80};

std::vector<uint8_t> TemporalUnitWithSequenceHeader() {
    std::vector<uint8_t> tu = {0x12, 0x00}; // temporal delimiter, size 0
    tu.insert(tu.end(), kSequenceHeaderObu.begin(), kSequenceHeaderObu.end());
    const uint8_t frame[] = {0x32, 0x03, 0x10, 0x20, 0x30}; // frame OBU, size 3
    tu.insert(tu.end(), frame, frame + sizeof(frame));
    return tu;
}

} // namespace

// The Matroska V_AV1 CodecPrivate is the AV1CodecConfigurationRecord: four
// fixed bytes followed by the Sequence Header OBU as configOBUs, so a demuxer
// can set up the decoder before the first keyframe arrives.
TEST(Av1CodecPrivate, RecordCarriesTheSequenceHeaderObu) {
    const auto tu = TemporalUnitWithSequenceHeader();
    std::vector<uint8_t> cp;
    char reason[128] = {};
    ASSERT_TRUE(exosnap::engine::codec_private::DeriveAv1CodecPrivate(tu.data(), tu.size(), cp, reason, sizeof(reason)))
        << reason;

    ASSERT_EQ(cp.size(), 4u + kSequenceHeaderObu.size());
    EXPECT_EQ(cp[0], 0x81u); // marker | version 1
    EXPECT_EQ(cp[1], 0x0cu); // profile 0, level 5.0
    EXPECT_EQ(cp[2], 0x0cu); // 8-bit, 4:2:0
    EXPECT_EQ(cp[3], 0x00u); // no initial presentation delay
    EXPECT_TRUE(std::equal(kSequenceHeaderObu.begin(), kSequenceHeaderObu.end(), cp.begin() + 4));
}

TEST(Av1CodecPrivate, PacketWithoutSequenceHeaderIsRejected) {
    const std::vector<uint8_t> tu = {0x12, 0x00, 0x32, 0x03, 0x10, 0x20, 0x30};
    std::vector<uint8_t> cp;
    char reason[128] = {};
    EXPECT_FALSE(
        exosnap::engine::codec_private::DeriveAv1CodecPrivate(tu.data(), tu.size(), cp, reason, sizeof(reason)));
    EXPECT_TRUE(cp.empty());
}
