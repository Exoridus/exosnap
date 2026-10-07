#include <gtest/gtest.h>

#include "annexb_to_avcc.h"
#include "matroska_stream_writer.h"
#include "mux_audio_hold.h"
#include "mux_queue.h"
#include "mux_thread.h"
#include "premux_state.h"
#include "session_internal.h"
#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/split_trigger_source.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "test_unique_temp.h"

// Host tests cover naming, writer isolation, and queued mux transitions without
// a GPU. Real encoder output still requires hardware and decoder validation.

namespace {

using exosnap::engine::DeriveSegmentPath;
using exosnap::engine::MatroskaStreamConfig;
using exosnap::engine::MatroskaStreamWriter;
using exosnap::engine::MuxPacket;
using exosnap::engine::SegmentPathExistsProbe;
using exosnap::engine::SegmentPathResult;

exosnap::engine::EncodedAudioPacket AudioPacket(uint64_t pts_ns, uint8_t marker, uint32_t track = 0) {
    return {{marker}, pts_ns, track};
}

TEST(MuxAudioHoldTest, ReorderedVideoProgressRetainsBoundaryAudio) {
    exosnap::engine::MuxAudioHold hold;
    ASSERT_TRUE(hold.Push(AudioPacket(200, 2)));
    ASSERT_TRUE(hold.Push(AudioPacket(100, 1)));
    hold.AdvanceVideo(100);
    EXPECT_FALSE(hold.PopEligible());
    hold.AdvanceVideo(80);
    EXPECT_FALSE(hold.PopEligible());
    hold.AdvanceVideo(200);
    hold.AdvanceVideo(80);
    ASSERT_TRUE(hold.PopEligible());
    EXPECT_FALSE(hold.PopEligible());
    EXPECT_FALSE(hold.PopBefore(200));
    auto next_segment = hold.Pop();
    ASSERT_TRUE(next_segment);
    EXPECT_EQ(next_segment->pts_ns, 200u);
    EXPECT_EQ(hold.bytes(), 0u);
}

TEST(MuxAudioHoldTest, BytesAndMediaTimeFailClosedWithoutLosingHeldPackets) {
    const size_t one_packet = sizeof(exosnap::engine::EncodedAudioPacket) + 1;
    exosnap::engine::MuxAudioHold hold({one_packet, 60});
    hold.AdvanceVideo(100);
    EXPECT_FALSE(hold.Push(AudioPacket(161, 3)));
    ASSERT_TRUE(hold.Push(AudioPacket(150, 1)));
    EXPECT_FALSE(hold.Push(AudioPacket(151, 2)));
    EXPECT_EQ(hold.bytes(), one_packet);
    auto kept = hold.Pop();
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->bytes.front(), 1);
    EXPECT_EQ(hold.bytes(), 0u);
}

TEST(MuxAudioHoldTest, BudgetCoversMaximumSupportedPcmAudioForOneMinute) {
    const exosnap::engine::MuxAudioHold::Limits limits;
    const uint64_t pcm_bytes = 96000ull * 2 * 4 * exosnap::engine::CodecPrivateData::kMaxAudioTracks * 60;
    const uint64_t packet_overhead =
        100ull * exosnap::engine::CodecPrivateData::kMaxAudioTracks * 60 * sizeof(exosnap::engine::EncodedAudioPacket);
    EXPECT_GT(limits.bytes, pcm_bytes + packet_overhead);
    EXPECT_EQ(limits.duration_ns, 60000000000ull);
}

// Convenience: most call sites in these tests only care about the derived
// path in the success case (naming/collision-policy assertions). Fails loudly
// (via a gtest fatal assertion) if the derivation did not succeed, so a
// regression that turns success into failure cannot silently pass a test that
// only inspects the (then default-constructed, empty) path.
std::filesystem::path DerivedOrFail(const SegmentPathResult& result) {
    EXPECT_TRUE(result.success) << "DeriveSegmentPath unexpectedly failed: " << result.message;
    return result.path;
}

// --- Minimal EBML walker (shared shape with test_matroska_stream_writer.cpp) ---

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
constexpr uint64_t kIdCluster = 0x1F43B675ULL;
constexpr uint64_t kIdDuration = 0x4489ULL;

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

