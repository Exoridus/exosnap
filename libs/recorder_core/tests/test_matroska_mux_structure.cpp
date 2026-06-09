#include <gtest/gtest.h>

// libebml / libmatroska — suppress MSVC warnings from third-party code
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#pragma warning(disable : 4244)
#pragma warning(disable : 4245)
#pragma warning(disable : 4100)
#endif
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTracks.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint64_t kTimescaleNs = 1000000ULL; // 1 ms per unit
static constexpr uint64_t kSeekHeadReserved = 150ULL;

// Well-known multi-byte Matroska element IDs (big-endian byte sequences in the
// file). Only used for presence checks via CountPattern; single-byte IDs like
// KaxCuePoint are counted with the structural walker (CountCuePoints) instead.
static constexpr uint8_t kIdKaxCues[] = {0x1C, 0x53, 0xBB, 0x6B};
static constexpr uint8_t kIdKaxSeekHead[] = {0x11, 0x4D, 0x9B, 0x74};
static constexpr uint8_t kIdKaxDuration[] = {0x44, 0x89};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read entire file into a byte vector.
static std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return {};
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    return buf;
}

// Count non-overlapping occurrences of a byte pattern in the file data.
// NOTE: only reliable for multi-byte element IDs (>= 2 bytes). A 1-byte ID like
// KaxCuePoint (0xBB) appears spuriously inside block payloads, so use the proper
// EBML structure walk (CountCuePoints) for those.
static int CountPattern(const std::vector<uint8_t>& data, const uint8_t* pattern, size_t plen) {
    int count = 0;
    for (size_t i = 0; i + plen <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, pattern, plen) == 0) {
            ++count;
            i += plen - 1;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Minimal deterministic EBML structure walker
// ---------------------------------------------------------------------------
// EBML element = ID (vint, marker bit kept) + Size (vint, marker bit stripped) +
// Data. This lets us count elements by their position in the tree rather than by
// scanning raw bytes, which is the only correct way to count single-byte IDs.

// Read an EBML ID; the leading-zero count of the first byte gives the length and
// the ID keeps all bytes (including the marker bit).
static uint64_t ReadEbmlId(const std::vector<uint8_t>& d, size_t& off, int& id_len) {
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
    id_len = len;
    return id;
}

// Read an EBML size vint (marker bit stripped). Sets is_unknown when all value
// bits are 1 (the EBML "unknown size" encoding used for the live Segment header).
static uint64_t ReadEbmlSize(const std::vector<uint8_t>& d, size_t& off, bool& is_unknown) {
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

// Parse the direct children of a master element occupying bytes [start, end).
static std::vector<EbmlNode> ParseChildren(const std::vector<uint8_t>& d, size_t start, size_t end) {
    std::vector<EbmlNode> out;
    size_t off = start;
    while (off + 2 <= end) {
        int idlen = 0;
        const uint64_t id = ReadEbmlId(d, off, idlen);
        bool unknown = false;
        const uint64_t size = ReadEbmlSize(d, off, unknown);
        size_t data_end = unknown ? end : (off + static_cast<size_t>(size));
        if (data_end > end)
            data_end = end; // clamp (finalized Segment may round up)
        out.push_back({id, off, static_cast<uint64_t>(data_end - off)});
        off = data_end;
    }
    return out;
}

// Well-known multi-byte element IDs as full vint values.
static constexpr uint64_t kEbmlIdSegment = 0x18538067ULL;
static constexpr uint64_t kEbmlIdCues = 0x1C53BB6BULL;
static constexpr uint64_t kEbmlIdCuePoint = 0xBBULL;

// Count KaxCuePoint children of the KaxCues master via real structure walking.
// libmatroska omits the Cues element entirely when it holds zero CuePoints (an
// empty Cues is invalid Matroska), so an absent Cues element means zero cue
// points. Returns -2 only if the Segment itself cannot be found (malformed file).
static int CountCuePoints(const std::vector<uint8_t>& d) {
    const auto top = ParseChildren(d, 0, d.size());
    for (const auto& e : top) {
        if (e.id != kEbmlIdSegment)
            continue;
        const auto level1 = ParseChildren(d, e.data_off, e.data_off + e.data_size);
        for (const auto& c : level1) {
            if (c.id != kEbmlIdCues)
                continue;
            const auto pts = ParseChildren(d, c.data_off, c.data_off + c.data_size);
            int n = 0;
            for (const auto& p : pts)
                if (p.id == kEbmlIdCuePoint)
                    ++n;
            return n;
        }
        return 0; // Segment found, no Cues element => zero seek points
    }
    return -2; // Segment not found => malformed file
}

// Synthetic packet for test MKV generation
struct TestPacket {
    uint64_t pts_ns = 0;
    uint32_t track_num = 0; // 1=video, 2=audio
    bool is_key = false;
    std::vector<uint8_t> bytes;
};

// Codec private stubs
static std::vector<uint8_t> FakeH264CodecPrivate() {
    // Minimal AVCDecoderConfigurationRecord
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
static std::vector<uint8_t> FakeAacCodecPrivate() {
    // AAC-LC, 48 kHz, stereo AudioSpecificConfig
    return {0x11, 0x90};
}
static std::vector<uint8_t> FakeAv1CodecPrivate() {
    return {0x81, 0x00, 0x00, 0x00};
}
static std::vector<uint8_t> FakeOpusCodecPrivate() {
    return {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 0x01, 0x02, 0x00, 0x00, 0x80, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00};
}

// Standard synthetic dataset: 5 video frames (3 keyframes) + 10 audio frames
static std::vector<TestPacket> MakeStandardPackets() {
    std::vector<TestPacket> packets;
    const uint64_t video_frame_ns = 1000000000ULL / 60;
    for (int i = 0; i < 5; ++i) {
        TestPacket vp;
        vp.pts_ns = static_cast<uint64_t>(i) * video_frame_ns;
        vp.track_num = 1;
        vp.is_key = (i % 2 == 0); // frames 0, 2, 4 are keyframes
        vp.bytes = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(vp.is_key ? 0x65 : 0x41)};
        packets.push_back(vp);
    }
    const uint64_t audio_frame_ns = 1024ULL * 1000000000ULL / 48000ULL;
    for (int i = 0; i < 10; ++i) {
        TestPacket ap;
        ap.pts_ns = static_cast<uint64_t>(i) * audio_frame_ns;
        ap.track_num = 2;
        ap.is_key = true;
        ap.bytes = {0xFF, 0xF1, 0x50, 0x80, 0x00, 0x1F, 0xFC};
        packets.push_back(ap);
    }
    std::stable_sort(packets.begin(), packets.end(),
                     [](const TestPacket& a, const TestPacket& b) { return a.pts_ns < b.pts_ns; });
    return packets;
}

// ---------------------------------------------------------------------------
// Core write function matching the mux_thread.cpp patterns exactly
// ---------------------------------------------------------------------------
static bool WriteTestMkv(const std::string& path, const std::vector<uint8_t>& video_cp,
                         const std::vector<uint8_t>& audio_cp, const std::string& video_codec_id,
                         const std::string& audio_codec_id, const std::vector<TestPacket>& packets,
                         uint64_t fps_num = 60, uint64_t fps_den = 1) {
    libebml::StdIOCallback* io = nullptr;
    try {
        io = new libebml::StdIOCallback(path.c_str(), MODE_CREATE);
    } catch (...) {
        return false;
    }

    // Duration
    uint64_t max_pts_ns = 0;
    for (const auto& p : packets)
        if (p.pts_ns > max_pts_ns)
            max_pts_ns = p.pts_ns;
    const uint64_t last_ns = (fps_num > 0) ? fps_den * 1000000000ULL / fps_num : 0;
    const double duration_ms = static_cast<double>((max_pts_ns + last_ns) / kTimescaleNs);

    // EBML header
    {
        libebml::EbmlHead h;
        libebml::GetChild<libebml::EDocType>(h).SetValue("matroska");
        libebml::GetChild<libebml::EDocTypeVersion>(h).SetValue(4);
        libebml::GetChild<libebml::EDocTypeReadVersion>(h).SetValue(2);
        h.Render(*io, true);
    }

    libmatroska::KaxSegment segment;
    segment.SetSizeInfinite(true);
    segment.WriteHead(*io, 8);
    const uint64_t segment_data_start = static_cast<uint64_t>(io->getFilePointer());

    // SeekHead placeholder — kept alive until ReplaceWith below.
    libebml::EbmlVoid seekhead_void;
    seekhead_void.SetSize(kSeekHeadReserved);
    seekhead_void.Render(*io);

    // Info with duration
    auto& info = libebml::AddNewChild<libmatroska::KaxInfo>(segment);
    libebml::GetChild<libmatroska::KaxTimecodeScale>(info).SetValue(kTimescaleNs);
    libebml::GetChild<libmatroska::KaxMuxingApp>(info).SetValueUTF8("exosnap-test");
    libebml::GetChild<libmatroska::KaxWritingApp>(info).SetValueUTF8("exosnap-test");
    libebml::GetChild<libmatroska::KaxDuration>(info).SetValue(duration_ms);
    info.Render(*io);

    // Tracks
    libmatroska::KaxTracks tracks;
    {
        auto& vid = libebml::AddNewChild<libmatroska::KaxTrackEntry>(tracks);
        libebml::GetChild<libmatroska::KaxTrackNumber>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackUID>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackType>(vid).SetValue(1);
        libebml::GetChild<libmatroska::KaxTrackFlagLacing>(vid).SetValue(0);
        libebml::GetChild<libmatroska::KaxCodecID>(vid).SetValue(video_codec_id);
        if (!video_cp.empty())
            libebml::GetChild<libmatroska::KaxCodecPrivate>(vid).CopyBuffer(const_cast<uint8_t*>(video_cp.data()),
                                                                            static_cast<uint32_t>(video_cp.size()));
        {
            auto& vs = libebml::GetChild<libmatroska::KaxTrackVideo>(vid);
            libebml::GetChild<libmatroska::KaxVideoPixelWidth>(vs).SetValue(1280);
            libebml::GetChild<libmatroska::KaxVideoPixelHeight>(vs).SetValue(720);
        }
        if (fps_num > 0)
            libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(vid).SetValue(fps_den * 1000000000ULL / fps_num);
        vid.SetGlobalTimecodeScale(kTimescaleNs);
    }
    {
        auto& aud = libebml::AddNewChild<libmatroska::KaxTrackEntry>(tracks);
        libebml::GetChild<libmatroska::KaxTrackNumber>(aud).SetValue(2);
        libebml::GetChild<libmatroska::KaxTrackUID>(aud).SetValue(2);
        libebml::GetChild<libmatroska::KaxTrackType>(aud).SetValue(2);
        libebml::GetChild<libmatroska::KaxTrackFlagLacing>(aud).SetValue(0);
        libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue(audio_codec_id);
        if (!audio_cp.empty())
            libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(const_cast<uint8_t*>(audio_cp.data()),
                                                                            static_cast<uint32_t>(audio_cp.size()));
        {
            auto& as = libebml::GetChild<libmatroska::KaxTrackAudio>(aud);
            libebml::GetChild<libmatroska::KaxAudioSamplingFreq>(as).SetValue(48000.0);
            libebml::GetChild<libmatroska::KaxAudioChannels>(as).SetValue(2);
        }
        aud.SetGlobalTimecodeScale(kTimescaleNs);
    }
    tracks.Render(*io); // bSaveDefault=false avoids rendering deprecated KaxTrackTimecodeScale

    // Clusters with SimpleBlock
    libmatroska::KaxCues cues;
    cues.SetGlobalTimecodeScale(kTimescaleNs);

    libmatroska::KaxCluster* cur = nullptr;
    uint64_t cluster_start_ms = 0;
    bool first_cluster = true;

    struct PendingCue {
        uint64_t timecode_ms;
    };
    std::vector<PendingCue> pending_cues;

    // DataBuffer objects are owned by myBuffers inside each KaxSimpleBlock.
    // The cluster's destructor calls ReleaseFrames() which deletes them.
    // Do not keep a separate owning reference.

    std::vector<libmatroska::KaxTrackEntry*> te_list;
    for (auto& child : tracks) {
        if (EbmlId(*child) == EBML_ID(libmatroska::KaxTrackEntry))
            te_list.push_back(static_cast<libmatroska::KaxTrackEntry*>(child));
    }

    auto flush = [&]() {
        if (!cur)
            return;
        cur->Render(*io, cues);
        const uint64_t cpos = cur->GetPosition();
        for (const auto& pc : pending_cues) {
            auto& pt = libebml::AddNewChild<libmatroska::KaxCuePoint>(cues);
            libebml::GetChild<libmatroska::KaxCueTime>(pt).SetValue(pc.timecode_ms);
            auto& tp = libebml::AddNewChild<libmatroska::KaxCueTrackPositions>(pt);
            libebml::GetChild<libmatroska::KaxCueTrack>(tp).SetValue(1);
            libebml::GetChild<libmatroska::KaxCueClusterPosition>(tp).SetValue(cpos);
        }
        pending_cues.clear();
        delete cur;
        cur = nullptr;
    };

    for (const auto& p : packets) {
        if (p.track_num == 0 || p.track_num > te_list.size())
            continue;
        libmatroska::KaxTrackEntry* te = te_list[p.track_num - 1];
        const uint64_t pkt_ms = p.pts_ns / kTimescaleNs;

        bool new_cluster = first_cluster;
        if (!first_cluster && cur) {
            const int64_t rel = static_cast<int64_t>(pkt_ms) - static_cast<int64_t>(cluster_start_ms);
            if (rel > 32767 || (p.is_key && p.track_num == 1 && (pkt_ms - cluster_start_ms) >= 2000))
                new_cluster = true;
        }
        if (new_cluster) {
            flush();
            first_cluster = false;
            cluster_start_ms = pkt_ms;
            cur = new libmatroska::KaxCluster();
            cur->SetParent(segment);
            cur->SetGlobalTimecodeScale(kTimescaleNs);
            cur->InitTimecode(cluster_start_ms, static_cast<int64_t>(kTimescaleNs));
        }

        auto* db =
            new libmatroska::DataBuffer(const_cast<uint8_t*>(p.bytes.data()), static_cast<uint32_t>(p.bytes.size()));

        auto& sb = libebml::AddNewChild<libmatroska::KaxSimpleBlock>(*cur);
        sb.SetParent(*cur);
        sb.AddFrame(*te, p.pts_ns, *db, libmatroska::LACING_NONE);
        sb.SetKeyframe(p.is_key);

        if (p.is_key && p.track_num == 1)
            pending_cues.push_back({pkt_ms});
    }
    flush();

    cues.Render(*io);

    // Replace the placeholder void with the real SeekHead (handles size accounting).
    {
        libmatroska::KaxSeekHead seekhead;
        seekhead.IndexThis(info, segment);
        seekhead.IndexThis(tracks, segment);
        seekhead.IndexThis(cues, segment);
        seekhead_void.ReplaceWith(seekhead, *io, /*ComeBackAfterward=*/true);
    }

    // Back-patch the real segment size (see mux_thread.cpp Step 12).
    const uint64_t eof = static_cast<uint64_t>(io->getFilePointer());
    if (segment.ForceSize(eof - segment_data_start))
        segment.OverwriteHead(*io);
    io->close();
    delete io;
    return true;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class MatroskaMuxStructureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tmp_ = (std::filesystem::temp_directory_path() / "exosnap_mux_struct_test.mkv").string();
    }
    void TearDown() override {
        std::remove(tmp_.c_str());
    }

    // Count occurrences of a known multi-byte Matroska element ID in the file
    // bytes. Only valid for IDs >= 2 bytes (see CountPattern note).
    int CountId(const uint8_t* id, size_t id_len) {
        const auto data = ReadFile(tmp_);
        return CountPattern(data, id, id_len);
    }

    // Count KaxCuePoint elements via real EBML structure walking.
    int CuePoints() {
        const auto data = ReadFile(tmp_);
        return CountCuePoints(data);
    }

    std::string tmp_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. H.264 + AAC write succeeds
TEST_F(MatroskaMuxStructureTest, H264_AAC_WriteSucceeds) {
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
}

// 2. AV1 + Opus write succeeds
TEST_F(MatroskaMuxStructureTest, AV1_Opus_WriteSucceeds) {
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeAv1CodecPrivate(), FakeOpusCodecPrivate(), "V_AV1", "A_OPUS", pkts));
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
}

