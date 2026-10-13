#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/recording_media_types.h>

#include <gtest/gtest.h>
#include <limits>

#include <type_traits>

namespace exosnap::engine {
namespace {

static_assert(sizeof(AudioSourceId) == 16);
static_assert(std::is_trivially_copyable_v<AudioSourceId>);
static_assert(!std::is_convertible_v<AudioSourceId, AudioTrackId>);

TEST(AudioIdentity, CanonicalCodecRoundTripsAllBytesAndRejectsMalformedInput) {
    const auto id = FromUuid<AudioSourceId>("00112233-4455-6677-8899-aabbccddeeff");
    ASSERT_TRUE(id);
    EXPECT_EQ(ToUuid(*id), "00112233-4455-6677-8899-aabbccddeeff");
    EXPECT_EQ(id->bytes.front(), 0x00);
    EXPECT_EQ(id->bytes.back(), 0xff);
    EXPECT_EQ(FromUuid<AudioSourceId>("00112233-4455-6677-8899-AABBCCDDEEFF"), id);
    EXPECT_FALSE(FromUuid<AudioSourceId>("00112233445566778899aabbccddeeff"));
    EXPECT_FALSE(FromUuid<AudioSourceId>("00112233_4455-6677-8899-aabbccddeeff"));
    EXPECT_FALSE(FromUuid<AudioSourceId>("00112233-4455-6677-8899-aabbccddeefg"));
    EXPECT_FALSE(FromUuid<AudioSourceId>("00112233-4455-6677-8899-aabbccddeeffx"));
}

TEST(AudioIdentity, NewIdentityIsVersionFourAndRoundTrips) {
    const auto first = NewId<AudioSourceId>();
    const auto second = NewId<AudioSourceId>();
    EXPECT_NE(first, second);
    EXPECT_EQ(first.bytes[6] >> 4, 4);
    EXPECT_EQ(first.bytes[8] >> 6, 2);
    EXPECT_EQ(FromUuid<AudioSourceId>(ToUuid(first)), first);
}

TEST(AudioOutcome, FailureCarriesNoUsablePlan) {
    EngineError error;
    error.code = EngineErrorCode::UnsupportedFormat;
    error.context = "unsupported audio layout";
    const Outcome<ResolvedAudioPlan> outcome{error};
    EXPECT_FALSE(outcome.succeeded());
    EXPECT_EQ(outcome.value(), nullptr);
    ASSERT_NE(outcome.error(), nullptr);
    EXPECT_EQ(outcome.error()->code, EngineErrorCode::UnsupportedFormat);
}

TEST(AudioPacketBudgets, EightShortOpusTracksFitThreeSecondsAndReorderMargin) {
    EXPECT_EQ(AudioHoldPacketLimit(0), 0u);
    EXPECT_EQ(AudioHoldPacketLimit(1), 1200u);
    EXPECT_EQ(AudioHoldPacketLimit(3), 3600u);
    EXPECT_EQ(AudioHoldPacketLimit(8), 9600u);
    EXPECT_EQ(ReorderPacketLimit(8, 30, 2500000, 0), 10498u);
    EXPECT_EQ(ReorderPacketLimit(1, 60, 20000000, 0), 4096u);
}

TrackGraph1Intent GraphWithTracks(uint32_t count) {
    TrackGraph1Intent graph;
    AudioSourceSpec source;
    source.id.bytes[15] = 1;
    graph.sources.push_back(source);
    for (uint32_t index = 0; index < count; ++index) {
        AudioTrackSpec track;
        track.id.bytes[15] = static_cast<uint8_t>(index + 1);
        track.order = index;
        graph.tracks.push_back(track);
        AudioRouteSpec route;
        route.id.bytes[15] = static_cast<uint8_t>(index + 1);
        route.source_id = source.id;
        route.track_id = track.id;
        graph.routes.push_back(route);
    }
    return graph;
}

TEST(AudioRoutingValidation, CountsRecordedTracksAndKeepsSilentRoutedTracks) {
    for (const uint32_t count : {0u, 1u, 3u, 8u}) {
        auto graph = GraphWithTracks(count);
        EXPECT_TRUE(ValidateTrackGraph1Intent(graph).succeeded());
        graph.sources.front().participation = false;
        graph.sources.front().muted = true;
        EXPECT_TRUE(ValidateTrackGraph1Intent(graph).succeeded());
    }
    auto graph = GraphWithTracks(9);
    const auto rejected = ValidateTrackGraph1Intent(graph);
    ASSERT_FALSE(rejected.succeeded());
    ASSERT_NE(rejected.error(), nullptr);
    EXPECT_EQ(rejected.error()->code, EngineErrorCode::ResourceBudgetExceeded);
    graph.routes.pop_back();
    EXPECT_TRUE(ValidateTrackGraph1Intent(graph).succeeded());
}

TEST(AudioRoutingValidation, RejectsDuplicateIdentityDanglingReferencesAndDuplicatePairs) {
    auto graph = GraphWithTracks(2);
    graph.tracks[1].id = graph.tracks[0].id;
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph = GraphWithTracks(2);
    graph.routes[1].source_id.bytes[15] = 99;
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph = GraphWithTracks(2);
    graph.routes[1].track_id = graph.routes[0].track_id;
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph = GraphWithTracks(2);
    graph.routes[1].id = graph.routes[0].id;
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
}

TEST(AudioRoutingValidation, RejectsNonfiniteDspParametersBeforeSourcePreparation) {
    auto graph = GraphWithTracks(1);
    graph.sources.front().dsp.hpf_cutoff_hz = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.sources.front().dsp.hpf_cutoff_hz = 80.0f;
    graph.sources.front().dsp.gate_threshold_db = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.sources.front().dsp.gate_threshold_db = -45.0f;
    graph.sources.front().dsp.agc_target_db = -std::numeric_limits<float>::infinity();
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
}

TEST(AudioRoutingValidation, ValidatesUnicodeCustomNamesAndGain) {
    auto graph = GraphWithTracks(1);
    graph.tracks.front().name_kind = AudioTrackNameKind::Custom;
    graph.tracks.front().name = "Voice";
    EXPECT_TRUE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.tracks.front().name = std::string(129, 'a');
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.tracks.front().name = "Line\nBreak";
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.tracks.front().name = std::string(1, static_cast<char>(0xff));
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
    graph.tracks.front().name = "Voice";
    graph.routes.front().gain_db = 25.0f;
    EXPECT_FALSE(ValidateTrackGraph1Intent(graph).succeeded());
}

} // namespace
} // namespace exosnap::engine