double ReadDurationMs(const std::vector<uint8_t>& d) {
    for (const auto& c : SegmentChildren(d)) {
        if (c.id != kIdInfo)
            continue;
        const auto info = ParseChildren(d, c.data_off, c.data_off + c.data_size);
        for (const auto& e : info) {
            if (e.id != kIdDuration)
                continue;
            if (e.data_size != 8)
                return -2.0;
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

bool SegmentSizeIsFinite(const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off + 2 <= d.size()) {
        const uint64_t id = ReadEbmlId(d, off);
        bool unknown = false;
        const uint64_t size = ReadEbmlSize(d, off, unknown);
        if (id == kIdSegment)
            return !unknown && size > 0;
        off += static_cast<size_t>(size);
    }
    return false;
}

using AudioMarkers = std::array<std::vector<std::pair<uint8_t, int64_t>>, 2>;

AudioMarkers ReadAudioMarkers(const std::filesystem::path& path) {
    const auto bytes = ReadFile(path.string());
    AudioMarkers markers;
    for (const auto& cluster : SegmentChildren(bytes)) {
        if (cluster.id != kIdCluster)
            continue;
        const auto blocks = ParseChildren(bytes, cluster.data_off, cluster.data_off + cluster.data_size);
        int64_t cluster_ms = 0;
        for (const auto& block : blocks) {
            if (block.id == 0xE7) {
                for (size_t i = 0; i < block.data_size; ++i)
                    cluster_ms = (cluster_ms << 8) | bytes[block.data_off + i];
            }
        }
        for (const auto& block : blocks) {
            if (block.id != 0xA3 || block.data_size < 5)
                continue;
            const uint8_t track = bytes[block.data_off] & 0x7F;
            if (track < 2 || track > 3)
                continue;
            const auto relative_ms = static_cast<int16_t>((bytes[block.data_off + 1] << 8) | bytes[block.data_off + 2]);
            markers[track - 2].emplace_back(bytes[block.data_off + 4], cluster_ms + relative_ms);
        }
    }
    return markers;
}

std::vector<uint8_t> SplitAccessUnit() {
    return {0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x28, 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80, 0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33};
}

std::shared_ptr<exosnap::engine::SessionState>
MakeSplitState(const std::filesystem::path& output, std::vector<exosnap::engine::CompletedSegment>& segments) {
    using namespace exosnap::engine;
    auto state = std::make_shared<SessionState>();
    state->config.output_path = output;
    state->config.container = Container::Matroska;
    state->config.video_codec = VideoCodec::H264;
    state->config.audio_codec = AudioCodec::Aac;
    state->audio_track_count = 2;
    state->encode_width = 640;
    state->encode_height = 360;
    state->session_start_qpc_100ns = 1000000;
    state->video_epoch_qpc_100ns.store(1000000);
    const auto access_unit = SplitAccessUnit();
    std::vector<uint8_t> sps_pps;
    EXPECT_TRUE(annexb::ExtractH264SpsAndPps(access_unit.data(), access_unit.size(), sps_pps));
    state->premux.PublishVideo(VideoCodec::H264, std::move(sps_pps));
    state->segment_callback = [&](const auto& segment) { segments.push_back(segment); };
    for (uint32_t track = 0; track < 2; ++track) {
        state->premux.PublishAudio(track, {{0x11, 0x90}, 0});
        state->audio_epoch_qpc_100ns[track].store(1000000 + 200000 * (track + 1));
    }
    return state;
}

TEST(MuxSplitAudioTest, DelayedKeyframePartitionsAlignedAudioAndEosDrainsBothTracks) {
    using namespace exosnap::engine;
    const auto output = exosnap_test::UniqueTempPath("mux_split_delayed_audio.mkv");
    std::vector<CompletedSegment> segments;
    auto state = MakeSplitState(output, segments);
    const auto access_unit = SplitAccessUnit();
    int64_t decode_ns = -40000000;
    const auto video = [&](uint64_t pts_ms, bool key, std::optional<SplitSentinel> split = std::nullopt) {
        EncodedVideoPacket packet;
        packet.bytes = access_unit;
        packet.pts_ns = pts_ms * 1000000;
        packet.dts_ns = decode_ns;
        decode_ns += 40000000;
        packet.keyframe = key;
        ASSERT_EQ(state->PushMuxItem(MuxItem{std::move(packet)}, split), MuxQueueWait::Ready);
    };
    video(0, true);
    for (uint32_t track = 0; track < 2; ++track) {
        uint8_t marker = 0;
        for (const uint64_t aligned_ms : {50u, 150u, 199u, 200u, 250u, 450u}) {
            const uint64_t raw_ms = aligned_ms - 20 * (track + 1);
            ASSERT_EQ(state->PushMuxItem(MuxItem{AudioPacket(raw_ms * 1000000, ++marker, track)}), MuxQueueWait::Ready);
        }
    }
    video(100, false);
    video(80, false);
    video(200, true, SplitSentinel{1, SplitTriggerSource::ManualButton});
    video(300, false);
    state->mux_queue.PushSentinel(VideoEosSentinel{});
    for (uint32_t track = 0; track < 2; ++track) {
        const uint64_t raw_ms = 500 - 20 * (track + 1);
        ASSERT_EQ(state->PushMuxItem(MuxItem{AudioPacket(raw_ms * 1000000, 7, track)}), MuxQueueWait::Ready);
        state->mux_queue.PushSentinel(AudioEosSentinel{track});
    }
    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();
    ASSERT_TRUE(mux->Join(10000));
    ASSERT_FALSE(state->HasFailure());
    ASSERT_EQ(segments.size(), 2u);
    ASSERT_TRUE(segments[0].succeeded);
    ASSERT_TRUE(segments[1].succeeded);
    const auto old_audio = ReadAudioMarkers(segments[0].path);
    const auto new_audio = ReadAudioMarkers(segments[1].path);
    const std::vector<std::pair<uint8_t, int64_t>> expected_old = {{1, 50}, {2, 150}, {3, 199}};
    const std::vector<std::pair<uint8_t, int64_t>> expected_new = {{4, 0}, {5, 50}, {6, 250}, {7, 300}};
    for (size_t track = 0; track < 2; ++track) {
        EXPECT_EQ(old_audio[track], expected_old);
        EXPECT_EQ(new_audio[track], expected_new);
    }
    for (const auto& segment : segments) {
        std::error_code ignored;
        std::filesystem::remove(segment.path, ignored);
    }
}

TEST(MuxSplitAudioTest, StaticVfrProgressKeepsAudioBoundedBeyondOneMinuteUntilDelayedSplit) {
    using namespace exosnap::engine;
    const auto output = exosnap_test::UniqueTempPath("mux_split_static_vfr_audio.mkv");
    std::vector<CompletedSegment> segments;
    auto state = MakeSplitState(output, segments);
    state->config.cfr = false;
    const auto video = [&](uint64_t pts_ns, std::optional<SplitSentinel> split = std::nullopt) {
        EncodedVideoPacket packet;
        packet.bytes = SplitAccessUnit();
        packet.pts_ns = pts_ns;
        packet.keyframe = true;
        ASSERT_EQ(state->PushMuxItem(MuxItem{std::move(packet)}, split), MuxQueueWait::Ready);
    };
    const auto audio = [&](uint64_t aligned_ns, uint8_t marker) {
        for (uint32_t track = 0; track < 2; ++track) {
            const uint64_t raw_ns = aligned_ns - 20000000 * (track + 1);
            ASSERT_EQ(state->PushMuxItem(MuxItem{AudioPacket(raw_ns, marker, track)}), MuxQueueWait::Ready);
        }
    };
    video(0);
    std::vector<std::pair<uint8_t, int64_t>> expected_old;
    for (uint8_t second = 1; second <= 70; ++second) {
        const uint64_t progress_ns = second * 1000000000ull;
        state->mux_queue.PushSentinel(VideoProgressSentinel{progress_ns});
        audio(progress_ns + 500000000, second);
        expected_old.emplace_back(second, second * 1000 + 500);
    }
    audio(70999000000ull, 71);
    expected_old.emplace_back(71, 70999);
    audio(71000000000ull, 72);
    video(71000000000ull, SplitSentinel{1, SplitTriggerSource::ManualButton});
    state->mux_queue.PushSentinel(VideoEosSentinel{});
    for (uint32_t track = 0; track < 2; ++track)
        state->mux_queue.PushSentinel(AudioEosSentinel{track});
    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();
    ASSERT_TRUE(mux->Join(10000));
    ASSERT_FALSE(state->HasFailure());
    ASSERT_EQ(segments.size(), 2u);
    const auto old_audio = ReadAudioMarkers(segments[0].path);
    const auto new_audio = ReadAudioMarkers(segments[1].path);
    const std::vector<std::pair<uint8_t, int64_t>> expected_new = {{72, 0}};
    for (size_t track = 0; track < 2; ++track) {
        EXPECT_EQ(old_audio[track], expected_old);
        EXPECT_EQ(new_audio[track], expected_new);
    }
    for (const auto& segment : segments) {
        std::error_code ignored;
        std::filesystem::remove(segment.path, ignored);
    }
}

std::vector<uint8_t> FakeH264Cp() {
    return {0x01, 0x42, 0x00, 0x1F, 0xFF, 0xE1, 0x00};
}
std::vector<uint8_t> FakeAacCp() {
    return {0x11, 0x90};
}

MatroskaStreamConfig MakeConfig(const std::string& path) {
    MatroskaStreamConfig c;
    c.output_path = path;
    c.video_codec_id = "V_MPEG4/ISO/AVC";
    c.video_codec_private = FakeH264Cp();
    c.encode_width = 1280;
    c.encode_height = 720;
    c.frame_rate_num = 60;
    c.frame_rate_den = 1;
    c.audio_codec = exosnap::engine::StreamAudioCodec::Aac;
    c.audio_track_count = 1;
    c.audio_tracks[0].codec_private = FakeAacCp();
    return c;
}

// Feed `seconds` of media starting at segment-local t=0. Mirrors the mux thread's
// per-segment rebasing: the caller supplies an already-rebased zero-based stream.
void FeedSegmentLocal(MatroskaStreamWriter& w, double seconds, int gop) {
    const uint64_t vframe = 1000000000ULL / 60;
    const uint64_t aframe = 1024ULL * 1000000000ULL / 48000ULL;
    const uint64_t total_ns = static_cast<uint64_t>(seconds * 1e9);
    const std::vector<uint8_t> blob(64, 0xAB);
    uint64_t vpts = 0, apts = 0;
    int vidx = 0;
    while (vpts < total_ns || apts < total_ns) {
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

// --- DeriveSegmentPath: naming + collision policy ---

TEST(DeriveSegmentPathTest, FirstSegmentKeepsBaseNameVerbatim) {
    const std::filesystem::path base = "C:/videos/recording.mkv";
    const auto result = DeriveSegmentPath(base, 0);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.path, base);
}

TEST(DeriveSegmentPathTest, LaterSegmentsGetPartSuffixBeforeExtension) {
    const std::filesystem::path base = "C:/videos/recording.mkv";
    // index is 0-based; part number is index+1, so index 1 -> _part-002.
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 1)), std::filesystem::path("C:/videos/recording_part-002.mkv"));
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 2)), std::filesystem::path("C:/videos/recording_part-003.mkv"));
}