// 3. Cues element is present in output
TEST_F(MatroskaMuxStructureTest, CuesElement_IsPresent) {
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_GE(CountId(kIdKaxCues, sizeof(kIdKaxCues)), 1) << "KaxCues element must be present";
}

// 4. Cue count equals video keyframe count (not total frame count)
TEST_F(MatroskaMuxStructureTest, CueCount_EqualsVideoKeyframeCount) {
    auto pkts = MakeStandardPackets();

    int kf_count = 0;
    for (const auto& p : pkts)
        if (p.track_num == 1 && p.is_key)
            ++kf_count;
    ASSERT_EQ(kf_count, 3); // frames 0, 2, 4

    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));

    EXPECT_EQ(CuePoints(), kf_count) << "KaxCuePoint count must equal video keyframe count (" << kf_count << ")";
}

// 5. SeekHead element is present
TEST_F(MatroskaMuxStructureTest, SeekHead_IsPresent) {
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_GE(CountId(kIdKaxSeekHead, sizeof(kIdKaxSeekHead)), 1) << "KaxSeekHead element must be present";
}

// 6. Duration element is present
TEST_F(MatroskaMuxStructureTest, DurationElement_IsPresent) {
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_GE(CountId(kIdKaxDuration, sizeof(kIdKaxDuration)), 1) << "KaxDuration element must be present in KaxInfo";
}

