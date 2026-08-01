#include <gtest/gtest.h>

#include "matroska_stream_writer.h"
#include "test_unique_temp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

// These tests exercise the PRODUCTION streaming writer (MatroskaStreamWriter)
// directly on synthetic packet streams — no GPU, no live session. They verify
// both Matroska structural correctness (parity with the batch muxer's
// guarantees) and the headline streaming property: peak buffered RAM is bounded
// by the reorder window, NOT by the total session length.

namespace {

using recorder_core::DurabilityFlushScheduler;
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

// ParseChildren reports where each element's DATA begins; checking a
// CueClusterPosition needs where the element itself begins, plus the Segment's
// own data start (the origin those positions are measured from).
struct SegmentChildRef {
    uint64_t id = 0;
    size_t element_off = 0; // offset of the element's ID byte, from file start
};

struct SegmentLayout {
    size_t data_off = 0; // first byte after the Segment header, from file start
    std::vector<SegmentChildRef> children;
};

SegmentLayout ReadSegmentLayout(const std::vector<uint8_t>& d) {
    SegmentLayout out;
    size_t off = 0;
    while (off + 2 <= d.size()) {
        const uint64_t id = ReadEbmlId(d, off);
        bool unknown = false;
        const uint64_t size = ReadEbmlSize(d, off, unknown);
        if (id != kIdSegment) {
            off += static_cast<size_t>(size); // skip the EBML head
            continue;
        }
        out.data_off = off;
        const size_t end = unknown ? d.size() : std::min(d.size(), off + static_cast<size_t>(size));
        size_t p = off;
        while (p + 2 <= end) {
            const size_t element_off = p;
            const uint64_t child_id = ReadEbmlId(d, p);
            bool child_unknown = false;
            const uint64_t child_size = ReadEbmlSize(d, p, child_unknown);
            size_t child_end = child_unknown ? end : (p + static_cast<size_t>(child_size));
            if (child_end > end)
                child_end = end;
            out.children.push_back({child_id, element_off});
            p = child_end;
        }
        return out;
    }
    return out;
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

// Every CueClusterPosition in the file, in document order.
std::vector<uint64_t> CueClusterPositions(const std::vector<uint8_t>& d) {
    constexpr uint64_t kIdCueTrackPositions = 0xB7ULL;
    constexpr uint64_t kIdCueClusterPosition = 0xF1ULL;
    std::vector<uint64_t> out;
    for (const auto& seg : SegmentChildren(d)) {
        if (seg.id != kIdCues)
            continue;
        for (const auto& point : ParseChildren(d, seg.data_off, seg.data_off + seg.data_size)) {
            if (point.id != kIdCuePoint)
                continue;
            for (const auto& tp : ParseChildren(d, point.data_off, point.data_off + point.data_size)) {
                if (tp.id != kIdCueTrackPositions)
                    continue;
                for (const auto& field : ParseChildren(d, tp.data_off, tp.data_off + tp.data_size)) {
                    if (field.id == kIdCueClusterPosition)
                        out.push_back(ReadUInt(d, field));
                }
            }
        }
    }
    return out;
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

// Descend into the KaxVideoColourMasterMeta (id 0x55D0) child of Colour, if
// present, and return ITS children (the chromaticity/luminance float fields).
std::vector<EbmlNode> MasterMetaChildren(const std::vector<EbmlNode>& colour, const std::vector<uint8_t>& d) {
    constexpr uint64_t kIdMasterMeta = 0x55D0ULL;
    for (const auto& c : colour) {
        if (c.id == kIdMasterMeta) {
            return ParseChildren(d, c.data_off, c.data_off + c.data_size);
        }
    }
    return {};
}

// Read a big-endian IEEE-754 float (4 or 8 bytes) from a node's raw bytes.
double ReadFloat(const std::vector<uint8_t>& d, const EbmlNode& n) {
    uint64_t bits = 0;
    for (uint64_t i = 0; i < n.data_size; ++i)
        bits = (bits << 8) | d[n.data_off + static_cast<size_t>(i)];
    if (n.data_size == 4) {
        const auto bits32 = static_cast<uint32_t>(bits);
        float v = 0.0f;
        std::memcpy(&v, &bits32, sizeof(v));
        return static_cast<double>(v);
    }
    double v = 0.0;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
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
        tmp_ = exosnap_test::UniqueTempPathStr("stream_writer.mkv");
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

// 2b. PCM path: track header carries CodecID "A_PCM/INT/LIT" and BitDepth=16,
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

    // CodecID "A_PCM/INT/LIT" present in the rendered container.
    const std::string kPcmId = "A_PCM/INT/LIT";
    const auto id_it = std::search(d.begin(), d.end(), kPcmId.begin(), kPcmId.end());
    EXPECT_NE(id_it, d.end()) << "A_PCM/INT/LIT CodecID not found in output";

    // KaxAudioBitDepth (EBML id 0x6264), 1-byte size 0x81, value 16 (0x10).
    const std::vector<uint8_t> kBitDepth16 = {0x62, 0x64, 0x81, 0x10};
    const auto bd_it = std::search(d.begin(), d.end(), kBitDepth16.begin(), kBitDepth16.end());
    EXPECT_NE(bd_it, d.end()) << "KaxAudioBitDepth=16 not found in output";

    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

// 2b-float. Float-PCM path: track header carries CodecID "A_PCM/FLOAT/IEEE"
//     (not "A_PCM/INT/LIT") and BitDepth=32 when audio_float is set.
TEST_F(StreamWriterTest, PcmFloat_WritesFloatCodecIdAndBitDepth32) {
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
    c.audio_tracks[0].codec_private = {}; // PCM/float has no CodecPrivate
    c.audio_bit_depth = 32;
    c.audio_float = true;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c)) << w.error();
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());

    // CodecID "A_PCM/FLOAT/IEEE" present in the rendered container.
    const std::string kPcmFloatId = "A_PCM/FLOAT/IEEE";
    const auto id_it = std::search(d.begin(), d.end(), kPcmFloatId.begin(), kPcmFloatId.end());
    EXPECT_NE(id_it, d.end()) << "A_PCM/FLOAT/IEEE CodecID not found in output";

    // The plain int CodecID must NOT appear as this track's CodecID: search for
    // the more specific "A_PCM/INT/LIT" string, which is not a substring of
    // "A_PCM/FLOAT/IEEE" and so must be absent from a float-only track.
    const std::string kPcmIntId = "A_PCM/INT/LIT";
    const auto int_id_it = std::search(d.begin(), d.end(), kPcmIntId.begin(), kPcmIntId.end());
    EXPECT_EQ(int_id_it, d.end()) << "A_PCM/INT/LIT must not appear for a float-PCM track";

    // KaxAudioBitDepth (EBML id 0x6264), 1-byte size 0x81, value 32 (0x20).
    const std::vector<uint8_t> kBitDepth32 = {0x62, 0x64, 0x81, 0x20};
    const auto bd_it = std::search(d.begin(), d.end(), kBitDepth32.begin(), kBitDepth32.end());
    EXPECT_NE(bd_it, d.end()) << "KaxAudioBitDepth=32 not found in output";

    EXPECT_TRUE(HasLevel1(d, kIdTracks));
    EXPECT_TRUE(SegmentSizeIsFinite(d));
}

// 2c. The CodecID written for integer PCM must be the exact string FFmpeg's
//     matroska demuxer (and any other spec-compliant demuxer) recognizes.
//     A byte-search only proves "we wrote what we intended" -- this proves
//     "what we intended is actually readable."
TEST_F(StreamWriterTest, Pcm_CodecIdIsReadableByFfmpegMatroskaDemuxer) {
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
    c.audio_tracks[0].codec_private = {};

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c));
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    AVFormatContext* fmt_ctx = nullptr;
    ASSERT_EQ(avformat_open_input(&fmt_ctx, tmp_.c_str(), nullptr, nullptr), 0)
        << "FFmpeg could not open the file MatroskaStreamWriter produced";
    ASSERT_GE(avformat_find_stream_info(fmt_ctx, nullptr), 0);

    int audio_stream_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_NE(audio_stream_idx, -1) << "No audio stream found by the demuxer";
    EXPECT_EQ(fmt_ctx->streams[static_cast<unsigned>(audio_stream_idx)]->codecpar->codec_id, AV_CODEC_ID_PCM_S16LE)
        << "FFmpeg's matroska demuxer did not recognize the PCM CodecID -- the string "
           "MatroskaStreamWriter wrote does not match FFmpeg's ff_mkv_codec_tags table";

    avformat_close_input(&fmt_ctx);
}