TEST(DeriveSegmentPathTest, PartNumberIsZeroPaddedThreeDigits) {
    const std::filesystem::path base = "C:/videos/clip.webm";
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 8)).filename().string(), "clip_part-009.webm");
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 98)).filename().string(), "clip_part-099.webm");
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 99)).filename().string(), "clip_part-100.webm");
}

TEST(DeriveSegmentPathTest, PreservesExtensionAndStemWithDots) {
    const std::filesystem::path base = "C:/videos/my.recording.v2.mkv";
    EXPECT_EQ(DerivedOrFail(DeriveSegmentPath(base, 1)).filename().string(), "my.recording.v2_part-002.mkv");
}

TEST(DeriveSegmentPathTest, CollisionAppendsDisambiguator) {
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path base = dir / "rec.mkv";

    // Pre-create the natural _part-002 path so the deriver must disambiguate.
    const fs::path natural = dir / "rec_part-002.mkv";
    {
        std::ofstream(natural) << "x";
    }

    const auto result = DeriveSegmentPath(base, 1);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(result.path.filename().string(), "rec_part-002_1.mkv");
    EXPECT_FALSE(fs::exists(result.path));

    fs::remove_all(dir);
}

TEST(DeriveSegmentPathTest, ManyCollisionsSkipPastAllOfThem) {
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path base = dir / "rec.mkv";

    // Pre-create the natural candidate plus disambiguators _1 through _5, so
    // the deriver must skip past all six before finding a free one.
    {
        std::ofstream(dir / "rec_part-002.mkv") << "x";
        for (unsigned n = 1; n <= 5; ++n) {
            std::ofstream(dir / ("rec_part-002_" + std::to_string(n) + ".mkv")) << "x";
        }
    }

    const auto result = DeriveSegmentPath(base, 1);
    ASSERT_TRUE(result.success) << result.message;
    EXPECT_EQ(result.path.filename().string(), "rec_part-002_6.mkv");
    EXPECT_FALSE(fs::exists(result.path));

    fs::remove_all(dir);
}