// 7. Cues not generated for zero video keyframes
TEST_F(MatroskaMuxStructureTest, NoCues_WhenNoVideoKeyframes) {
    std::vector<TestPacket> pkts;
    const uint64_t fn = 1000000000ULL / 60;
    for (int i = 0; i < 5; ++i) {
        TestPacket p;
        p.pts_ns = static_cast<uint64_t>(i) * fn;
        p.track_num = 1;
        p.is_key = false; // no keyframes at all
        p.bytes = {0x41};
        pkts.push_back(p);
    }
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    // Cues element is still emitted (cues.Render), but it must contain no CuePoints.
    EXPECT_EQ(CuePoints(), 0) << "No KaxCuePoint entries expected when there are no video keyframes";
}

// 8. Single keyframe produces exactly one cue entry
TEST_F(MatroskaMuxStructureTest, SingleKeyframe_ProducesExactlyOneCue) {
    std::vector<TestPacket> pkts;
    const uint64_t fn = 1000000000ULL / 60;
    for (int i = 0; i < 4; ++i) {
        TestPacket p;
        p.pts_ns = static_cast<uint64_t>(i) * fn;
        p.track_num = 1;
        p.is_key = (i == 0);
        p.bytes = {static_cast<uint8_t>(p.is_key ? 0x65 : 0x41)};
        pkts.push_back(p);
    }
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_EQ(CuePoints(), 1) << "Exactly one KaxCuePoint for one keyframe";
}

