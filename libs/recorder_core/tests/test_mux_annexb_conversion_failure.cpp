// Annex-B -> avcC/hvcC sample conversion failures must fail the recording.
//
// The Matroska track header carries an avcC / hvcC CodecPrivate, so every
// H.264 / HEVC sample written into that track must be length-prefixed. When the
// per-packet conversion fails, the muxer used to fall back to the raw Annex-B
// bytes: the resulting file's samples contradict its own track header, players
// reject it, and the session still reported success. These tests pin the
// loud-failure behavior instead — a conversion failure is recorded as a Mux
// phase failure, exactly like a writer I/O error.

#include <gtest/gtest.h>

#include "annexb_to_avcc.h"
#include "mux_thread.h"
#include "session_internal.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include "test_unique_temp.h"

namespace {

using recorder_core::EncodedVideoPacket;
using recorder_core::ErrorPhase;
using recorder_core::MuxItem;
using recorder_core::MuxThread;
using recorder_core::SessionState;
using recorder_core::VideoCodec;
using recorder_core::VideoEosSentinel;

// Minimal well-formed Annex-B access unit: SPS (type 7), PPS (type 8), IDR
// (type 5). Enough for both the avcC builder and the per-packet converter.
std::vector<uint8_t> MakeAnnexBAccessUnit() {
    return {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x28, // SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80, // PPS
        0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22, 0x33, // IDR slice
    };
}

// Prepares an H.264 session state whose mux thread can open a real segment file.
std::shared_ptr<SessionState> MakeH264State(const std::filesystem::path& out_path) {
    auto state = std::make_shared<SessionState>();
    state->config.output_path = out_path;
    state->config.video_codec = VideoCodec::H264;
    state->config.frame_rate_num = 60;
    state->config.frame_rate_den = 1;
    state->audio_track_count = 0;
    state->encode_width = 640;
    state->encode_height = 360;

    const std::vector<uint8_t> au = MakeAnnexBAccessUnit();
    std::vector<uint8_t> sps_pps;
    EXPECT_TRUE(recorder_core::annexb::ExtractH264SpsAndPps(au.data(), au.size(), sps_pps));
    state->codec_private.h264_sps_pps = sps_pps;
    state->codec_private.h264_ready = true;
    return state;
}

void PushVideo(SessionState& state, std::vector<uint8_t> bytes, uint64_t pts_ns) {
    EncodedVideoPacket pkt;
    pkt.bytes = std::move(bytes);
    pkt.pts_ns = pts_ns;
    pkt.keyframe = true;
    MuxItem item;
    item.payload = std::move(pkt);
    std::lock_guard lk(state.mux_mutex);
    state.PushMuxItemLocked(std::move(item));
    state.mux_cv.notify_all();
}

void PushVideoEos(SessionState& state) {
    MuxItem item;
    item.payload = VideoEosSentinel{};
    std::lock_guard lk(state.mux_mutex);
    state.PushMuxItemLocked(std::move(item));
    state.mux_cv.notify_all();
}

void RemoveQuietly(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

TEST(MuxAnnexBConversionFailure, UnconvertibleH264PacketFailsTheRecording) {
    const std::filesystem::path out = exosnap_test::UniqueTempPath("mux_annexb_fail.mkv");
    auto state = MakeH264State(out);

    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();

    // No start code anywhere: ConvertAnnexBToAvcc finds no NAL units and fails.
    PushVideo(*state, {0xDE, 0xAD, 0xBE, 0xEF}, 0);
    PushVideoEos(*state);

    ASSERT_TRUE(mux->Join(10000));

    ASSERT_TRUE(state->HasFailure());
    std::lock_guard lk(state->failure_mutex);
    EXPECT_EQ(state->failure.error_phase, ErrorPhase::Mux);
    EXPECT_NE(state->failure.error_detail.find("Annex-B"), std::string::npos);

    RemoveQuietly(out);
}

TEST(MuxAnnexBConversionFailure, ConvertibleH264PacketKeepsTheRecordingHealthy) {
    const std::filesystem::path out = exosnap_test::UniqueTempPath("mux_annexb_ok.mkv");
    auto state = MakeH264State(out);

    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();

    PushVideo(*state, MakeAnnexBAccessUnit(), 0);
    PushVideoEos(*state);

    ASSERT_TRUE(mux->Join(10000));

    EXPECT_FALSE(state->HasFailure());

    RemoveQuietly(out);
}

} // namespace