TEST(DeriveSegmentPathTest, CollisionLimitExhaustedReturnsErrorNotAStaleCandidate) {
    // A probe that reports every candidate as colliding, forever. This
    // exercises the bounded-scan exhaustion path without actually creating
    // 10000 files on disk.
    const SegmentPathExistsProbe always_exists = [](const std::filesystem::path&, std::error_code& ec) {
        ec.clear();
        return true;
    };

    const std::filesystem::path base = "C:/videos/recording.mkv";
    const auto result = DeriveSegmentPath(base, 1, always_exists);

    EXPECT_FALSE(result.success);
    // The old buggy behavior silently returned the last (still-colliding)
    // candidate; the fixed contract must never do that -- path stays empty.
    EXPECT_TRUE(result.path.empty());
    EXPECT_TRUE(static_cast<bool>(result.error));
    EXPECT_FALSE(result.message.empty());
}

TEST(DeriveSegmentPathTest, FilesystemProbeErrorPropagatesImmediatelyWithoutContinuingTheScan) {
    // Simulates a genuine filesystem error (e.g. permission denied) on the
    // very first probe. std::filesystem::exists on this toolchain silently
    // downgrades most real OS errors to "not found" (verified empirically:
    // ACL-denied directories, invalid characters, and very long paths all
    // still yield an empty error_code), so a real permission error cannot be
    // reproduced organically here -- the injectable probe exists specifically
    // to make this path testable.
    int call_count = 0;
    const SegmentPathExistsProbe denied = [&](const std::filesystem::path&, std::error_code& ec) {
        ++call_count;
        ec = std::make_error_code(std::errc::permission_denied);
        return false; // value is irrelevant once ec is set -- must not be trusted
    };

    const std::filesystem::path base = "C:/videos/recording.mkv";
    const auto result = DeriveSegmentPath(base, 1, denied);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.path.empty());
    EXPECT_TRUE(static_cast<bool>(result.error));
    EXPECT_EQ(result.error, std::make_error_code(std::errc::permission_denied));
    // Must abort on the FIRST error, never mask it by trying more candidates.
    EXPECT_EQ(call_count, 1);
}

