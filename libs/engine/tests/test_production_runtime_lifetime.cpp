#include "file_output.h"
#include "matroska_stream_writer.h"
#include "production_runtime.h"
#include "recording_audio_manifest.h"
#include "session_internal.h"
#include <cstdint>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>
#include <utility>

#include <gtest/gtest.h>

#include <memory>

namespace exosnap::engine {
namespace {

TEST(RecordingManifest, KeepsSourceAndTrackIdentityAcrossSegmentsWithoutDeviceBindings) {
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.spec.binding.endpoint_id = "private-endpoint";
    source.spec.post_dsp_gain_linear = 0.5f;
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.name = "Custom stem";
    track.role = AudioTrackRole::Stem;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 0.25f});
    track.fixed_average_divisor = 1;
    plan.tracks.push_back(track);
    plan.default_monitor_track_ids.push_back(track.id);
    MatroskaStreamConfig config;
    config.audio_codec = StreamAudioCodec::Pcm;
    config.audio_track_count = 1;
    config.audio_tracks[0].name = track.name;
    const auto recording_id = NewId<RecordingId>();
    const auto first = BuildRecordingAudioManifest(plan, config, recording_id, 0);
    const auto second = BuildRecordingAudioManifest(plan, config, recording_id, 2'000'000'000ULL);
    ASSERT_TRUE(first.succeeded()) << first.error()->context;
    ASSERT_TRUE(second.succeeded()) << second.error()->context;
    EXPECT_EQ(first.value()->recording_id, second.value()->recording_id);
    EXPECT_NE(first.value()->media_id, second.value()->media_id);
    EXPECT_NE(first.value()->segment_id, second.value()->segment_id);
    EXPECT_EQ(first.value()->tracks[0].container.track_id, track.id);
    EXPECT_EQ(first.value()->tracks[0].routes[0].source_id, source.spec.id);
    EXPECT_EQ(first.value()->tracks[0].container.role, "stem");
    EXPECT_EQ(second.value()->segment_origin_r_ns, 2'000'000'000LL);
    const auto serialized = SerializeAudioManifest1(*first.value());
    ASSERT_TRUE(serialized.succeeded());
    EXPECT_EQ(serialized.value()->find("private-endpoint"), std::string::npos);
}

TEST(RuntimeCommand, ContentIdentityTracksResolvedAudioAndWebcamWithoutObjectAddresses) {
    RecorderConfig config;
    config.output_path = L"test.mkv";
    const auto original = RuntimeConfigContentKey(config);
    EXPECT_EQ(original, RuntimeConfigContentKey(RecorderConfig(config)));
    EXPECT_EQ(original.size(), 64u);
    auto changed = config;
    changed.webcam.chroma_softness = 0.33f;
    EXPECT_NE(original, RuntimeConfigContentKey(changed));
    changed = config;
    changed.resolved_audio_plan = ResolvedAudioPlan{};
    EXPECT_NE(original, RuntimeConfigContentKey(changed));
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    changed.resolved_audio_plan->sources.push_back(source);
    const auto graph_key = RuntimeConfigContentKey(changed);
    changed.resolved_audio_plan->sources.front().spec.dsp.agc_enabled = true;
    EXPECT_NE(graph_key, RuntimeConfigContentKey(changed));
    changed = config;
    changed.color.mastering_display_max_luminance = 1000;
    EXPECT_NE(original, RuntimeConfigContentKey(changed));
}

TEST(RuntimeFrameLease, CpuReleaseDoesNotCompleteGpuWrite) {
    RuntimeFramePool pool(1, 7, {1});
    const auto slot = pool.Reserve();
    ASSERT_TRUE(slot);
    auto texture = std::make_shared<int>(91);
    const auto token = pool.BeginWrite(*slot, texture);
    ASSERT_TRUE(token.succeeded());
    auto frame = pool.Publish(*slot, 1, {123}, {});
    ASSERT_TRUE(frame.succeeded());
    EXPECT_EQ(frame.value()->texture(), texture.get());
    frame = EngineError{};
    EXPECT_FALSE(pool.Reserve());
    EXPECT_TRUE(pool.Complete(*token.value()));
    EXPECT_TRUE(pool.Reserve());
}

TEST(RuntimeFrameLease, CompletedGpuWriteCannotReuseHeldReaderSurface) {
    RuntimeFramePool pool(1, 7, {1});
    const auto slot = pool.Reserve();
    ASSERT_TRUE(slot);
    const auto token = pool.BeginWrite(*slot, std::make_shared<int>(4));
    ASSERT_TRUE(token.succeeded());
    auto frame = pool.Publish(*slot, 3, {89}, {});
    ASSERT_TRUE(frame.succeeded());
    EXPECT_TRUE(pool.Complete(*token.value()));
    EXPECT_FALSE(pool.Reserve());
    EXPECT_EQ(frame.value()->sequence(), 3u);
    frame = EngineError{};
    EXPECT_TRUE(pool.Reserve());
}

TEST(RuntimeFrameLease, OneRetiredGenerationIsBoundedAndLateCompletionIsScoped) {
    RuntimeFramePool pool(1, 7, {1});
    auto slot = pool.Reserve();
    ASSERT_TRUE(slot);
    const auto old = pool.BeginWrite(*slot, std::make_shared<int>(11));
    ASSERT_TRUE(old.succeeded());
    auto frame = pool.Publish(*slot, 1, {}, {});
    ASSERT_TRUE(frame.succeeded());
    EXPECT_TRUE(pool.RotateGeneration({2}));
    EXPECT_EQ(pool.retired_slots(), 1u);
    EXPECT_FALSE(pool.RotateGeneration({3}));
    EXPECT_FALSE(pool.Complete({8, {1}, old.value()->submit_serial}));
    EXPECT_TRUE(pool.Complete(*old.value()));
    EXPECT_FALSE(pool.RotateGeneration({3}));
    frame = EngineError{};
    EXPECT_TRUE(pool.RotateGeneration({3}));
    EXPECT_EQ(pool.retired_slots(), 0u);
}

TEST(RuntimeFrameLease, CompletedClosedReaderResourcesRemainBudgetedUntilCpuRelease) {
    RuntimeFramePool pool(1, 7, {1});
    RawVideoReader reader;
    auto slot = pool.Reserve();
    ASSERT_TRUE(slot);
    auto token = pool.BeginWrite(*slot, std::make_shared<int>(17));
    ASSERT_TRUE(token.succeeded());
    auto frame = pool.Publish(*slot, 1, {}, {});
    ASSERT_TRUE(frame.succeeded());
    ASSERT_TRUE(reader.TryPublish(std::move(*frame.value())));
    frame = EngineError{};
    ASSERT_TRUE(pool.Complete(*token.value()));
    auto held = reader.TakeLatest();
    ASSERT_TRUE(held);
    reader.Close();
    EXPECT_TRUE(pool.idle());
    EXPECT_FALSE(pool.reclaimable());
    EXPECT_EQ(*static_cast<int*>(held->texture()), 17);
    EXPECT_TRUE(pool.RotateGeneration({2}));
    EXPECT_FALSE(pool.reclaimable());
    EXPECT_FALSE(pool.RotateGeneration({3}));
    held.reset();
    EXPECT_TRUE(pool.reclaimable());
    EXPECT_TRUE(pool.RotateGeneration({3}));
}

TEST(RuntimeOutputLifetime, StopDuringPrepareCannotAttachOrResetOtherReader) {
    auto runtime = std::make_shared<ProductionRuntime>();
    auto state = std::make_shared<SessionState>();
    state->config.record_audio = false;
    RuntimeOutputCommand command{NewId<CommandId>(), {1}, {1}, "first-output"};
    auto output = runtime->PrepareOutput(state, command);
    ASSERT_TRUE(output.succeeded());
    output.value()->get()->RequestStop({77});
    EXPECT_FALSE(runtime->AttachOutput(*output.value()).succeeded());
    EXPECT_EQ(output.value()->get()->phase(), FileOutputPhase::Closed);
    EXPECT_EQ(output.value()->get()->end_qpc().value, 77u);
    EXPECT_EQ(runtime->active_output(), nullptr);
}

TEST(RuntimeOutputLifetime, ClosedOutputLeavesRuntimeAndNextGenerationIndependent) {
    auto runtime = std::make_shared<ProductionRuntime>();
    auto first_state = std::make_shared<SessionState>();
    RuntimeOutputCommand first{NewId<CommandId>(), {1}, {1}, "first-output"};
    auto first_output = runtime->PrepareOutput(first_state, first);
    ASSERT_TRUE(first_output.succeeded());
    ASSERT_TRUE(runtime->AttachOutput(*first_output.value()).succeeded());
    ASSERT_EQ(runtime->TakePendingOutput(), *first_output.value());
    first_output.value()->get()->RequestStop({100});
    first_output.value()->get()->VideoDrained();
    runtime->OutputDetached(*first_output.value());
    auto second_state = std::make_shared<SessionState>();
    RuntimeOutputCommand second{NewId<CommandId>(), {1}, {2}, "second-output"};
    auto second_output = runtime->PrepareOutput(second_state, second);
    ASSERT_TRUE(second_output.succeeded());
    ASSERT_TRUE(runtime->AttachOutput(*second_output.value()).succeeded());
    EXPECT_NE(*first_output.value(), *second_output.value());
    EXPECT_EQ(first_output.value()->get()->end_qpc().value, 100u);
    EXPECT_EQ(second_output.value()->get()->end_qpc().value, 0u);
    EXPECT_FALSE(runtime->stopping());
}

TEST(RuntimeOutputLifetime, RepeatedCommandIsIdempotentAndConflictingContentRejected) {
    auto runtime = std::make_shared<ProductionRuntime>();
    auto state = std::make_shared<SessionState>();
    RuntimeOutputCommand command{NewId<CommandId>(), {1}, {1}, "same-intent"};
    auto first = runtime->PrepareOutput(state, command);
    ASSERT_TRUE(first.succeeded());
    auto repeat = runtime->PrepareOutput(state, command);
    ASSERT_TRUE(repeat.succeeded());
    EXPECT_EQ(*first.value(), *repeat.value());
    command.content_key = "different-intent";
    EXPECT_FALSE(runtime->PrepareOutput(state, command).succeeded());
    command.command_id = NewId<CommandId>();
    command.expected_revision = {999};
    auto stale = runtime->PrepareOutput(state, command);
    ASSERT_FALSE(stale.succeeded());
    EXPECT_EQ(stale.error()->code, EngineErrorCode::StaleRevision);
}

TEST(RuntimeOutputLifetime, OutputOwnsNoStrongSessionStateCycle) {
    auto runtime = std::make_shared<ProductionRuntime>();
    auto state = std::make_shared<SessionState>();
    auto output = runtime->PrepareOutput(state, {NewId<CommandId>(), {1}, {1}, "output"});
    ASSERT_TRUE(output.succeeded());
    state->production_runtime = runtime;
    state->file_output = *output.value();
    std::weak_ptr<SessionState> weak = state;
    state.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(output.value()->get()->LockState(), nullptr);
}

TEST(RuntimeOutputLifetime, ShutdownSignalsFiniteOutputAndRawReaderWithoutJoiningWorker) {
    auto runtime = std::make_shared<ProductionRuntime>();
    auto state = std::make_shared<SessionState>();
    auto reader = runtime->OpenRawReader(state->config);
    ASSERT_TRUE(reader.succeeded());
    auto output = runtime->PrepareOutput(state, {NewId<CommandId>(), {1}, {1}, "shutdown"});
    ASSERT_TRUE(output.succeeded());
    ASSERT_TRUE(runtime->AttachOutput(*output.value()).succeeded());
    runtime->RequestShutdown();
    EXPECT_TRUE(runtime->stopping());
    EXPECT_TRUE(state->stop_requested.load());
    EXPECT_TRUE(reader.value()->get()->closed());
    EXPECT_EQ(output.value()->get()->phase(), FileOutputPhase::Closing);
    EXPECT_GT(output.value()->get()->end_qpc().value, 0u);
    EXPECT_EQ(WaitForSingleObject(output.value()->get()->VideoEndedHandle(), 0), WAIT_TIMEOUT);
    output.value()->get()->VideoDrained();
    EXPECT_EQ(WaitForSingleObject(output.value()->get()->VideoEndedHandle(), 0), WAIT_OBJECT_0);
}

TEST(RuntimeFrameLease, OptionalReaderOnlyReturnsCompletedFramesAndHasIndependentSlots) {
    RuntimeFramePool first(3, 7, {1}), second(3, 7, {1});
    RawVideoReader held_reader, progressing_reader;
    auto slot = first.Reserve();
    ASSERT_TRUE(slot);
    auto token = first.BeginWrite(*slot, std::make_shared<int>(1));
    ASSERT_TRUE(token.succeeded());
    auto frame = first.Publish(*slot, 1, {}, {});
    ASSERT_TRUE(frame.succeeded());
    ASSERT_TRUE(held_reader.TryPublish(*frame.value()));
    EXPECT_FALSE(held_reader.TakeLatest());
    ASSERT_TRUE(first.Complete(*token.value()));
    auto held = held_reader.TakeLatest();
    ASSERT_TRUE(held);
    for (uint64_t sequence = 1; sequence <= 10; ++sequence) {
        slot = second.Reserve();
        ASSERT_TRUE(slot);
        token = second.BeginWrite(*slot, std::make_shared<int>(static_cast<int>(sequence)));
        ASSERT_TRUE(token.succeeded());
        auto fresh = second.Publish(*slot, sequence, {}, {});
        ASSERT_TRUE(fresh.succeeded());
        ASSERT_TRUE(second.Complete(*token.value()));
        ASSERT_TRUE(progressing_reader.TryPublish(std::move(*fresh.value())));
        auto observed = progressing_reader.TakeLatest();
        ASSERT_TRUE(observed);
        EXPECT_EQ(observed->sequence(), sequence);
    }
    EXPECT_EQ(held->sequence(), 1u);
    EXPECT_EQ(progressing_reader.frames_published(), 10u);
    held_reader.Close();
    EXPECT_EQ(held->texture(), frame.value()->texture());
}

} // namespace
} // namespace exosnap::engine