// 2d-float. Same proof for the float-PCM CodecID.
TEST_F(StreamWriterTest, PcmFloat_CodecIdIsReadableByFfmpegMatroskaDemuxer) {
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
    c.audio_tracks[0].codec_private = {};
    c.audio_bit_depth = 32;
    c.audio_float = true;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(c)) << w.error();
    FeedSeconds(w, 3.0, 30, 32);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    AVFormatContext* fmt_ctx = nullptr;
    ASSERT_EQ(avformat_open_input(&fmt_ctx, tmp_.c_str(), nullptr, nullptr), 0)
        << "FFmpeg could not open the file MatroskaStreamWriter produced";
    ASSERT_GE(avformat_find_stream_info(fmt_ctx, nullptr), 0);

    int audio_stream_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_NE(audio_stream_idx, -1) << "No audio stream found by the demuxer";
    EXPECT_EQ(fmt_ctx->streams[static_cast<unsigned>(audio_stream_idx)]->codecpar->codec_id, AV_CODEC_ID_PCM_F32LE)
        << "FFmpeg's matroska demuxer did not recognize the float-PCM CodecID -- the string "
           "MatroskaStreamWriter wrote does not match FFmpeg's ff_mkv_codec_tags table";

    avformat_close_input(&fmt_ctx);
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