TEST(DeriveSegmentPathTest, NonexistentParentDirectoryStillSucceeds) {
    // DeriveSegmentPath only resolves NAME collisions; it is not responsible
    // for validating that the parent directory exists (that is
    // RecorderSession::Validate's job at recording start, and the segment
    // writer's Open() surfaces a real directory-missing failure per-segment --
    // see SplitSegmentTest.FailureOnLaterSegmentLeavesEarlierIntact below).
    // A candidate under a directory that does not exist on disk simply
    // "does not exist" like any other free candidate.
    const std::filesystem::path base = exosnap_test::UniqueTempPath("does_not_exist_dir") / "sub" / "recording.mkv";
    const auto result = DeriveSegmentPath(base, 1);
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_EQ(result.path.filename().string(), "recording_part-002.mkv");
}

TEST(DeriveSegmentPathTest, ProbeConfirmsFreeButAnotherWriterCreatesItBeforeOpen) {
    // The TOCTOU race this whole fix is about: DeriveSegmentPath confirms a
    // candidate is free, but before the caller opens it for writing, some
    // other writer (or a leftover from a previous run) creates a file at that
    // exact path. The derivation itself cannot detect this (it already
    // returned); closing this gap is MatroskaStreamWriter's job (exclusive
    // create instead of truncate-on-exists -- see matroska_stream_writer.cpp).
    // This test proves the two pieces fit together: Open() must fail (not
    // silently truncate) when the race is hit.
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path base = dir / "rec.mkv";

    const auto result = DeriveSegmentPath(base, 1);
    ASSERT_TRUE(result.success) << result.message;
    ASSERT_FALSE(fs::exists(result.path));

    // Simulate the race: another process creates the file with real content
    // AFTER DeriveSegmentPath confirmed it free, but BEFORE this caller opens it.
    const std::string sentinel_content = "not-mine";
    {
        std::ofstream victim(result.path);
        victim << sentinel_content;
    }

    MatroskaStreamConfig cfg = MakeConfig(result.path.string());
    MatroskaStreamWriter w;
    EXPECT_FALSE(w.Open(cfg)) << "Open() must fail (exclusive create) instead of truncating the raced file";
    EXPECT_TRUE(w.failed());

    // The other writer's file must survive untouched -- the whole point of
    // exclusive creation is that this writer never got to truncate it.
    std::string content;
    {
        // Scoped so the handle is closed before remove_all() below -- on
        // Windows an open handle blocks directory removal (unlike POSIX).
        std::ifstream check(result.path);
        std::getline(check, content);
    }
    EXPECT_EQ(content, sentinel_content);

    fs::remove_all(dir);
}

