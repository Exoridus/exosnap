#include "audio_runtime_graph.h"
#include "qpc_100ns.h"
#include "shared_audio_capture.h"
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <atomic>
#include <gtest/gtest.h>
#include <windows.h>

namespace exosnap::engine {
namespace {
uint64_t NowNs() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), static_cast<uint64_t>(frequency.QuadPart)) * 100;
}
class FiniteCapture final : public IAudioCaptureSource {
  public:
    explicit FiniteCapture(std::shared_ptr<std::atomic<uint32_t>> opens, uint64_t start)
        : opens_(std::move(opens)), start_(start), pcm_(960, 0.25f) {
    }
    bool Init(std::string&) override {
        ++*opens_;
        return true;
    }
    uint32_t PendingFrameCount() override {
        return packets_ < 20 ? 480 : 0;
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string&) override {
        if (!PendingFrameCount())
            return false;
        buffer = {};
        buffer.bytes = reinterpret_cast<const uint8_t*>(pcm_.data());
        buffer.num_frames = 480;
        return true;
    }
    void ReleaseBuffer() override {
        ++packets_;
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return 2;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    uint64_t LastBufferQpcNs() const override {
        return start_ + packets_ * 10000000ULL;
    }
    void Shutdown() override {
    }

  private:
    std::shared_ptr<std::atomic<uint32_t>> opens_;
    uint64_t start_;
    uint32_t packets_ = 0;
    std::vector<float> pcm_;
    std::string name_ = "Finite fixture";
};
TEST(AudioRuntimeGraph, SharedCaptureProcessesEachSourceOnceAndFansOutToTwoTracks) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    plan.revision = {1};
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    for (uint32_t i = 0; i < 2; ++i) {
        ResolvedRoutingTrack track;
        track.id = NewId<AudioTrackId>();
        track.mux_index = {i};
        track.fixed_average_divisor = 1;
        track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.0f});
        plan.tracks.push_back(track);
    }
    auto opens = std::make_shared<std::atomic<uint32_t>>(0);
    auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    auto graph = AudioRuntimeGraph::Prepare(
        plan, registry, start, 48000, 2, false, error, [opens, start](const ResolvedAudioSource& resolved) {
            SharedAudioCaptureRequest request;
            request.key = *resolved.capture_key;
            request.admission.sample_rate = 48000;
            request.admission.channels = 2;
            request.admission.maximum_packet_frames = 480;
            request.native_factory = [opens, start] { return std::make_unique<FiniteCapture>(opens, start); };
            return request;
        });
    ASSERT_NE(graph, nullptr) << error;
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 2u);
    for (auto& track : tracks)
        ASSERT_TRUE(track->Init(error)) << error;
    graph->Start();
    for (uint32_t wait = 0; wait < 2000; ++wait) {
        const auto meters = graph->SourceMeters();
        if (!meters.empty() && meters.front().processed_frames == 9600)
            break;
        Sleep(1);
    }
    const auto meters = graph->SourceMeters();
    ASSERT_EQ(meters.size(), 1u);
    EXPECT_EQ(meters.front().processed_blocks, 20u);
    EXPECT_EQ(meters.front().processed_frames, 9600u);
    EXPECT_EQ(opens->load(), 1u);
    graph->RequestStop(start + 200000000ULL);
    ASSERT_TRUE(graph->WaitDrained(3000, error)) << error;
    std::vector<float> first;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto* track = dynamic_cast<AudioRuntimeTrackSource*>(tracks[i].get());
        ASSERT_NE(track, nullptr);
        ASSERT_TRUE(track->BeginDrain(start + (i == 0 ? 200000000ULL : 300000000ULL), error)) << error;
        std::vector<float> samples;
        while (track->PendingFrameCount()) {
            RawAudioBuffer raw;
            ASSERT_TRUE(track->AcquireBuffer(raw, error)) << error;
            const auto* pcm = reinterpret_cast<const float*>(raw.bytes);
            samples.insert(samples.end(), pcm, pcm + static_cast<size_t>(raw.num_frames) * 2);
            track->ReleaseBuffer();
        }
        ASSERT_FALSE(samples.empty());
        if (i == 0)
            first = samples;
        else
            EXPECT_EQ(first, samples);
    }
    tracks.clear();
    graph.reset();
    for (uint32_t wait = 0; wait < 1000 && registry->ReservedPayloadBytes(); ++wait)
        Sleep(1);
    EXPECT_EQ(registry->ReservedPayloadBytes(), 0u);
}

TEST(AudioRuntimeGraph, SilentSourcesUseTheCommonStartWithoutOpeningCapture) {
    const auto start = NowNs();
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.silence_for_this_start = true;
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.fixed_average_divisor = 1;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.0f});
    plan.tracks.push_back(track);
    auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    auto graph = AudioRuntimeGraph::Prepare(plan, registry, start, 48000, 2, false, error);
    ASSERT_NE(graph, nullptr) << error;
    auto outputs = graph->TakeTracks();
    Sleep(20);
    ASSERT_TRUE(outputs.front()->Init(error)) << error;
    graph->Start();
    auto* mixer = dynamic_cast<AudioRuntimeTrackSource*>(outputs.front().get());
    ASSERT_NE(mixer, nullptr);
    ASSERT_TRUE(mixer->BeginDrain(start + 100000000ULL, error)) << error;
    RawAudioBuffer raw;
    ASSERT_TRUE(mixer->AcquireBuffer(raw, error)) << error;
    EXPECT_LE(start - mixer->LastBufferQpcNs(), 1000000000ULL / 48000);
    EXPECT_LE(mixer->LastBufferQpcNs(), start);
    mixer->ReleaseBuffer();
    EXPECT_TRUE(graph->SourceMeters().empty());
    EXPECT_EQ(registry->Statistics().capture_opens, 0u);
}
} // namespace
} // namespace exosnap::engine