// 2e. Open() uses exclusive creation, not truncate-on-exists: a file already
// present at output_path (e.g. a segment writer racing DeriveSegmentPath's
// probe, or a stale leftover) must make Open() fail rather than silently
// overwrite it. This closes the TOCTOU gap between a probe confirming a path
// is free and this Open() call actually creating the file
// (see DeriveSegmentPathTest.ProbeConfirmsFreeButAnotherWriterCreatesItBeforeOpen
// in test_split_segments.cpp for the paired end-to-end scenario).
TEST_F(StreamWriterTest, Open_PreexistingFile_FailsInsteadOfTruncating) {
    const std::string sentinel_content = "not-mine";
    {
        std::ofstream victim(tmp_);
        victim << sentinel_content;
    }

    MatroskaStreamWriter w;
    EXPECT_FALSE(w.Open(MakeConfig(tmp_, /*h264=*/true, /*opus=*/false)));
    EXPECT_TRUE(w.failed());

    std::ifstream check(tmp_);
    std::string content;
    std::getline(check, content);
    EXPECT_EQ(content, sentinel_content) << "a pre-existing file at output_path must survive Open() untouched";
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

// 4b. Cue points when the keyframe interval equals the 2 s cluster boundary — the
// real-recording regime (one keyframe per cluster, the keyframe IS the boundary).
// The gop=60 case above keeps two keyframes per cluster and never exercises this.
TEST_F(StreamWriterTest, CuePointPerKeyframe_WhenGopEqualsClusterBoundary) {
    const double seconds = 7.0;
    const int gop = 120; // keyframe every 2 s @ 60 fps == cluster boundary
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
    EXPECT_EQ(CountCuePoints(d), expected_kf) << "one cue per 2 s keyframe when keyframe == cluster boundary";
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

// Color metadata (ADR 0032; default flipped Full->Limited by
// fix/color-range-signaling): the video track carries an SDR BT.709
// limited-range 8-bit Colour element by default, so the file is no longer
// color-ambiguous and no HDR sub-elements are emitted. Limited is the default
// because common consumer players (verified: VLC) ignore the range flag
// entirely and always expand limited->full — a Full-range recording looked
// permanently crushed/dark there regardless of correct tagging.
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
    EXPECT_EQ(ReadUInt(d, *range), 1u);     // studio/limited range (the current default)
    EXPECT_EQ(ReadUInt(d, *bits), 8u);
    EXPECT_EQ(FindColourChild(colour, 0x55BCULL), nullptr) << "MaxCLL must be absent for SDR";
    EXPECT_EQ(FindColourChild(colour, 0x55BDULL), nullptr) << "MaxFALL must be absent for SDR";
}

// Color-range-signaling fix: Full range (0-255) remains available as an
// explicit opt-in and must still round-trip correctly — Matrix/Primaries/
// Transfer stay BT.709, only Range changes to 2 (full). This is the other
// half of the SDR range selector's two valid states (Limited/default is
// covered above).
TEST_F(StreamWriterTest, WritesBt709ColourElementForFullRangeOption) {
    auto cfg = MakeConfig(tmp_, /*h264=*/false, /*opus=*/true);
    cfg.color.range = recorder_core::ColorRange::Full;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(cfg));
    FeedSeconds(w, 1.0, 30, 64);
    ASSERT_TRUE(w.Finalize());

    const auto d = ReadFile(tmp_);
    const auto colour = VideoColourChildren(d);
    ASSERT_FALSE(colour.empty()) << "video track has no Colour element";

    const EbmlNode* primaries = FindColourChild(colour, 0x55BBULL);
    const EbmlNode* transfer = FindColourChild(colour, 0x55BAULL);
    const EbmlNode* matrix = FindColourChild(colour, 0x55B1ULL);
    const EbmlNode* range = FindColourChild(colour, 0x55B9ULL);
    ASSERT_NE(primaries, nullptr);
    ASSERT_NE(transfer, nullptr);
    ASSERT_NE(matrix, nullptr);
    ASSERT_NE(range, nullptr);
    EXPECT_EQ(ReadUInt(d, *primaries), 1u); // BT.709 — must not change with range
    EXPECT_EQ(ReadUInt(d, *transfer), 1u);  // BT.709 — must not change with range
    EXPECT_EQ(ReadUInt(d, *matrix), 1u);    // BT.709 — must not change with range
    EXPECT_EQ(ReadUInt(d, *range), 2u);     // full range (Matroska Range=2)
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
    EXPECT_TRUE(MasterMetaChildren(colour, d).empty())
        << "KaxVideoColourMasterMeta must be absent when has_mastering_display is not set, "
           "even though other HDR fields (MaxCLL/MaxFALL) are present";
}