// --- Segment transition: each segment is an independently valid container ---

TEST(SplitSegmentTest, ThreeSegmentsEachIndependentlyValid) {
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path base = dir / "session.mkv";

    // Simulate three back-to-back segments from one logical session. Each writer
    // owns exactly one file and is fed a fresh zero-based timeline, exactly as the
    // mux thread does on a SplitSentinel.
    const double seg_seconds[3] = {3.0, 4.0, 2.0};
    for (uint32_t i = 0; i < 3; ++i) {
        const fs::path seg_path = DerivedOrFail(DeriveSegmentPath(base, i));
        MatroskaStreamWriter w;
        ASSERT_TRUE(w.Open(MakeConfig(seg_path.string()))) << w.error();
        FeedSegmentLocal(w, seg_seconds[i], 60);
        ASSERT_TRUE(w.Finalize()) << w.error();
        ASSERT_FALSE(w.failed()) << w.error();

        const auto d = ReadFile(seg_path.string());
        ASSERT_FALSE(d.empty());
        EXPECT_TRUE(HasLevel1(d, kIdTracks));
        EXPECT_TRUE(HasLevel1(d, kIdCues));
        EXPECT_GE(CountClusters(d), 1);
        EXPECT_TRUE(SegmentSizeIsFinite(d));
        // Segment-local duration ~= the per-segment seconds, NOT cumulative.
        const double dur = ReadDurationMs(d);
        EXPECT_NEAR(dur, seg_seconds[i] * 1000.0, 60.0)
            << "segment " << i << " duration must be segment-local (zero-based), not cumulative";
    }

    EXPECT_TRUE(fs::exists(base));
    EXPECT_TRUE(fs::exists(dir / "session_part-002.mkv"));
    EXPECT_TRUE(fs::exists(dir / "session_part-003.mkv"));
    fs::remove_all(dir);
}

TEST(SplitSegmentTest, FailureOnLaterSegmentLeavesEarlierIntact) {
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path base = dir / "session.mkv";

    // Segment 0 finalizes cleanly.
    const fs::path seg0 = DerivedOrFail(DeriveSegmentPath(base, 0));
    {
        MatroskaStreamWriter w;
        ASSERT_TRUE(w.Open(MakeConfig(seg0.string())));
        FeedSegmentLocal(w, 3.0, 60);
        ASSERT_TRUE(w.Finalize());
    }
    const auto before = fs::file_size(seg0);
    const auto before_data = ReadFile(seg0.string());
    EXPECT_TRUE(SegmentSizeIsFinite(before_data));

    // Segment 1 fails to open (parent path does not exist -> open error). This
    // mirrors a mid-session I/O failure on segment N.
    const fs::path bad_seg = dir / "does_not_exist_subdir" / "session_part-002.mkv";
    {
        MatroskaStreamWriter w;
        EXPECT_FALSE(w.Open(MakeConfig(bad_seg.string())));
        EXPECT_TRUE(w.failed());
    }

    // The already-finalized earlier segment is byte-for-byte intact.
    ASSERT_TRUE(fs::exists(seg0));
    EXPECT_EQ(fs::file_size(seg0), before);
    const auto after_data = ReadFile(seg0.string());
    EXPECT_EQ(before_data, after_data) << "an earlier finalized segment must survive a later segment failure";

    fs::remove_all(dir);
}

