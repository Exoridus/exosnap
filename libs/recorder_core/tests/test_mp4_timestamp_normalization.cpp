#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Pure-logic mirror of the A/V alignment normalization in mp4_mux_thread.cpp.
// These tests validate the algorithm without requiring a real IMFSinkWriter.
// ---------------------------------------------------------------------------

namespace {

struct MockPacket {
    uint64_t pts_ns;
    uint32_t stream_id; // 0 = video, 1+ = audio
};

// Mirrors the normalization logic in Mp4MuxThread::Run():
//   head_start_ns = (video_epoch_qpc_100ns - session_start_qpc_100ns) * 100
//   Audio packets with pts_ns < head_start_ns are dropped.
//   Remaining audio packets have pts_ns shifted down by head_start_ns.
//   Video packets are unchanged.
static void ApplyMp4Normalization(std::vector<MockPacket>& packets, uint64_t video_epoch_qpc_100ns,
                                  uint64_t session_start_qpc_100ns) {
    if (video_epoch_qpc_100ns <= session_start_qpc_100ns)
        return;

    const uint64_t head_start_ns = (video_epoch_qpc_100ns - session_start_qpc_100ns) * 100ULL;

    auto it = packets.begin();
    while (it != packets.end()) {
        if (it->stream_id != 0) {
            if (it->pts_ns < head_start_ns) {
                it = packets.erase(it);
                continue;
            }
            it->pts_ns -= head_start_ns;
        }
        ++it;
    }
}

// --- Helpers -----------------------------------------------------------------

static uint32_t CountStream(const std::vector<MockPacket>& pkts, uint32_t id) {
    uint32_t n = 0;
    for (const auto& p : pkts) {
        if (p.stream_id == id)
            ++n;
    }
    return n;
}

static const MockPacket* FindFirstOfStream(const std::vector<MockPacket>& pkts, uint32_t id) {
    for (const auto& p : pkts)
        if (p.stream_id == id)
            return &p;
    return nullptr;
}

// --- Tests -------------------------------------------------------------------

// Video packets are never modified regardless of the offset.
TEST(Mp4TimestampNormalization, VideoPacketsUnchanged) {
    // session_start = 1000ms, video_epoch = 1500ms → head_start = 500ms
    const uint64_t session_start = 10000000ULL; // 1000ms in 100ns units
    const uint64_t video_epoch = 15000000ULL;   // 1500ms in 100ns units

    std::vector<MockPacket> packets = {
        {0, 0},        // video at 0ms
        {16666666, 0}, // video at ~16.7ms
        {33333333, 0}, // video at ~33.3ms
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    ASSERT_EQ(packets.size(), 3u);
    EXPECT_EQ(packets[0].pts_ns, 0u);
    EXPECT_EQ(packets[1].pts_ns, 16666666u);
    EXPECT_EQ(packets[2].pts_ns, 33333333u);
}

// Audio packets captured before the first video frame are dropped.
TEST(Mp4TimestampNormalization, AudioBeforeHeadStartDropped) {
    // session_start = 0, video_epoch = 9000000 (900ms in 100ns) → head_start = 900ms in ns
    const uint64_t session_start = 0ULL;
    const uint64_t video_epoch = 9000000ULL; // 900ms in 100ns units
    // head_start_ns = 9000000 * 100 = 900,000,000 ns = 900ms

    std::vector<MockPacket> packets = {
        {0, 1},         // audio at 0ms  → dropped
        {213333333, 1}, // audio at ~213ms → dropped
        {800000000, 1}, // audio at 800ms → dropped (< 900ms)
        {900000000, 1}, // audio at exactly 900ms → kept, normalized to 0
        {913333333, 1}, // audio at ~913ms → kept, normalized to ~13ms
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    EXPECT_EQ(CountStream(packets, 1), 2u);
    const MockPacket* first = FindFirstOfStream(packets, 1);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->pts_ns, 0u); // 900ms - 900ms = 0
}

// Remaining audio packets are shifted to align with video start.
TEST(Mp4TimestampNormalization, AudioAfterHeadStartShifted) {
    // head_start = 500ms
    const uint64_t session_start = 0ULL;
    const uint64_t video_epoch = 5000000ULL; // 500ms in 100ns → head_start = 500,000,000 ns

    const uint64_t head_start_ns = 500000000ULL;

    std::vector<MockPacket> packets = {
        {head_start_ns, 1},            // audio at 500ms → normalized to 0
        {head_start_ns + 21333333, 1}, // audio at 521.3ms → normalized to ~21.3ms
        {head_start_ns + 42666666, 1}, // audio at 542.7ms → normalized to ~42.7ms
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    ASSERT_EQ(packets.size(), 3u);
    EXPECT_EQ(packets[0].pts_ns, 0u);
    EXPECT_EQ(packets[1].pts_ns, 21333333u);
    EXPECT_EQ(packets[2].pts_ns, 42666666u);
}

// When video_epoch == session_start (no WGC delay), no normalization is applied.
TEST(Mp4TimestampNormalization, NoNormalizationWhenEpochEqualsSessionStart) {
    const uint64_t session_start = 10000000ULL;
    const uint64_t video_epoch = 10000000ULL; // same as session start

    std::vector<MockPacket> packets = {
        {0, 0},
        {0, 1},
        {21333333, 1},
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    ASSERT_EQ(packets.size(), 3u);
    EXPECT_EQ(packets[0].pts_ns, 0u);
    EXPECT_EQ(packets[1].pts_ns, 0u);
    EXPECT_EQ(packets[2].pts_ns, 21333333u);
}

// When video_epoch < session_start (should never happen in practice), no normalization.
TEST(Mp4TimestampNormalization, NoNormalizationWhenEpochBeforeSessionStart) {
    const uint64_t session_start = 10000000ULL;
    const uint64_t video_epoch = 5000000ULL; // before session start (defensive)

    std::vector<MockPacket> packets = {
        {0, 1},
        {21333333, 1},
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[0].pts_ns, 0u);
    EXPECT_EQ(packets[1].pts_ns, 21333333u);
}

// Mixed audio and video: video unchanged, early audio dropped, late audio shifted.
TEST(Mp4TimestampNormalization, MixedStreamsAlignCorrectly) {
    // head_start = 900ms
    const uint64_t session_start = 0ULL;
    const uint64_t video_epoch = 9000000ULL; // 900ms
    const uint64_t head_start_ns = 900000000ULL;

    std::vector<MockPacket> packets = {
        {0, 1},                        // audio 0ms → dropped
        {0, 0},                        // video 0ms → unchanged
        {450000000, 1},                // audio 450ms → dropped
        {16666666, 0},                 // video ~16.7ms → unchanged
        {head_start_ns, 1},            // audio 900ms → normalized to 0
        {33333333, 0},                 // video ~33.3ms → unchanged
        {head_start_ns + 21333333, 1}, // audio ~921ms → normalized to ~21ms
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    EXPECT_EQ(CountStream(packets, 0), 3u); // all video kept
    EXPECT_EQ(CountStream(packets, 1), 2u); // only 2 audio kept

    // Video packets unchanged
    uint32_t vidIdx = 0;
    const uint64_t expectedVideo[] = {0, 16666666, 33333333};
    for (const auto& p : packets) {
        if (p.stream_id == 0) {
            EXPECT_EQ(p.pts_ns, expectedVideo[vidIdx++]);
        }
    }

    // Audio starts at 0 after normalization
    const MockPacket* firstAudio = FindFirstOfStream(packets, 1);
    ASSERT_NE(firstAudio, nullptr);
    EXPECT_EQ(firstAudio->pts_ns, 0u);
}

// head_start_ns computation: (video_epoch - session_start) * 100 gives nanoseconds.
TEST(Mp4TimestampNormalization, HeadStartComputationIsCorrect) {
    // video_epoch = session_start + 5000 (500 us in 100ns units = 500,000 ns)
    const uint64_t session_start = 1000000ULL;
    const uint64_t video_epoch = session_start + 5000ULL;     // +500us
    const uint64_t expected_head_start_ns = 5000ULL * 100ULL; // = 500,000 ns

    std::vector<MockPacket> packets = {
        {0, 1},                          // audio before head_start → dropped
        {expected_head_start_ns - 1, 1}, // just before → dropped
        {expected_head_start_ns, 1},     // at boundary → kept, normalized to 0
    };

    ApplyMp4Normalization(packets, video_epoch, session_start);

    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].pts_ns, 0u);
}

} // namespace