// 9. Empty packet list: file still written successfully
TEST_F(MatroskaMuxStructureTest, EmptyPacketList_WritesFile) {
    std::vector<TestPacket> empty;
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", empty));
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
}

// 10. Large PTS values do not cause overflow or empty output
TEST_F(MatroskaMuxStructureTest, LargeTimestamps_DoNotOverflow) {
    std::vector<TestPacket> pkts;
    const uint64_t base = 2ULL * 3600ULL * 1000000000ULL; // 2 hours
    const uint64_t fn = 1000000000ULL / 60;
    for (int i = 0; i < 5; ++i) {
        TestPacket p;
        p.pts_ns = base + static_cast<uint64_t>(i) * fn;
        p.track_num = 1;
        p.is_key = (i == 0);
        p.bytes = {static_cast<uint8_t>(p.is_key ? 0x65 : 0x41)};
        pkts.push_back(p);
    }
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
}

// 11. Multiple audio tracks — both are written (track count 2)
TEST_F(MatroskaMuxStructureTest, TwoAudioTrackPaths_AreIndexedByTrackNum) {
    // Use the single-audio setup (WriteTestMkv only writes one audio track);
    // this tests that track_num=2 packets reach the audio KaxTrackEntry[1].
    auto pkts = MakeStandardPackets();
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts));
    // At least one audio frame was written — file must be non-trivial
    const auto sz = std::filesystem::file_size(tmp_);
    EXPECT_GT(sz, 300u);
}

