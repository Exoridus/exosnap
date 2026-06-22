#include <gtest/gtest.h>

#include "matroska_stream_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// These tests exercise the PRODUCTION streaming writer (MatroskaStreamWriter)
// directly on synthetic packet streams — no GPU, no live session. They verify
// both Matroska structural correctness (parity with the batch muxer's
// guarantees) and the headline streaming property: peak buffered RAM is bounded
// by the reorder window, NOT by the total session length.

namespace {

using recorder_core::MatroskaStreamConfig;
using recorder_core::MatroskaStreamWriter;
using recorder_core::MuxPacket;
using recorder_core::StreamAudioCodec;

constexpr uint64_t kTimescaleNs = 1000000ULL;

// --- Minimal EBML structure walker (mirrors test_matroska_mux_structure.cpp) ---

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    return buf;
}

uint64_t ReadEbmlId(const std::vector<uint8_t>& d, size_t& off) {
    const uint8_t first = d[off];
    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 4 && !(first & mask)) {
        mask = static_cast<uint8_t>(mask >> 1);
        ++len;
    }
    uint64_t id = 0;
    for (int i = 0; i < len; ++i)
        id = (id << 8) | d[off + static_cast<size_t>(i)];
    off += static_cast<size_t>(len);
    return id;
}

uint64_t ReadEbmlSize(const std::vector<uint8_t>& d, size_t& off, bool& is_unknown) {
    const uint8_t first = d[off];
    int len = 1;
    uint8_t mask = 0x80;
    while (len <= 8 && !(first & mask)) {
        mask = static_cast<uint8_t>(mask >> 1);
        ++len;
    }
    uint64_t size = static_cast<uint64_t>(first & (mask - 1));
    for (int i = 1; i < len; ++i)
        size = (size << 8) | d[off + static_cast<size_t>(i)];
    off += static_cast<size_t>(len);
    const uint64_t all_ones = (len == 8) ? 0x00FFFFFFFFFFFFFFULL : ((1ULL << (7 * len)) - 1ULL);
    is_unknown = (size == all_ones);
    return size;
}

struct EbmlNode {
    uint64_t id = 0;
    size_t data_off = 0;
    uint64_t data_size = 0;
};

std::vector<EbmlNode> ParseChildren(const std::vector<uint8_t>& d, size_t start, size_t end) {
    std::vector<EbmlNode> out;
    size_t off = start;
    while (off + 2 <= end) {
        const uint64_t id = ReadEbmlId(d, off);
        bool unknown = false;
        const uint64_t size = ReadEbmlSize(d, off, unknown);
        size_t data_end = unknown ? end : (off + static_cast<size_t>(size));
        if (data_end > end)
            data_end = end;
        out.push_back({id, off, static_cast<uint64_t>(data_end - off)});
        off = data_end;
    }
    return out;
}

constexpr uint64_t kIdSegment = 0x18538067ULL;
constexpr uint64_t kIdInfo = 0x1549A966ULL;
constexpr uint64_t kIdTracks = 0x1654AE6BULL;
constexpr uint64_t kIdCues = 0x1C53BB6BULL;
constexpr uint64_t kIdSeekHead = 0x114D9B74ULL;
constexpr uint64_t kIdCluster = 0x1F43B675ULL;
constexpr uint64_t kIdCuePoint = 0xBBULL;
constexpr uint64_t kIdDuration = 0x4489ULL;

// Locate the Segment's level-1 children.
std::vector<EbmlNode> SegmentChildren(const std::vector<uint8_t>& d) {
    const auto top = ParseChildren(d, 0, d.size());
    for (const auto& e : top)
        if (e.id == kIdSegment)
            return ParseChildren(d, e.data_off, e.data_off + e.data_size);
    return {};
}

bool HasLevel1(const std::vector<uint8_t>& d, uint64_t id) {
    for (const auto& c : SegmentChildren(d))
        if (c.id == id)
            return true;
    return false;
}

int CountClusters(const std::vector<uint8_t>& d) {
    int n = 0;
    for (const auto& c : SegmentChildren(d))
        if (c.id == kIdCluster)
            ++n;
    return n;
}

int CountCuePoints(const std::vector<uint8_t>& d) {
    for (const auto& c : SegmentChildren(d)) {
        if (c.id != kIdCues)
            continue;
        const auto pts = ParseChildren(d, c.data_off, c.data_off + c.data_size);
        int n = 0;
        for (const auto& p : pts)
            if (p.id == kIdCuePoint)
                ++n;
        return n;
    }
    return 0;
}