// Mastering display metadata (SMPTE ST 2086) round-trips into
// KaxVideoColourMasterMeta when has_mastering_display is set. Uses
// representative BT.2020 primaries + D65 white point. Independent of the
// `hdr`/MaxCLL/MaxFALL gate — see the absence assertion in
// WritesConfiguredColourValuesIncludingHdr above for the other half.
TEST_F(StreamWriterTest, WritesMasteringDisplayMetadataWhenSet) {
    auto cfg = MakeConfig(tmp_, /*h264=*/false, /*opus=*/true);
    cfg.color.primaries = recorder_core::ColorPrimaries::Bt2020;
    cfg.color.transfer = recorder_core::TransferCharacteristics::SmpteSt2084;
    cfg.color.matrix = recorder_core::MatrixCoefficients::Bt2020Ncl;
    cfg.color.bits_per_channel = 10;
    cfg.color.hdr = true;
    cfg.color.max_content_light_level = 1000;
    cfg.color.max_frame_average_light_level = 400;
    cfg.color.has_mastering_display = true;
    cfg.color.mastering_display_primary_r_x = 0.708f;
    cfg.color.mastering_display_primary_r_y = 0.292f;
    cfg.color.mastering_display_primary_g_x = 0.170f;
    cfg.color.mastering_display_primary_g_y = 0.797f;
    cfg.color.mastering_display_primary_b_x = 0.131f;
    cfg.color.mastering_display_primary_b_y = 0.046f;
    cfg.color.mastering_display_white_point_x = 0.3127f;
    cfg.color.mastering_display_white_point_y = 0.3290f;
    cfg.color.mastering_display_max_luminance = 1000.0f;
    cfg.color.mastering_display_min_luminance = 0.0001f;

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(cfg));
    FeedSeconds(w, 1.0, 30, 64);
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    const auto colour = VideoColourChildren(d);
    ASSERT_FALSE(colour.empty()) << "video track has no Colour element";
    const auto mdcv = MasterMetaChildren(colour, d);
    ASSERT_FALSE(mdcv.empty()) << "KaxVideoColourMasterMeta must be present when has_mastering_display is set";

    constexpr uint64_t kRx = 0x55D1ULL, kRy = 0x55D2ULL, kGx = 0x55D3ULL, kGy = 0x55D4ULL, kBx = 0x55D5ULL,
                       kBy = 0x55D6ULL, kWx = 0x55D7ULL, kWy = 0x55D8ULL, kLumMax = 0x55D9ULL, kLumMin = 0x55DAULL;

    const EbmlNode* rx = FindColourChild(mdcv, kRx);
    const EbmlNode* ry = FindColourChild(mdcv, kRy);
    const EbmlNode* gx = FindColourChild(mdcv, kGx);
    const EbmlNode* gy = FindColourChild(mdcv, kGy);
    const EbmlNode* bx = FindColourChild(mdcv, kBx);
    const EbmlNode* by = FindColourChild(mdcv, kBy);
    const EbmlNode* wx = FindColourChild(mdcv, kWx);
    const EbmlNode* wy = FindColourChild(mdcv, kWy);
    const EbmlNode* lmax = FindColourChild(mdcv, kLumMax);
    const EbmlNode* lmin = FindColourChild(mdcv, kLumMin);
    ASSERT_NE(rx, nullptr);
    ASSERT_NE(ry, nullptr);
    ASSERT_NE(gx, nullptr);
    ASSERT_NE(gy, nullptr);
    ASSERT_NE(bx, nullptr);
    ASSERT_NE(by, nullptr);
    ASSERT_NE(wx, nullptr);
    ASSERT_NE(wy, nullptr);
    ASSERT_NE(lmax, nullptr);
    ASSERT_NE(lmin, nullptr);

    constexpr double kTol = 1e-4;
    EXPECT_NEAR(ReadFloat(d, *rx), 0.708, kTol);
    EXPECT_NEAR(ReadFloat(d, *ry), 0.292, kTol);
    EXPECT_NEAR(ReadFloat(d, *gx), 0.170, kTol);
    EXPECT_NEAR(ReadFloat(d, *gy), 0.797, kTol);
    EXPECT_NEAR(ReadFloat(d, *bx), 0.131, kTol);
    EXPECT_NEAR(ReadFloat(d, *by), 0.046, kTol);
    EXPECT_NEAR(ReadFloat(d, *wx), 0.3127, kTol);
    EXPECT_NEAR(ReadFloat(d, *wy), 0.3290, kTol);
    EXPECT_NEAR(ReadFloat(d, *lmax), 1000.0, 0.1);
    EXPECT_NEAR(ReadFloat(d, *lmin), 0.0001, 1e-6);
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