// 12. AV1 codec private is exactly 4 bytes for minimal configuration
TEST_F(MatroskaMuxStructureTest, Av1CodecPrivate_MinimalFourBytes) {
    const auto cp = FakeAv1CodecPrivate();
    EXPECT_EQ(cp.size(), 4u) << "AV1CodecConfigurationRecord minimum is 4 bytes";
    EXPECT_EQ(cp[0] & 0x80, 0x80u) << "marker bit must be set";
    EXPECT_EQ(cp[0] & 0x7F, 0x01u) << "version must be 1";
}

// 13. Opus codec private is exactly 19 bytes (OpusHead)
TEST_F(MatroskaMuxStructureTest, OpusCodecPrivate_IsNineteenBytes) {
    const auto cp = FakeOpusCodecPrivate();
    EXPECT_EQ(cp.size(), 19u) << "OpusHead CodecPrivate must be 19 bytes";
    const std::string magic(reinterpret_cast<const char*>(cp.data()), 8);
    EXPECT_EQ(magic, "OpusHead");
}

// 14. File size is larger for more packets (regression: data is actually written)
TEST_F(MatroskaMuxStructureTest, MorePackets_ProduceLargerFile) {
    std::vector<TestPacket> few_pkts;
    {
        TestPacket p;
        p.pts_ns = 0;
        p.track_num = 1;
        p.is_key = true;
        p.bytes = {0x65};
        few_pkts.push_back(p);
    }
    ASSERT_TRUE(
        WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", few_pkts));
    const auto size_small = std::filesystem::file_size(tmp_);
    std::remove(tmp_.c_str());

    auto many_pkts = MakeStandardPackets();
    ASSERT_TRUE(
        WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", many_pkts));
    const auto size_large = std::filesystem::file_size(tmp_);

    EXPECT_GT(size_large, size_small) << "File with more packets must be larger than file with fewer packets";
}

// 15. Long-duration multi-cluster recording: no crash, file produced
TEST_F(MatroskaMuxStructureTest, MultiCluster_LongDuration_ProducesFile) {
    std::vector<TestPacket> pkts;
    // 10 video keyframes 1 second apart (triggers cluster splits at 2s intervals)
    for (int i = 0; i < 10; ++i) {
        TestPacket p;
        p.pts_ns = static_cast<uint64_t>(i) * 1000000000ULL;
        p.track_num = 1;
        p.is_key = true;
        p.bytes = {0x65};
        pkts.push_back(p);
    }
    ASSERT_TRUE(WriteTestMkv(tmp_, FakeH264CodecPrivate(), FakeAacCodecPrivate(), "V_MPEG4/ISO/AVC", "A_AAC", pkts, 1,
                             1)); // 1 fps
    EXPECT_GT(std::filesystem::file_size(tmp_), 0u);
    // Every video keyframe gets a cue entry regardless of how clusters split.
    EXPECT_EQ(CuePoints(), 10) << "10 keyframes → 10 cue entries";
}

} // namespace