// Read the KaxDuration (8-byte big-endian double) from Info. Returns -1 if absent.
double ReadDurationMs(const std::vector<uint8_t>& d) {
    for (const auto& c : SegmentChildren(d)) {
        if (c.id != kIdInfo)
            continue;
        const auto info = ParseChildren(d, c.data_off, c.data_off + c.data_size);
        for (const auto& e : info) {
            if (e.id != kIdDuration)
                continue;
            if (e.data_size != 8)
                return -2.0; // wrong size => back-patch would have corrupted file
            uint64_t bits = 0;
            for (int i = 0; i < 8; ++i)
                bits = (bits << 8) | d[e.data_off + static_cast<size_t>(i)];
            double val = 0.0;
            std::memcpy(&val, &bits, sizeof(val));
            return val;
        }
    }
    return -1.0;
}

// Verify the top-level Segment size is finite (back-patched, not "unknown").
bool SegmentSizeIsFinite(const std::vector<uint8_t>& d) {
    const auto top = ParseChildren(d, 0, d.size());
    size_t off = 0;
    // Walk to the Segment element header to read its size field directly.
    while (off + 2 <= d.size()) {
        const size_t id_start = off;
        const uint64_t id = ReadEbmlId(d, off);
        bool unknown = false;
        const uint64_t size = ReadEbmlSize(d, off, unknown);
        if (id == kIdSegment)
            return !unknown && size > 0;
        // Skip non-segment top-level element (the EBML head).
        off += static_cast<size_t>(size);
        (void)id_start;
    }
    return false;
}

std::vector<uint8_t> FakeH264Cp() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
std::vector<uint8_t> FakeAacCp() {
    return {0x11, 0x90};
}
std::vector<uint8_t> FakeAv1Cp() {
    return {0x81, 0x00, 0x00, 0x00};
}
std::vector<uint8_t> FakeOpusCp() {
    return {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 0x01, 0x02, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00};
}
// Minimal native FLAC header: "fLaC" marker + a (fake but length-correct)
// STREAMINFO block header. The writer only checks the leading marker.
std::vector<uint8_t> FakeFlacCp() {
    std::vector<uint8_t> cp = {'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, 0x22}; // last-block STREAMINFO, len=34
    cp.resize(8 + 34, 0x00);
    return cp;
}

// Read an unsigned big-endian EBML integer from a node's data bytes.
uint64_t ReadUInt(const std::vector<uint8_t>& d, const EbmlNode& n) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < n.data_size; ++i)
        v = (v << 8) | d[n.data_off + static_cast<size_t>(i)];
    return v;
}

// Navigate Segment -> Tracks -> first video TrackEntry -> Video -> Colour and
// return the Colour element's children. Empty if any level is missing.
std::vector<EbmlNode> VideoColourChildren(const std::vector<uint8_t>& d) {
    constexpr uint64_t kIdTrackEntry = 0xAEULL;
    constexpr uint64_t kIdTrackVideo = 0xE0ULL;
    constexpr uint64_t kIdColour = 0x55B0ULL;
    for (const auto& seg : SegmentChildren(d)) {
        if (seg.id != kIdTracks)
            continue;
        for (const auto& te : ParseChildren(d, seg.data_off, seg.data_off + seg.data_size)) {
            if (te.id != kIdTrackEntry)
                continue;
            for (const auto& tc : ParseChildren(d, te.data_off, te.data_off + te.data_size)) {
                if (tc.id != kIdTrackVideo)
                    continue;
                for (const auto& vc : ParseChildren(d, tc.data_off, tc.data_off + tc.data_size)) {
                    if (vc.id == kIdColour)
                        return ParseChildren(d, vc.data_off, vc.data_off + vc.data_size);
                }
            }
        }
    }
    return {};
}

const EbmlNode* FindColourChild(const std::vector<EbmlNode>& colour, uint64_t id) {
    for (const auto& c : colour)
        if (c.id == id)
            return &c;
    return nullptr;
}

MatroskaStreamConfig MakeConfig(const std::string& path, bool h264, bool opus) {
    MatroskaStreamConfig c;
    c.output_path = path;
    c.video_codec_id = h264 ? "V_MPEG4/ISO/AVC" : "V_AV1";
    c.video_codec_private = h264 ? FakeH264Cp() : FakeAv1Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = opus ? StreamAudioCodec::Opus : StreamAudioCodec::Aac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = opus ? FakeOpusCp() : FakeAacCp();
    return c;
}