// 9b. Every CueClusterPosition must resolve to the start of a real Cluster.
//
// CueClusterPosition is defined relative to the Segment's DATA start, and
// libmatroska's KaxCluster::GetPosition() already returns exactly that. Running
// that result through KaxSegment::GetRelativePosition() a second time subtracts
// the Segment header twice, so every seek lands short of its cluster -- inside
// the preceding element's payload. Whether a demuxer recovers from that is pure
// luck of what the bytes there happen to look like, which is why the count-based
// cue tests above cannot see it: a wrong position is still a well-formed entry.
TEST_F(StreamWriterTest, CueClusterPositions_ResolveToRealClusters) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, 12.0, 60, 16); // keyframes every 1 s, cluster boundary every 2 s
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    ASSERT_FALSE(d.empty());
    const SegmentLayout layout = ReadSegmentLayout(d);
    ASSERT_GT(layout.data_off, 0u) << "Segment header not found";

    std::vector<size_t> cluster_starts;
    for (const auto& child : layout.children) {
        if (child.id == kIdCluster)
            cluster_starts.push_back(child.element_off);
    }
    ASSERT_GE(cluster_starts.size(), 2u) << "12 s recording must span multiple clusters";

    const auto positions = CueClusterPositions(d);
    ASSERT_FALSE(positions.empty()) << "keyframes were fed, so cue entries must exist";
    for (const uint64_t pos : positions) {
        const size_t absolute = layout.data_off + static_cast<size_t>(pos);
        bool is_cluster_start = false;
        for (const size_t start : cluster_starts)
            is_cluster_start = is_cluster_start || (start == absolute);
        EXPECT_TRUE(is_cluster_start) << "CueClusterPosition " << pos << " resolves to file offset " << absolute
                                      << ", which is not the start of any Cluster (first cluster starts at "
                                      << cluster_starts.front() << ")";
    }
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