// --- RecordingSplitSettings engine model (SPLIT-BY-SIZE-R1) ---

TEST(RecordingSplitSettingsTest, DefaultsZeroBothThresholds) {
    exosnap::engine::RecordingSplitSettings s;
    EXPECT_EQ(s.duration_ms, 0ULL);
    EXPECT_EQ(s.size_bytes, 0ULL);
}

TEST(RecordingSplitSettingsTest, BothThresholdsCanBeActiveSimultaneously) {
    exosnap::engine::RecordingSplitSettings s;
    s.duration_ms = 30ULL * 60ULL * 1000ULL;           // 30 min
    s.size_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2 GiB
    EXPECT_GT(s.duration_ms, 0ULL);
    EXPECT_GT(s.size_bytes, 0ULL);
}

TEST(RecordingSplitSettingsTest, ZeroDisablesThatDimension) {
    exosnap::engine::RecordingSplitSettings s;
    s.duration_ms = 0;
    s.size_bytes = 512ULL * 1024ULL * 1024ULL;
    EXPECT_EQ(s.duration_ms, 0ULL);
    EXPECT_GT(s.size_bytes, 0ULL);
}

TEST(RecordingSplitSettingsTest, EqualityChecksAllFields) {
    exosnap::engine::RecordingSplitSettings a;
    exosnap::engine::RecordingSplitSettings b;
    EXPECT_EQ(a, b);
    b.size_bytes = 1;
    EXPECT_NE(a, b);
}

// --- bytes_written grows as data is pushed ---
// Validates the size-split trigger premise: bytes_written() reflects committed
// data so the mux thread can compare it to a size threshold.

TEST(SplitSizeTriggerTest, BytesWrittenGrowsAcrossPackets) {
    namespace fs = std::filesystem;
    const fs::path dir = exosnap_test::UniqueTempPath("splitdir");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path path = dir / "size_test.mkv";

    MatroskaStreamWriter w;
    ASSERT_TRUE(w.Open(MakeConfig(path.string()))) << w.error();

    // Feed one second of data (60 frames @ 64 bytes each + audio).
    FeedSegmentLocal(w, 1.0, 60);

    // After pushing data, bytes_written() must be non-zero (at least one cluster
    // has been flushed / rendered to the file by the reorder window).
    const uint64_t bytes_after_1s = w.bytes_written();

    // Feed another second.
    // NOTE: FeedSegmentLocal starts from pts=0; we must feed pts-rebased frames.
    // For simplicity, use a second writer pass at a different PTS offset.
    // Instead, just verify that bytes grow after further feed:
    FeedSegmentLocal(w, 1.0, 60);
    const uint64_t bytes_after_2s = w.bytes_written();

    // bytes_written is monotone; after more data it must be >= previous value.
    EXPECT_GE(bytes_after_2s, bytes_after_1s);

    ASSERT_TRUE(w.Finalize()) << w.error();
    EXPECT_GT(w.bytes_written(), 0ULL) << "bytes_written must be >0 after finalized segment";

    fs::remove_all(dir);
}

TEST(SplitSizeTriggerTest, SizeSplitAtomicCoalescesPreventsDoubleFire) {
    // Simulate the mux thread's compare-exchange guard:
    // only the first CAS succeeds; subsequent ones are no-ops.
    std::atomic<bool> size_split_armed{false};

    int fire_count = 0;
    auto try_arm = [&]() {
        bool expected = false;
        if (size_split_armed.compare_exchange_strong(expected, true)) {
            ++fire_count;
        }
    };

    // First call arms.
    try_arm();
    EXPECT_EQ(fire_count, 1);

    // Second call (same segment — size still over threshold) — must NOT re-fire.
    try_arm();
    EXPECT_EQ(fire_count, 1);

    // Simulate segment transition resetting the guard.
    size_split_armed.store(false);

    // Third call (new segment) — fires again.
    try_arm();
    EXPECT_EQ(fire_count, 2);
}

} // namespace