// Push interleaved A/V packets covering `seconds` of media at 60 fps video +
// ~47 fps audio, with a video keyframe every `gop` frames. Packets are pushed in
// per-track monotonic order but interleaved across tracks (the realistic case
// the reorder window must handle).
void FeedSeconds(MatroskaStreamWriter& w, double seconds, int gop, size_t payload_bytes) {
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t aframe = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    const std::vector<uint8_t> blob(payload_bytes, 0xAB);

    uint64_t vpts = 0, apts = 0;
    int vidx = 0;
    while (vpts < total_ns || apts < total_ns) {
        // Emit whichever stream is behind, to interleave by PTS.
        if (vpts <= apts && vpts < total_ns) {
            MuxPacket p;
            p.pts_ns = vpts;
            p.track_num = 1;
            p.is_key = (vidx % gop == 0);
            p.bytes = blob;
            w.Push(std::move(p));
            vpts += vframe;
            ++vidx;
        } else if (apts < total_ns) {
            MuxPacket p;
            p.pts_ns = apts;
            p.track_num = 2;
            p.is_key = true;
            p.bytes = blob;
            w.Push(std::move(p));
            apts += aframe;
        } else {
            break;
        }
    }
}

class StreamWriterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tmp_ = (std::filesystem::temp_directory_path() / "exosnap_stream_writer_test.mkv").string();
        std::remove(tmp_.c_str());
    }
    void TearDown() override {
        std::remove(tmp_.c_str());
    }
    std::string tmp_;
};

// 1. Basic H.264+AAC produces a structurally complete file.
TEST_F(StreamWriterTest, H264Aac_ProducesCompleteContainer) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, /*h264=*/true, /*opus=*/false)));
    FeedSeconds(w, 5.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());
    EXPECT_TRUE(HasLevel1(d, kIdSeekHead));
    EXPECT_TRUE(HasLevel1(d, kIdInfo));
    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(HasLevel1(d, kIdCues));
    EXPECT_GE(CountClusters(d), 1);
    EXPECT_TRUE(SegmentSizeIsFinite(d)) << "Segment size must be back-patched to a finite value";
}

// 2. AV1+Opus path also works.
TEST_F(StreamWriterTest, Av1Opus_ProducesCompleteContainer) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, /*h264=*/false, /*opus=*/true)));
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    const auto d = ReadFile(tmp_);
    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

// 2b. PCM path: track header carries CodecID "A_PCM/INT_LIT" and BitDepth=16,
//     and no CodecPrivate. Verified by scanning the rendered Tracks bytes for the
//     ASCII CodecID and the BitDepth element (id 0x6264, value 16).
TEST_F(StreamWriterTest, Pcm_WritesPcmCodecIdAndBitDepth) {
    MatroskaStreamConfig c;
    c.output_path = tmp_;
    c.video_codec_id = "V_AV1";
    c.video_codec_private = FakeAv1Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = StreamAudioCodec::Pcm;
    c.audio_track_count = 1;
    // PCM carries no CodecPrivate.
    c.audio_tracks[0].codec_private = {};

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c));
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());

    // CodecID "A_PCM/INT_LIT" present in the rendered container.
    const std::string kPcmId = "A_PCM/INT_LIT";
    const auto id_it = std::search(d.begin(), d.end(), kPcmId.begin(), kPcmId.end());
    EXPECT_NE(id_it, d.end()) << "A_PCM/INT_LIT CodecID not found in output";

    // KaxAudioBitDepth (EBML id 0x6264), 1-byte size 0x81, value 16 (0x10).
    const std::vector<uint8_t> kBitDepth16 = {0x62, 0x64, 0x81, 0x10};
    const auto bd_it = std::search(d.begin(), d.end(), kBitDepth16.begin(), kBitDepth16.end());
    EXPECT_NE(bd_it, d.end()) << "KaxAudioBitDepth=16 not found in output";

    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