// --- DurabilityFlushScheduler: pure cadence logic, no file I/O and no real
//     timer dependency (synthetic steady_clock time points throughout). This
//     is the unit-testable heart of the periodic durability flush added to
//     FlushCluster()/Finalize(): fflush()+FlushFileBuffers() should be due
//     roughly every couple of seconds, never on every single cluster. ---

TEST(DurabilityFlushSchedulerTest, DueBeforeAnyFlushHasBeenRecorded) {
    DurabilityFlushScheduler sched(std::chrono::milliseconds(2000));
    // No MarkFlushed() call yet: due immediately, regardless of `now`.
    EXPECT_TRUE(sched.IsDue(std::chrono::steady_clock::now()));
}

TEST(DurabilityFlushSchedulerTest, NotDueBeforeIntervalElapses) {
    DurabilityFlushScheduler sched(std::chrono::milliseconds(2000));
    const auto t0 = std::chrono::steady_clock::now();
    sched.MarkFlushed(t0);
    EXPECT_FALSE(sched.IsDue(t0 + std::chrono::milliseconds(500)));
    EXPECT_FALSE(sched.IsDue(t0 + std::chrono::milliseconds(1999)));
}

TEST(DurabilityFlushSchedulerTest, DueAtOrAfterTheConfiguredInterval) {
    DurabilityFlushScheduler sched(std::chrono::milliseconds(2000));
    const auto t0 = std::chrono::steady_clock::now();
    sched.MarkFlushed(t0);
    EXPECT_TRUE(sched.IsDue(t0 + std::chrono::milliseconds(2000)));
    EXPECT_TRUE(sched.IsDue(t0 + std::chrono::milliseconds(5000)));
}

TEST(DurabilityFlushSchedulerTest, MarkFlushedResetsTheClock) {
    DurabilityFlushScheduler sched(std::chrono::milliseconds(2000));
    const auto t0 = std::chrono::steady_clock::now();
    sched.MarkFlushed(t0);
    const auto t1 = t0 + std::chrono::milliseconds(2500);
    ASSERT_TRUE(sched.IsDue(t1)) << "sanity: due before the second MarkFlushed()";
    sched.MarkFlushed(t1);
    EXPECT_FALSE(sched.IsDue(t1 + std::chrono::milliseconds(100)));
    EXPECT_TRUE(sched.IsDue(t1 + std::chrono::milliseconds(2000)));
}