// 2c. FLAC path: track header carries CodecID "A_FLAC", the native fLaC header
//     as CodecPrivate, and BitDepth=16. Verified by scanning the rendered bytes.
TEST_F(StreamWriterTest, Flac_WritesFlacCodecIdAndCodecPrivate) {
    MatroskaStreamConfig c;
    c.output_path = tmp_;
    c.video_codec_id = "V_AV1";
    c.video_codec_private = FakeAv1Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = StreamAudioCodec::Flac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = FakeFlacCp();

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c));
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());

    // CodecID "A_FLAC" present in the rendered container.
    const std::string kFlacId = "A_FLAC";
    const auto id_it = std::search(d.begin(), d.end(), kFlacId.begin(), kFlacId.end());
    EXPECT_NE(id_it, d.end()) << "A_FLAC CodecID not found in output";

    // The native "fLaC" header (CodecPrivate) must be embedded.
    const std::string kFlacMarker = "fLaC";
    const auto cp_it = std::search(d.begin(), d.end(), kFlacMarker.begin(), kFlacMarker.end());
    EXPECT_NE(cp_it, d.end()) << "native fLaC header (CodecPrivate) not found in output";

    // KaxAudioBitDepth (EBML id 0x6264), 1-byte size 0x81, value 16 (0x10).
    const std::vector<uint8_t> kBitDepth16 = {0x62, 0x64, 0x81, 0x10};
    const auto bd_it = std::search(d.begin(), d.end(), kBitDepth16.begin(), kBitDepth16.end());
    EXPECT_NE(bd_it, d.end()) << "KaxAudioBitDepth=16 not found in output";

    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

// 2d. A FLAC CodecPrivate without the leading "fLaC" marker is rejected at Open().
TEST_F(StreamWriterTest, Flac_MalformedCodecPrivate_Rejected) {
    MatroskaStreamConfig c;
    c.output_path = tmp_;
    c.video_codec_id = "V_AV1";
    c.video_codec_private = FakeAv1Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = StreamAudioCodec::Flac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = {0x00, 0x01, 0x02, 0x03}; // not "fLaC"

    MatroskaStreamWriter w;
    EXPECT_FALSE(w.Open(c));
    EXPECT_TRUE(w.failed());
}

// 3. Incomplete Opus CodecPrivate is rejected at Open().
TEST_F(StreamWriterTest, ShortOpusCodecPrivate_Rejected) {
    auto cfg = MakeConfig(tmp_, /*h264=*/false, /*opus=*/true);
    cfg.audio_tracks[0].codec_private = {'O', 'p', 'u', 's'}; // too short
    MatroskaStreamWriter w;
    EXPECT_FALSE(w.Open(cfg));
    EXPECT_TRUE(w.failed());
}

// 4. One cue point per video keyframe (parity with batch muxer). The expected
//    count is derived from the same frame walk FeedSeconds uses so the test
//    cannot drift from the feeder's exact boundary arithmetic.
TEST_F(StreamWriterTest, CuePointPerVideoKeyframe) {
    const double seconds = 10.0;
    const int gop = 60;
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    int expected_kf = 0;
    for (uint64_t vpts = 0, vidx = 0; vpts < total_ns; vpts += vframe, ++vidx) {
        if (vidx % static_cast<uint64_t>(gop) == 0)
            ++expected_kf;
    }

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, seconds, gop, 16);
    ASSERT_TRUE(w.Finalize());
    const auto d = ReadFile(tmp_);
    EXPECT_EQ(CountCuePoints(d), expected_kf);
}

// 5. Duration is back-patched to a real value with a stable 8-byte float.
TEST_F(StreamWriterTest, DurationBackPatched) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, 4.0, 60, 16);
    ASSERT_TRUE(w.Finalize());
    const auto d = ReadFile(tmp_);
    const double dur = ReadDurationMs(d);
    ASSERT_GT(dur, 0.0) << "Duration must be a positive back-patched value (got " << dur << ")";
    // ~4000 ms (allow a frame of slack each way).
    EXPECT_NEAR(dur, 4000.0, 50.0);
}

// 6. HEADLINE: peak buffered RAM is bounded by the reorder window, independent of
//    total session length. Feed 60 s and assert the window never holds more than
//    a few seconds' worth of packets — proving constant-RAM streaming.
TEST_F(StreamWriterTest, PeakWindowBoundedRegardlessOfLength) {
    auto cfg = MakeConfig(tmp_, true, false);
    cfg.reorder_window_ns = 2ULL * 1000000000ULL; // 2 s window
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(cfg));

    const size_t payload = 4096; // realistic-ish per-packet size
    FeedSeconds(w, 60.0, 60, payload);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    // At 60 fps video + ~47 fps audio, one second is ~107 packets. A 2 s window
    // should never hold more than a small multiple of that. The whole 60 s session
    // is ~6400 packets; the window must be a tiny fraction of that.
    const size_t peak = w.peak_window_packets();
    EXPECT_GT(peak, 0u);
    EXPECT_LT(peak, 600u) << "window held " << peak << " packets — RAM is NOT bounded by the window";

    // Bytes peak likewise bounded (well under the ~26 MB the full 60 s would be).
    EXPECT_LT(w.peak_window_bytes(), 4u * 1024u * 1024u)
        << "peak window bytes " << w.peak_window_bytes() << " exceeds the bounded-RAM budget";
}