TEST(DurabilityFlushSchedulerTest, WriterUsesTheDocumentedTwoSecondInterval) {
    // Single source of truth: the writer's cadence constant must be what the
    // durability-window reasoning (and this test file's comments) assume.
    EXPECT_EQ(MatroskaStreamWriter::kDurabilityFlushInterval, std::chrono::milliseconds(2000));
}

// --- MatroskaStreamWriter integration: the periodic flush actually fires
//     during real Push()/FlushCluster()/Finalize() traffic, without adding a
//     physical-I/O syscall on every single cluster. ---

// 11. A multi-cluster recording performs at least one durability flush, but far
//     fewer than one per cluster: the whole synthetic feed below executes in
//     well under the 2 s durability cadence, so only the very first
//     FlushCluster() call (nothing flushed yet) should durably flush during
//     streaming, plus exactly one more forced flush at Finalize().
TEST_F(StreamWriterTest, DurabilityFlush_FiresButNotOncePerCluster) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    FeedSeconds(w, 12.0, 60, 16); // same feed as the multi-cluster test above
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const auto d = ReadFile(tmp_);
    const int clusters = CountClusters(d);
    ASSERT_GE(clusters, 3) << "test assumes several clusters to prove the flush isn't per-cluster";

    EXPECT_GE(w.durability_flush_count(), 1u) << "recording must be durably flushed at least once";
    EXPECT_LT(w.durability_flush_count(), static_cast<uint64_t>(clusters))
        << "durability flush must be gated by cadence, not fired on every FlushCluster()";
}

// 12. Even a recording with zero packets must still get the unconditional final
//     flush in Finalize() -- the (minimal) container is durably written.
TEST_F(StreamWriterTest, EmptySession_StillPerformsFinalDurabilityFlush) {
    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();
    EXPECT_EQ(w.durability_flush_count(), 1u)
        << "Finalize() must still durably flush the container even with no clusters";
}

// 13. The progress sink (SetProgressSink) must keep advancing through
//     Finalize()'s back-patch phase (Cues render, SeekHead/Duration/Segment-
//     size patches, final durability flush), not freeze at whatever it was
//     after the last streaming-phase FlushCluster() call. The out-of-thread
//     shutdown watcher (FinalizeProgressTracker, see finalize_join_policy.h)
//     polls this sink while the mux thread is blocked inside Finalize(); if it
//     stops advancing, a slow-but-working finalize (e.g. on a network drive)
//     is misclassified as a stall and the recording is falsely reported as
//     failed even though the file finishes writing correctly.
TEST_F(StreamWriterTest, ProgressSinkAdvancesThroughFinalizeBackPatch) {
    MatroskaStreamWriter w;
    std::atomic<uint64_t> sink{0};
    w.SetProgressSink(&sink);

    ASSERT_TRUE(w.Open(MakeConfig(tmp_, true, false)));
    // Cross at least one 2 s cluster boundary so the sink already carries a
    // nonzero value published from the streaming phase before Finalize().
    FeedSeconds(w, /*seconds=*/5.0, /*gop=*/30, /*payload_bytes=*/512);

    const uint64_t pre_finalize_sink = sink.load(std::memory_order_relaxed);

    ASSERT_TRUE(w.Finalize());
    ASSERT_FALSE(w.failed()) << w.error();

    const uint64_t post_finalize_sink = sink.load(std::memory_order_relaxed);
    const auto file_size = static_cast<uint64_t>(std::filesystem::file_size(tmp_));

    // Cues/SeekHead/Duration/Segment-size are written/patched strictly after
    // the last streaming-phase cluster flush, so the published value must grow.
    EXPECT_GT(post_finalize_sink, pre_finalize_sink)
        << "Progress sink did not advance during Finalize()'s back-patch phase";
    // And it must reflect the true finalized file size, not an earlier
    // checkpoint frozen mid-Finalize().
    EXPECT_EQ(post_finalize_sink, file_size) << "Progress sink does not reflect the finalized file's true size";
}

} // namespace