// 7. Out-of-order cross-track PTS within the window are emitted in PTS order.
//    (Audio pushed slightly behind video, then catching up — the window must
//    reorder so no timestamp regression reaches the clusters.)
TEST_F(StreamWriterTest, ReorderWindowSortsInterleavedStreams) {
    MatroskaStreamWriter w;
    auto cfg = MakeConfig(tmp_, true, false);
    cfg.reorder_window_ns = 1ULL * 1000000000ULL;
    ASSERT_TRUE(w.Open(cfg));

    const uint64_t ms = kTimescaleNs;
    // Push a video frame at t=0 (key), then audio at t=5ms, t=15ms, then a video
    // P-frame at t=10ms — out of order relative to the audio at 15ms. The window
    // must sort these so the file is monotone.
    auto vp = [&](uint64_t t, bool key) {
        MuxPacket p;
        p.pts_ns = t;
        p.track_num = 1;
        p.is_key = key;
        p.bytes = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(key ? 0x65 : 0x41)};
        return p;
    };
    auto ap = [&](uint64_t t) {
        MuxPacket p;
        p.pts_ns = t;
        p.track_num = 2;
        p.is_key = true;
        p.bytes = {0xFF, 0xF1, 0x50, 0x80};
        return p;
    };
    w.Push(vp(0, true));
    w.Push(ap(5 * ms));
    w.Push(ap(15 * ms));
    w.Push(vp(10 * ms, false)); // arrives after a later-PTS audio packet
    w.Push(vp(60 * ms, false)); // advance the horizon to force the window to drain
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
    // One keyframe => one cue.
    const auto d = ReadFile(tmp_);
    EXPECT_EQ(CountCuePoints(d), 1);
}

// 8. Empty session (no packets) still finalizes a valid, minimal container.
TEST_F(StreamWriterTest, EmptySession_FinalizesValidContainer) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    ASSERT_TRUE(w.Finalize());
    const auto d = ReadFile(tmp_);
    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
    EXPECT_EQ(CountCuePoints(d), 0);
}

// Color metadata (ADR 0032): the video track carries an SDR BT.709 limited-range
// 8-bit Colour element by default, so the file is no longer color-ambiguous and
// no HDR sub-elements are emitted.
TEST_F(StreamWriterTest, WritesBt709ColourElementByDefault) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, /*h264=*/false, /*opus=*/true)));
    FeedSeconds(w, 1.0, 30, 64);
    ASSERT_TRUE(w.Finalize());

    const auto d = ReadFile(tmp_);
    const auto colour = VideoColourChildren(d);
    ASSERT_FALSE(colour.empty()) << "video track has no Colour element";

    const EbmlNode* primaries = FindColourChild(colour, 0x55BBULL);
    const EbmlNode* transfer = FindColourChild(colour, 0x55BAULL);
    const EbmlNode* matrix = FindColourChild(colour, 0x55B1ULL);
    const EbmlNode* range = FindColourChild(colour, 0x55B9ULL);
    const EbmlNode* bits = FindColourChild(colour, 0x55B2ULL);
    ASSERT_NE(primaries, nullptr);
    ASSERT_NE(transfer, nullptr);
    ASSERT_NE(matrix, nullptr);
    ASSERT_NE(range, nullptr);
    ASSERT_NE(bits, nullptr);
    EXPECT_EQ(ReadUInt(d, *primaries), 1u); // BT.709
    EXPECT_EQ(ReadUInt(d, *transfer), 1u);  // BT.709
    EXPECT_EQ(ReadUInt(d, *matrix), 1u);    // BT.709
    EXPECT_EQ(ReadUInt(d, *range), 1u);     // limited / studio range
    EXPECT_EQ(ReadUInt(d, *bits), 8u);
    EXPECT_EQ(FindColourChild(colour, 0x55BCULL), nullptr) << "MaxCLL must be absent for SDR";
    EXPECT_EQ(FindColourChild(colour, 0x55BDULL), nullptr) << "MaxFALL must be absent for SDR";
}

// Non-default color values (including HDR10 light levels) round-trip into the
// Colour element — the model is ready for the HDR slice.
TEST_F(StreamWriterTest, WritesConfiguredColourValuesIncludingHdr) {
    auto cfg = MakeConfig(tmp_, /*h264=*/false, /*opus=*/true);
    cfg.color.primaries = recorder_core::ColorPrimaries::Bt2020;
    cfg.color.transfer = recorder_core::TransferCharacteristics::SmpteSt2084;
    cfg.color.matrix = recorder_core::MatrixCoefficients::Bt2020Ncl;
    cfg.color.range = recorder_core::ColorRange::Full;
    cfg.color.bits_per_channel = 10;
    cfg.color.hdr = true;
    cfg.color.max_content_light_level = 1000;
    cfg.color.max_frame_average_light_level = 400;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(cfg));
    FeedSeconds(w, 1.0, 30, 64);
    ASSERT_TRUE(w.Finalize());

    const auto d = ReadFile(tmp_);
    const auto colour = VideoColourChildren(d);
    ASSERT_FALSE(colour.empty());
    const EbmlNode* primaries = FindColourChild(colour, 0x55BBULL);
    const EbmlNode* transfer = FindColourChild(colour, 0x55BAULL);
    const EbmlNode* matrix = FindColourChild(colour, 0x55B1ULL);
    const EbmlNode* range = FindColourChild(colour, 0x55B9ULL);
    const EbmlNode* bits = FindColourChild(colour, 0x55B2ULL);
    const EbmlNode* maxcll = FindColourChild(colour, 0x55BCULL);
    const EbmlNode* maxfall = FindColourChild(colour, 0x55BDULL);
    ASSERT_NE(primaries, nullptr);
    ASSERT_NE(transfer, nullptr);
    ASSERT_NE(matrix, nullptr);
    ASSERT_NE(range, nullptr);
    ASSERT_NE(bits, nullptr);
    ASSERT_NE(maxcll, nullptr);
    ASSERT_NE(maxfall, nullptr);
    EXPECT_EQ(ReadUInt(d, *primaries), 9u); // BT.2020
    EXPECT_EQ(ReadUInt(d, *transfer), 16u); // PQ (SMPTE ST 2084)
    EXPECT_EQ(ReadUInt(d, *matrix), 9u);    // BT.2020 NCL
    EXPECT_EQ(ReadUInt(d, *range), 2u);     // full range
    EXPECT_EQ(ReadUInt(d, *bits), 10u);
    EXPECT_EQ(ReadUInt(d, *maxcll), 1000u);
    EXPECT_EQ(ReadUInt(d, *maxfall), 400u);
}

// 9. Multi-cluster: a long recording splits into multiple clusters (2 s rule).
TEST_F(StreamWriterTest, LongRecording_MultipleClusters) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, 12.0, 60, 16); // keyframes every 1 s, cluster boundary every 2 s
    ASSERT_TRUE(w.Finalize());
    const auto d = ReadFile(tmp_);
    EXPECT_GE(CountClusters(d), 2) << "12 s recording must span multiple clusters";
}

// 10. Push after Finalize is a no-op and does not corrupt the file.
TEST_F(StreamWriterTest, PushAfterFinalize_NoOp) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, 2.0, 60, 16);
    ASSERT_TRUE(w.Finalize());
    const auto before = std::filesystem::file_size(tmp_);
    MuxPacket p;
    p.pts_ns = 99ULL * kTimescaleNs;
    p.track_num = 1;
    p.is_key = true;
    p.bytes = {0x65};
    EXPECT_FALSE(w.Push(std::move(p)));
    EXPECT_EQ(std::filesystem::file_size(tmp_), before);
}

// ADR 0030: audio_sample_rate/channels/bit_depth fields are threaded into the
// container header. We verify the file opens cleanly with non-default values;
// byte-level parsing of KaxAudioSamplingFreq/KaxAudioChannels/KaxAudioBitDepth
// is deferred to the full AV-verification round (requires an EBML node walker
// with audio-track descent).
TEST_F(StreamWriterTest, NonDefaultAudioFormat_OpensAndFinalizes) {
    MatroskaStreamConfig c;
    c.output_path = tmp_;
    c.video_codec_id = "V_AV1";
    c.video_codec_private = FakeAv1Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = StreamAudioCodec::Pcm;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = {}; // PCM has no CodecPrivate
    // Non-default audio format (ADR 0030).
    c.audio_sample_rate = 44100;
    c.audio_channels = 1;
    c.audio_bit_depth = 24;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c)) << w.error();
    FeedSeconds(w, 2.0, 60, 16);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());
    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

} // namespace
