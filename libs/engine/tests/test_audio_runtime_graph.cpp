#include "audio_runtime_graph.h"
#include "audio_tee.h"
#include "logical_audio_source.h"
#include "shared_audio_capture.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/logging/logging.h>
#include <exosnap/engine/qpc_100ns.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/session_stats.h>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <system_error>
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
bool WaitForGraphCondition(auto condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        Sleep(1);
    }
    return true;
}

struct StopGraphOnExit {
    AudioRuntimeGraph& graph;
    ~StopGraphOnExit() {
        graph.RequestStop(NowNs());
        std::string error;
        graph.WaitDrained(1500, error);
    }
};

struct FiniteCapturePacketGate {
    std::atomic<bool> released{false};
    std::atomic<uint32_t> empty_polls{0};
};

class FiniteCapture final : public IAudioCaptureSource {
  public:
    explicit FiniteCapture(std::shared_ptr<std::atomic<uint32_t>> opens, uint64_t start, uint32_t packet_limit = 20,
                           uint64_t delivery_delay_ns = 0, bool latency_known = true,
                           std::shared_ptr<FiniteCapturePacketGate> gate = {})
        : opens_(std::move(opens)), start_(start), packet_limit_(packet_limit), delivery_delay_ns_(delivery_delay_ns),
          latency_known_(latency_known), gate_(std::move(gate)), pcm_(960, 0.25f) {
    }
    bool Init(std::string&) override {
        ++*opens_;
        return true;
    }
    std::optional<uint64_t> MaxCaptureLatencyNs() const override {
        return latency_known_ ? std::optional<uint64_t>{50000000ULL} : std::nullopt;
    }
    uint32_t PendingFrameCount() override {
        if (packets_ >= packet_limit_ || NowNs() < start_ + delivery_delay_ns_)
            return 0;
        if (packets_ && gate_ && !gate_->released.load()) {
            ++gate_->empty_polls;
            return 0;
        }
        return 480;
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
    uint32_t packet_limit_ = 20;
    uint64_t delivery_delay_ns_ = 0;
    bool latency_known_ = true;
    std::shared_ptr<FiniteCapturePacketGate> gate_;
    std::vector<float> pcm_;
    std::string name_ = "Finite fixture";
};

AudioRuntimeGraph::RequestFactory MakeGatedFiniteCaptureRequest(std::shared_ptr<std::atomic<uint32_t>> opens,
                                                                uint64_t start, bool latency_known,
                                                                std::shared_ptr<FiniteCapturePacketGate> gate) {
    return
        [opens = std::move(opens), start, latency_known, gate = std::move(gate)](const ResolvedAudioSource& resolved) {
            SharedAudioCaptureRequest request;
            request.key = *resolved.capture_key;
            request.correction_enabled = false;
            request.admission.sample_rate = 48000;
            request.admission.channels = 2;
            request.admission.maximum_packet_frames = 480;
            request.native_factory = [opens, start, latency_known, gate] {
                return std::make_unique<FiniteCapture>(opens, start, 2, 0, latency_known, gate);
            };
            return request;
        };
}

struct OutageControl {
    std::atomic<uint32_t> phase{0};
    std::atomic<uint32_t> active_captures{0};
    std::atomic<uint32_t> peak_captures{0};
    std::atomic<bool> block_reinit{false};
    std::atomic<bool> reinit_entered{false};
    int64_t recovery_offset_ns = -50000000;
    std::atomic<uint64_t> last_native_end{0};
    std::atomic<uint64_t> recovery_qpc{0};
};

class OutageCapture final : public IAudioCaptureSource {
  public:
    OutageCapture(uint64_t start, bool fails, std::shared_ptr<OutageControl> control, float healthy_peak = 0.5f)
        : next_qpc_(start), fails_(fails), healthy_peak_(healthy_peak), control_(std::move(control)),
          pcm_(960, healthy_peak) {
    }
    bool Init(std::string&) override {
        const auto active = ++control_->active_captures;
        auto peak = control_->peak_captures.load();
        while (active > peak && !control_->peak_captures.compare_exchange_weak(peak, active)) {
        }
        opened_ = true;
        return true;
    }
    bool Reinit(std::string& error) override {
        control_->reinit_entered = true;
        while (control_->block_reinit)
            Sleep(1);
        if (control_->phase < 2) {
            error = "Capture remains unavailable";
            return false;
        }
        recovered_ = true;
        next_qpc_ = static_cast<uint64_t>(static_cast<int64_t>(NowNs()) + control_->recovery_offset_ns);
        control_->recovery_qpc = next_qpc_;
        return true;
    }
    uint32_t PendingFrameCount() override {
        return (fails_ && control_->phase == 1) || NowNs() >= next_qpc_ + 10000000ULL ? 480 : 0;
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override {
        if (fails_ && control_->phase == 1) {
            error = "Capture was disconnected";
            return false;
        }
        std::fill(pcm_.begin(), pcm_.end(), fails_ ? (recovered_ ? 0.75f : 0.25f) : healthy_peak_);
        buffer = {};
        buffer.bytes = reinterpret_cast<const uint8_t*>(pcm_.data());
        buffer.num_frames = 480;
        return true;
    }
    void ReleaseBuffer() override {
        next_qpc_ += 10000000ULL;
        if (fails_ && !recovered_)
            control_->last_native_end = next_qpc_;
    }
    std::optional<uint64_t> MaxCaptureLatencyNs() const override {
        return 50000000ULL;
    }
    uint64_t LastBufferQpcNs() const override {
        return next_qpc_;
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
    void Shutdown() override {
        if (opened_) {
            opened_ = false;
            --control_->active_captures;
        }
    }

  private:
    bool opened_ = false;
    uint64_t next_qpc_;
    bool fails_, recovered_ = false;
    float healthy_peak_;
    std::shared_ptr<OutageControl> control_;
    std::vector<float> pcm_;
    std::string name_ = "Outage fixture";
};

enum class DefaultRecoveryScenario {
    Immediate,
    Missing,
    ChangesWhileOpening,
    ChangesBeforeAcceptance,
    StopsDuringConfirmation,
    PointsToLostDevice,
    QueuedRecovery,
    StaleSameEndpoint,
    LossesBeforeStart
};

struct BlockedEndpointOpen {
    std::binary_semaphore entered{0};
    std::binary_semaphore finish{0};
};

class ScopedRecoveryLogBarrier {
  public:
    explicit ScopedRecoveryLogBarrier(const std::shared_ptr<BlockedEndpointOpen>& barrier)
        : path_(std::filesystem::temp_directory_path() /
                ("exosnap-audio-recovery-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(NowNs()) +
                 ".jsonl")) {
        logging::LoggerConfig config;
        config.filePath = path_;
        config.minimumLevel = logging::LogLevel::Warn;
        config.sink = [barrier](const logging::LogRecord& record) {
            if (record.component == "audio" &&
                record.message == "Current role-default endpoint could not be reopened") {
                barrier->entered.release();
                (void)barrier->finish.try_acquire_for(std::chrono::seconds(4));
            }
        };
        logging::initialize(config);
    }

    ~ScopedRecoveryLogBarrier() {
        logging::shutdown();
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

  private:
    std::filesystem::path path_;
};

void VerifyDefaultEndpointRecovery(DefaultRecoveryScenario scenario) {
    auto control = std::make_shared<OutageControl>();
    auto lookup_calls = std::make_shared<std::atomic<uint32_t>>(0);
    auto unexpected_role = std::make_shared<std::atomic<bool>>(false);
    const bool queued_recovery = scenario == DefaultRecoveryScenario::QueuedRecovery;
    const bool losses_before_start = scenario == DefaultRecoveryScenario::LossesBeforeStart;
    const bool stale_same_endpoint = scenario == DefaultRecoveryScenario::StaleSameEndpoint;
    char initial_endpoint = 'B';
    if (scenario == DefaultRecoveryScenario::Missing || queued_recovery)
        initial_endpoint = '\0';
    if (scenario == DefaultRecoveryScenario::PointsToLostDevice || stale_same_endpoint)
        initial_endpoint = 'A';
    auto role_endpoint = std::make_shared<std::atomic<char>>(initial_endpoint);
    auto blocked_open =
        scenario == DefaultRecoveryScenario::ChangesWhileOpening ? std::make_shared<BlockedEndpointOpen>() : nullptr;
    auto blocked_delivery = queued_recovery ? std::make_shared<BlockedEndpointOpen>() : nullptr;
    auto blocked_lookup = stale_same_endpoint ? std::make_shared<BlockedEndpointOpen>() : nullptr;
    auto blocked_confirmation = scenario == DefaultRecoveryScenario::ChangesBeforeAcceptance
                                    ? std::make_shared<BlockedEndpointOpen>()
                                    : nullptr;
    auto blocked_stop = scenario == DefaultRecoveryScenario::StopsDuringConfirmation
                            ? std::make_shared<BlockedEndpointOpen>()
                            : nullptr;
    if (queued_recovery || losses_before_start || stale_same_endpoint)
        control->recovery_offset_ns = 0;
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    for (uint32_t i = 0; i < 2; ++i) {
        ResolvedAudioSource source;
        source.spec.id = NewId<AudioSourceId>();
        source.spec.binding.follow_default = i == 0;
        source.spec.binding.endpoint_role = AudioEndpointRole::Communications;
        source.capture_key = PhysicalCaptureKey{};
        source.capture_key->endpoint_id = "endpoint-A";
        plan.sources.push_back(source);
        ResolvedRoutingTrack track;
        track.id = NewId<AudioTrackId>();
        track.mux_index = {i};
        track.fixed_average_divisor = 1;
        track.limiter.enabled = false;
        track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
        plan.tracks.push_back(std::move(track));
    }
    const auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    auto graph = AudioRuntimeGraph::Prepare(
        plan, registry, start, 48000, 2, false, error,
        [control, start, blocked_open](const ResolvedAudioSource& source) {
            SharedAudioCaptureRequest request;
            request.key = *source.capture_key;
            request.admission.sample_rate = 48000;
            request.admission.channels = 2;
            request.admission.maximum_packet_frames = 480;
            const bool endpoint_a = request.key.endpoint_id == "endpoint-A";
            const bool endpoint_b = request.key.endpoint_id == "endpoint-B";
            request.native_factory = [control, endpoint_a, endpoint_b, start, blocked_open] {
                if (endpoint_b && blocked_open) {
                    blocked_open->entered.release();
                    (void)blocked_open->finish.try_acquire_for(std::chrono::milliseconds(1500));
                }
                return std::make_unique<OutageCapture>(endpoint_a ? start : NowNs(), endpoint_a, control,
                                                       endpoint_b ? 0.5f : 0.625f);
            };
            return request;
        },
        [lookup_calls, unexpected_role, role_endpoint, blocked_lookup, blocked_confirmation,
         blocked_stop](AudioBindingKind kind, AudioEndpointRole role) {
            const char endpoint = role_endpoint->load();
            const auto call = ++*lookup_calls;
            const auto barrier = call == 1   ? blocked_lookup
                                 : call == 2 ? blocked_confirmation
                                 : call == 3 ? blocked_stop
                                             : nullptr;
            if (barrier) {
                barrier->entered.release();
                (void)barrier->finish.try_acquire_for(std::chrono::seconds(4));
            }
            if (kind != AudioBindingKind::PlaybackEndpoint || role != AudioEndpointRole::Communications)
                *unexpected_role = true;
            return endpoint ? std::string("endpoint-") + endpoint : std::string{};
        });
    ASSERT_NE(graph, nullptr) << error;
    const auto original = graph->PhysicalMeters();
    ASSERT_EQ(original.size(), 1u);
    EXPECT_EQ(original.front().reader_count, 2u);
    EXPECT_EQ(registry->Statistics().capture_opens, 1u);
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 2u);
    for (auto& track : tracks)
        ASSERT_TRUE(track->Init(error)) << error;
    std::optional<ScopedRecoveryLogBarrier> recovery_log;
    if (blocked_delivery)
        recovery_log.emplace(blocked_delivery);
    auto wait_until = [](auto ready) {
        const auto deadline = NowNs() + 1500000000ULL;
        while (!ready() && NowNs() < deadline)
            Sleep(1);
        return ready();
    };
    if (losses_before_start) {
        EXPECT_TRUE(wait_until([&] { return graph->PhysicalMeters().front().input_blocks != 0; }));
        control->block_reinit = true;
        control->phase = 1;
        EXPECT_TRUE(wait_until([&] { return control->reinit_entered.load(); }));
        const auto before = graph->PhysicalMeters().front().input_blocks;
        control->phase = 2;
        control->block_reinit = false;
        EXPECT_TRUE(wait_until([&] { return graph->PhysicalMeters().front().input_blocks >= before + 3; }));
        control->reinit_entered = false;
        control->block_reinit = true;
        control->phase = 1;
        EXPECT_TRUE(wait_until([&] { return control->reinit_entered.load(); }));
        EXPECT_EQ(lookup_calls->load(), 0u);
    }
    graph->Start();

    bool stale_endpoint_played = false;
    bool fixed_recovery_played = false;
    auto wait_for_peak = [&](size_t index, float peak) {
        const uint64_t deadline = NowNs() + 1500000000ULL;
        while (NowNs() < deadline) {
            for (size_t output = 0; output < tracks.size(); ++output) {
                auto& track = tracks[output];
                while (track->PendingFrameCount()) {
                    RawAudioBuffer raw;
                    if (!track->AcquireBuffer(raw, error))
                        return false;
                    if ((queued_recovery || losses_before_start || stale_same_endpoint) && raw.bytes) {
                        const auto* pcm = reinterpret_cast<const float*>(raw.bytes);
                        const auto* end = pcm + raw.num_frames * track->Channels();
                        const bool fixed_samples = std::find(pcm, end, 0.75f) != end;
                        stale_endpoint_played |= output == 0 && fixed_samples;
                        fixed_recovery_played |= output == 1 && fixed_samples;
                    }
                    track->ReleaseBuffer();
                }
            }
            const auto meters = graph->TrackMeters();
            stale_endpoint_played |= (blocked_open || blocked_confirmation) && meters[0].pre_limiter_peak == 0.5f;
            if (meters[index].pre_limiter_peak == peak)
                return true;
            Sleep(1);
        }
        return false;
    };
    if (!losses_before_start) {
        EXPECT_TRUE(wait_for_peak(0, 0.25f)) << error;
        EXPECT_EQ(lookup_calls->load(), 0u);
        control->phase = 1;
    }
    if (blocked_lookup) {
        EXPECT_TRUE(blocked_lookup->entered.try_acquire_for(std::chrono::milliseconds(1500)));
        *role_endpoint = 'B';
        control->phase = 2;
        EXPECT_TRUE(wait_for_peak(1, 0.75f)) << error;
        blocked_lookup->finish.release();
    }
    if (scenario == DefaultRecoveryScenario::Missing || scenario == DefaultRecoveryScenario::PointsToLostDevice) {
        const uint64_t deadline = NowNs() + 1500000000ULL;
        while (!lookup_calls->load() && NowNs() < deadline)
            Sleep(1);
        EXPECT_GT(lookup_calls->load(), 0u);
    }
    if (scenario == DefaultRecoveryScenario::Missing) {
        control->phase = 2;
        EXPECT_TRUE(wait_for_peak(1, 0.75f)) << error;
        EXPECT_EQ(graph->TrackMeters()[0].pre_limiter_peak, 0.f);
        EXPECT_EQ(graph->PhysicalMeters().size(), 1u);
        *role_endpoint = 'B';
    }
    if (scenario == DefaultRecoveryScenario::PointsToLostDevice)
        *role_endpoint = 'C';
    if (blocked_open) {
        EXPECT_TRUE(blocked_open->entered.try_acquire_for(std::chrono::milliseconds(1500)));
        *role_endpoint = 'C';
        blocked_open->finish.release();
    }
    if (blocked_confirmation) {
        EXPECT_TRUE(blocked_confirmation->entered.try_acquire_for(std::chrono::milliseconds(1500)));
        *role_endpoint = 'C';
        blocked_confirmation->finish.release();
    }
    if (blocked_stop) {
        EXPECT_TRUE(blocked_stop->entered.try_acquire_for(std::chrono::milliseconds(1500)));
        control->phase = 2;
        graph->RequestStop(NowNs());
        const bool drained = graph->WaitDrained(2000, error);
        blocked_stop->finish.release();
        EXPECT_TRUE(drained) << error;
        if (!drained)
            EXPECT_TRUE(graph->WaitDrained(3000, error)) << error;
        return;
    }
    if (blocked_delivery) {
        // Let the fixed capture queue recovered PCM and lose the device again before graph delivery resumes.
        EXPECT_TRUE(blocked_delivery->entered.try_acquire_for(std::chrono::milliseconds(1500)));
        const auto before = graph->PhysicalMeters().front().input_blocks;
        control->phase = 2;
        const auto deadline = NowNs() + 1500000000ULL;
        while (graph->PhysicalMeters().front().input_blocks < before + 3 && NowNs() < deadline)
            Sleep(1);
        EXPECT_GE(graph->PhysicalMeters().front().input_blocks, before + 3);
        control->reinit_entered = false;
        control->block_reinit = true;
        control->phase = 1;
        const auto loss_deadline = NowNs() + 1500000000ULL;
        while (!control->reinit_entered && NowNs() < loss_deadline)
            Sleep(1);
        EXPECT_TRUE(control->reinit_entered);
        *role_endpoint = 'B';
        blocked_delivery->finish.release();
    }
    const float current_peak = scenario == DefaultRecoveryScenario::ChangesWhileOpening ||
                                       scenario == DefaultRecoveryScenario::ChangesBeforeAcceptance ||
                                       scenario == DefaultRecoveryScenario::PointsToLostDevice
                                   ? 0.625f
                                   : 0.5f;
    const bool recovered_on_current_endpoint = wait_for_peak(0, current_peak);
    EXPECT_TRUE(recovered_on_current_endpoint) << error;
    EXPECT_GT(lookup_calls->load(), 0u);
    EXPECT_FALSE(unexpected_role->load());
    const auto split = graph->SourceMeters();
    for (const auto& meter : split) {
        if (meter.source_id == plan.sources[0].spec.id && recovered_on_current_endpoint)
            EXPECT_NE(meter.clock_id, original.front().clock_id);
        if (meter.source_id == plan.sources[1].spec.id)
            EXPECT_EQ(meter.clock_id, original.front().clock_id);
    }
    if (queued_recovery || losses_before_start || stale_same_endpoint)
        EXPECT_TRUE(fixed_recovery_played);
    control->phase = 2;
    control->block_reinit = false;
    EXPECT_TRUE(wait_for_peak(1, 0.75f)) << error;
    if (recovered_on_current_endpoint)
        EXPECT_TRUE(wait_for_peak(0, current_peak)) << error;
    EXPECT_FALSE(stale_endpoint_played);
    graph->RequestStop(NowNs());
    EXPECT_TRUE(graph->WaitDrained(3000, error)) << error;
}

TEST(AudioRuntimeGraph, LostSharedEndpointMovesOnlyTheRoleDefaultSource) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::Immediate);
}

TEST(AudioRuntimeGraph, MissingRoleDoesNotUseTheRecoveredFixedEndpoint) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::Missing);
}

TEST(AudioRuntimeGraph, ReopenedEndpointMustStillBeTheRoleDefault) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::ChangesWhileOpening);
}

TEST(AudioRuntimeGraph, ReopenedEndpointMustStillBeDefaultWhenAccepted) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::ChangesBeforeAcceptance);
}

TEST(AudioRuntimeGraph, StopDrainsWhileFinalRoleConfirmationIsStalled) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::StopsDuringConfirmation);
}

TEST(AudioRuntimeGraph, LostEndpointRemainingDefaultDoesNotEndRecovery) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::PointsToLostDevice);
}

TEST(AudioRuntimeGraph, RepeatedLossDoesNotAuthorizeQueuedFixedRecoverySamples) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::QueuedRecovery);
}

TEST(AudioRuntimeGraph, RecoveredSameEndpointRequiresAFreshRoleLookup) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::StaleSameEndpoint);
}

TEST(AudioRuntimeGraph, TwoLossesBeforeFirstGraphPollSuppressQueuedRecoveredPcm) {
    VerifyDefaultEndpointRecovery(DefaultRecoveryScenario::LossesBeforeStart);
}

ResolvedAudioPlan SingleDefaultEndpointPlan() {
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.spec.binding.follow_default = true;
    source.capture_key = PhysicalCaptureKey{};
    source.capture_key->endpoint_id = "A";
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.fixed_average_divisor = 1;
    track.limiter.enabled = false;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
    plan.tracks.push_back(track);
    return plan;
}

SharedAudioCaptureRequest EndpointRequest(std::string id) {
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = std::move(id);
    request.admission.sample_rate = 48000;
    request.admission.channels = 2;
    request.admission.maximum_packet_frames = 480;
    request.correction_enabled = false;
    return request;
}

TEST(AudioRuntimeGraph, AttachingRecoveredFixedOwnerRequiresRoleValidationBeforePcm) {
    const auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    const auto plan = SingleDefaultEndpointPlan();
    auto control = std::make_shared<OutageControl>();
    control->recovery_offset_ns = 0;
    auto factory = [control](const ResolvedAudioSource& source) {
        auto request = EndpointRequest(source.capture_key->endpoint_id);
        const bool fixed = request.key.endpoint_id == "A";
        request.native_factory = [control, fixed] { return std::make_unique<OutageCapture>(NowNs(), fixed, control); };
        return request;
    };
    std::string error;
    auto fixed = registry->AttachReaders({factory(plan.sources.front())}, NowNs(), error);
    ASSERT_EQ(fixed.size(), 1u) << error;
    control->block_reinit = true;
    control->phase = 1;
    const auto deadline = NowNs() + 1500000000ULL;
    while (!control->reinit_entered && NowNs() < deadline)
        Sleep(1);
    EXPECT_TRUE(control->reinit_entered);
    control->phase = 2;
    control->block_reinit = false;
    const auto recovery_deadline = NowNs() + 1500000000ULL;
    while (SharedAudioReaderRecoveryState(*fixed.front()).healthy_generation != 2 && NowNs() < recovery_deadline)
        Sleep(1);
    ASSERT_EQ(SharedAudioReaderRecoveryState(*fixed.front()).healthy_generation, 2u);

    auto barrier = std::make_shared<BlockedEndpointOpen>();
    auto role = std::make_shared<std::atomic<char>>('A');
    auto graph = AudioRuntimeGraph::Prepare(plan, registry, NowNs(), 48000, 2, false, error, factory,
                                            [barrier, role, first = true](AudioBindingKind, AudioEndpointRole) mutable {
                                                if (role->load() == 'A' && first) {
                                                    first = false;
                                                    barrier->entered.release();
                                                    (void)barrier->finish.try_acquire_for(std::chrono::seconds(4));
                                                }
                                                return std::string(1, role->load());
                                            });
    ASSERT_NE(graph, nullptr) << error;
    auto tracks = graph->TakeTracks();
    ASSERT_TRUE(tracks.front()->Init(error)) << error;
    graph->Start();
    EXPECT_TRUE(barrier->entered.try_acquire_for(std::chrono::milliseconds(1500)));
    auto read_until = [&](IAudioCaptureSource& source, auto inspect) {
        const auto limit = NowNs() + 1500000000ULL;
        while (NowNs() < limit) {
            if (source.PendingFrameCount()) {
                RawAudioBuffer raw;
                if (!source.AcquireBuffer(raw, error))
                    return false;
                const auto* samples = reinterpret_cast<const float*>(raw.bytes);
                std::span<const float> pcm;
                if (samples)
                    pcm = {samples, static_cast<size_t>(raw.num_frames) * source.Channels()};
                const bool done = inspect(pcm);
                source.ReleaseBuffer();
                if (done)
                    return true;
            } else {
                Sleep(1);
            }
        }
        return false;
    };
    unsigned checked = 0;
    EXPECT_TRUE(read_until(*tracks.front(), [&](auto pcm) {
        EXPECT_TRUE(std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.f; }));
        return ++checked == 3;
    })) << error;
    auto contains = [](float sample) {
        return [sample](auto pcm) { return std::find(pcm.begin(), pcm.end(), sample) != pcm.end(); };
    };
    EXPECT_TRUE(read_until(*fixed.front(), contains(0.75f))) << error;
    *role = 'B';
    barrier->finish.release();
    EXPECT_TRUE(read_until(*tracks.front(), contains(0.5f))) << error;
    graph->RequestStop(NowNs());
    EXPECT_TRUE(graph->WaitDrained(3000, error)) << error;
}

TEST(AudioRuntimeGraph, StoppedQueuedReopenSkipsNativeActivationAndReleasesFreshAdmission) {
    const auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    const auto plan = SingleDefaultEndpointPlan();
    auto control = std::make_shared<OutageControl>();
    auto preparing = std::make_shared<BlockedEndpointOpen>();
    auto obsolete_opens = std::make_shared<std::atomic_uint>(0);
    auto worker_lifetime = std::make_shared<int>(0);
    std::weak_ptr<int> worker = worker_lifetime;
    const auto start = NowNs();
    std::string error;
    auto graph = AudioRuntimeGraph::Prepare(
        plan, registry, start, 48000, 2, false, error,
        [control, preparing, obsolete_opens, start, worker_lifetime](const ResolvedAudioSource& source) {
            auto request = EndpointRequest(source.capture_key->endpoint_id);
            const bool obsolete = request.key.endpoint_id == "B";
            if (obsolete)
                preparing->entered.release();
            request.native_factory = [control, obsolete, obsolete_opens, start] {
                if (obsolete)
                    ++*obsolete_opens;
                return std::make_unique<OutageCapture>(start, !obsolete, control);
            };
            return request;
        },
        [](AudioBindingKind, AudioEndpointRole) { return std::string("B"); });
    ASSERT_NE(graph, nullptr) << error;
    worker_lifetime.reset();
    auto occupied = std::make_shared<BlockedEndpointOpen>();
    auto transaction = std::async(std::launch::async, [registry, occupied] {
        std::string detail;
        registry->AttachReaders({}, 0, detail, [&](auto, auto&) {
            occupied->entered.release();
            return occupied->finish.try_acquire_for(std::chrono::seconds(4));
        });
    });
    ASSERT_TRUE(occupied->entered.try_acquire_for(std::chrono::milliseconds(1500)));
    graph->Start();
    control->phase = 1;
    EXPECT_TRUE(preparing->entered.try_acquire_for(std::chrono::milliseconds(1500)));
    graph->RequestStop(NowNs());
    EXPECT_TRUE(graph->WaitDrained(3000, error)) << error;
    graph.reset();
    const auto deadline = NowNs() + 1500000000ULL;
    while (!worker.expired() && NowNs() < deadline)
        Sleep(1);
    EXPECT_TRUE(worker.expired());
    std::binary_semaphore fresh_entered{0};
    auto fresh_admission = std::async(std::launch::async, [&] {
        auto fresh = EndpointRequest("fresh");
        fresh.native_factory = [control] { return std::make_unique<OutageCapture>(NowNs(), false, control); };
        std::string detail;
        fresh_entered.release();
        auto readers = registry->AttachReaders({fresh}, NowNs(), detail);
        return std::pair(readers.size(), detail);
    });
    EXPECT_TRUE(fresh_entered.try_acquire_for(std::chrono::milliseconds(1500)));
    EXPECT_EQ(fresh_admission.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    occupied->finish.release();
    transaction.get();
    EXPECT_EQ(fresh_admission.wait_for(std::chrono::milliseconds(1500)), std::future_status::ready);
    const auto [reader_count, detail] = fresh_admission.get();
    EXPECT_EQ(reader_count, 1u) << detail;
    EXPECT_EQ(obsolete_opens->load(), 0u);
}

TEST(AudioRuntimeGraph, ExclusiveDefaultHandoffAtPhysicalOwnerLimit) {
    const auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    auto control = std::make_shared<OutageControl>();
    std::vector<SharedAudioCaptureRequest> peers;
    for (uint32_t i = 1; i < kMaxPhysicalAudioCaptures; ++i) {
        auto request = EndpointRequest("peer-" + std::to_string(i));
        request.native_factory = [control] { return std::make_unique<OutageCapture>(NowNs(), false, control); };
        peers.push_back(std::move(request));
    }
    std::string error;
    auto readers = registry->AttachReaders(peers, NowNs(), error);
    ASSERT_EQ(readers.size(), peers.size()) << error;
    const auto plan = SingleDefaultEndpointPlan();
    const auto start = NowNs();
    auto graph = AudioRuntimeGraph::Prepare(
        plan, registry, start, 48000, 2, false, error,
        [control, start](const ResolvedAudioSource& source) {
            auto request = EndpointRequest(source.capture_key->endpoint_id);
            const bool original = request.key.endpoint_id == "A";
            request.native_factory = [control, start, original] {
                return std::make_unique<OutageCapture>(original ? start : NowNs(), original, control);
            };
            return request;
        },
        [](AudioBindingKind, AudioEndpointRole) { return std::string("B"); });
    ASSERT_NE(graph, nullptr) << error;
    ASSERT_EQ(registry->MeterSnapshots().size(), kMaxPhysicalAudioCaptures);
    auto tracks = graph->TakeTracks();
    ASSERT_TRUE(tracks.front()->Init(error)) << error;
    graph->Start();
    control->phase = 1;
    bool heard_replacement = false;
    const auto deadline = NowNs() + 1500000000ULL;
    while (!heard_replacement && NowNs() < deadline) {
        if (tracks.front()->PendingFrameCount()) {
            RawAudioBuffer raw;
            if (tracks.front()->AcquireBuffer(raw, error)) {
                const auto* samples = reinterpret_cast<const float*>(raw.bytes);
                heard_replacement |= samples && std::any_of(samples, samples + raw.num_frames * 2,
                                                            [](float value) { return value == 0.5f; });
                tracks.front()->ReleaseBuffer();
            }
        }
        Sleep(1);
    }
    EXPECT_TRUE(heard_replacement) << error;
    EXPECT_EQ(control->peak_captures.load(), kMaxPhysicalAudioCaptures);
    EXPECT_EQ(registry->MeterSnapshots().size(), kMaxPhysicalAudioCaptures);
    graph->RequestStop(NowNs());
    EXPECT_TRUE(graph->WaitDrained(3000, error)) << error;
}

class AudioGraphOutage : public ::testing::Test {
  protected:
    std::shared_ptr<OutageControl> control = std::make_shared<OutageControl>();
    std::shared_ptr<SharedAudioCaptureRegistry> registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::shared_ptr<AudioRuntimeGraph> graph;
    std::vector<std::unique_ptr<IAudioCaptureSource>> tracks;
    std::vector<float> pcm[3];
    bool saw_discontinuity[3]{};
    uint64_t start = 0;
    std::string error;

    bool Start(bool denoise = false) {
        start = NowNs();
        ResolvedAudioPlan plan;
        for (uint32_t i = 0; i < 2; ++i) {
            ResolvedAudioSource source;
            source.spec.id = NewId<AudioSourceId>();
            source.spec.binding.kind = i ? AudioBindingKind::MicrophoneEndpoint : AudioBindingKind::PlaybackEndpoint;
            source.spec.dsp.rnnoise_enabled = i && denoise;
            source.capture_key = PhysicalCaptureKey{};
            source.capture_key->endpoint_id = i ? "failed-source" : "healthy-source";
            plan.sources.push_back(source);
        }
        for (uint32_t i = 0; i < 3; ++i) {
            ResolvedRoutingTrack track;
            track.id = NewId<AudioTrackId>();
            track.mux_index = {i};
            track.fixed_average_divisor = 2;
            track.limiter.enabled = false;
            for (const auto& source : plan.sources)
                track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
            plan.tracks.push_back(track);
        }
        graph = AudioRuntimeGraph::Prepare(plan, registry, start, 48000, 2, false, error,
                                           [this](const ResolvedAudioSource& source) {
                                               SharedAudioCaptureRequest request;
                                               request.key = *source.capture_key;
                                               request.admission.sample_rate = 48000;
                                               request.admission.channels = 2;
                                               request.admission.maximum_packet_frames = 480;
                                               const bool fails = request.key.endpoint_id == "failed-source";
                                               request.native_factory = [this, fails] {
                                                   return std::make_unique<OutageCapture>(start, fails, control);
                                               };
                                               return request;
                                           });
        if (!graph)
            return false;
        tracks = graph->TakeTracks();
        for (auto& track : tracks)
            if (!track->Init(error))
                return false;
        graph->Start();
        return true;
    }
    bool Consume(size_t index) {
        while (tracks[index]->PendingFrameCount()) {
            RawAudioBuffer raw;
            if (!tracks[index]->AcquireBuffer(raw, error))
                return false;
            const auto* samples = reinterpret_cast<const float*>(raw.bytes);
            pcm[index].insert(pcm[index].end(), samples, samples + static_cast<size_t>(raw.num_frames) * 2);
            saw_discontinuity[index] |= raw.data_discontinuity;
            tracks[index]->ReleaseBuffer();
        }
        return true;
    }
    bool RunFor(uint32_t milliseconds) {
        const uint64_t end = NowNs() + static_cast<uint64_t>(milliseconds) * 1000000ULL;
        while (NowNs() < end) {
            if (!Consume(0) || !Consume(1))
                return false;
            Sleep(1);
        }
        return true;
    }
    void TearDown() override {
        control->phase = 2;
        control->block_reinit = false;
        if (graph) {
            graph->RequestStop(NowNs());
            std::string ignored;
            graph->WaitDrained(2000, ignored);
        }
        for (auto& track : tracks)
            track->Shutdown();
        tracks.clear();
        graph.reset();
    }
};

TEST_F(AudioGraphOutage, ActivePeerSurvivesOutageRecoveryAndSlowConsumerOverflow) {
    ASSERT_TRUE(Start()) << error;
    ASSERT_TRUE(RunFor(80)) << error;
    control->phase = 1;
    ASSERT_TRUE(RunFor(3000)) << error;
    ASSERT_GT(pcm[0].size(), 250000u);
    EXPECT_TRUE(std::all_of(pcm[0].end() - 48000, pcm[0].end(), [](float value) { return value == 0.25f; }));
    EXPECT_TRUE(control->reinit_entered);
    control->phase = 2;
    const uint64_t deadline = NowNs() + 4000000000ULL;
    while ((pcm[0].empty() || pcm[0].back() != 0.625f) && NowNs() < deadline)
        ASSERT_TRUE(RunFor(20)) << error;
    ASSERT_FALSE(pcm[0].empty());
    EXPECT_FLOAT_EQ(pcm[0].back(), 0.625f);
    const uint64_t end = NowNs();
    graph->RequestStop(end);
    ASSERT_TRUE(graph->WaitDrained(1500, error)) << error;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto* track = dynamic_cast<AudioRuntimeTrackSource*>(tracks[i].get());
        ASSERT_NE(track, nullptr);
        ASSERT_TRUE(track->BeginDrain(end, error)) << error;
        ASSERT_TRUE(Consume(i)) << error;
    }
    EXPECT_EQ(pcm[0], pcm[1]);
    EXPECT_EQ(pcm[0].size(), pcm[2].size());
    EXPECT_TRUE(saw_discontinuity[2]);
    const auto meters = graph->SourceMeters();
    EXPECT_TRUE(
        std::any_of(meters.begin(), meters.end(), [](const auto& meter) { return meter.reader_overflow_frames > 0; }));
    EXPECT_TRUE(
        std::any_of(meters.begin(), meters.end(), [](const auto& meter) { return meter.missing_frames > 120000; }));
}

TEST_F(AudioGraphOutage, BlockedRecoveryStillPublishesSharedSilenceAndStopsWithDspTail) {
    ASSERT_TRUE(Start(true)) << error;
    ASSERT_TRUE(RunFor(80)) << error;
    control->block_reinit = true;
    control->phase = 1;
    ASSERT_TRUE(RunFor(500)) << error;
    EXPECT_TRUE(control->reinit_entered);
    ASSERT_GT(pcm[0].size(), 40000u);
    EXPECT_TRUE(std::all_of(pcm[0].end() - 9600, pcm[0].end(), [](float value) { return value == 0.25f; }));
    const uint64_t end = NowNs();
    graph->RequestStop(end);
    ASSERT_TRUE(graph->WaitDrained(1500, error)) << error;
    for (size_t i = 0; i < 2; ++i) {
        auto* track = dynamic_cast<AudioRuntimeTrackSource*>(tracks[i].get());
        ASSERT_NE(track, nullptr);
        ASSERT_TRUE(track->BeginDrain(end, error)) << error;
        ASSERT_TRUE(Consume(i)) << error;
    }
    EXPECT_EQ(pcm[0], pcm[1]);
    tracks.clear();
    graph.reset();
    EXPECT_GT(registry->ReservedPayloadBytes(), 0u);
    control->phase = 2;
    control->block_reinit = false;
    for (uint32_t wait = 0; wait < 2000 && registry->ReservedPayloadBytes(); ++wait)
        Sleep(1);
    EXPECT_EQ(registry->ReservedPayloadBytes(), 0u);
}

TEST_F(AudioGraphOutage, RecoveryCountsTheGapAfterTheLastSyntheticPacketOnce) {
    control->recovery_offset_ns = 200000000;
    ASSERT_TRUE(Start()) << error;
    ASSERT_TRUE(RunFor(80)) << error;
    control->phase = 1;
    ASSERT_TRUE(RunFor(500)) << error;
    control->phase = 2;
    const uint64_t deadline = NowNs() + 4000000000ULL;
    while ((pcm[0].empty() || pcm[0].back() != 0.625f) && NowNs() < deadline)
        ASSERT_TRUE(RunFor(20)) << error;
    ASSERT_FALSE(pcm[0].empty());
    ASSERT_FLOAT_EQ(pcm[0].back(), 0.625f);
    const uint64_t elapsed = control->recovery_qpc - control->last_native_end;
    const uint64_t expected = elapsed / 1000000000ULL * 48000 + elapsed % 1000000000ULL * 48000 / 1000000000ULL;
    const auto meters = graph->SourceMeters();
    const auto failed =
        std::find_if(meters.begin(), meters.end(), [](const auto& meter) { return meter.missing_frames > 0; });
    ASSERT_NE(failed, meters.end());
    EXPECT_GE(failed->missing_frames, expected);
    EXPECT_LE(failed->missing_frames, expected + 1);
    const uint64_t counted = failed->missing_frames;
    ASSERT_TRUE(RunFor(100)) << error;
    const auto later = graph->SourceMeters();
    const auto same = std::find_if(later.begin(), later.end(),
                                   [&](const auto& meter) { return meter.source_id == failed->source_id; });
    ASSERT_NE(same, later.end());
    EXPECT_EQ(same->missing_frames, counted);
}

TEST(AudioRuntimeGraph, SharedCaptureProcessesEachSourceOnceAndFansOutToTwoTracks) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    plan.revision = {1};
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    source.spec.post_dsp_gain_linear = 0.5f;
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
    const auto physical = graph->PhysicalMeters();
    ASSERT_EQ(physical.size(), 1u);
    EXPECT_EQ(physical.front().capture_opens, 1u);
    EXPECT_EQ(physical.front().reader_count, 1u);
    EXPECT_EQ(physical.front().input_frames, 9600u);
    EXPECT_GT(registry->ReservedPayloadBytes(), 0u);
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
        ASSERT_EQ(samples.size(), 9600u * 2);
        EXPECT_NEAR(samples[960], 0.125f, 0.0001f);
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

TEST(AudioRuntimeGraph, DelayedSharedProducerPreservesPcmAcrossUnequalTrackSchedules) {
    // Put both packets behind the wall-clock playback horizon. Only the producer's frontier should withhold them.
    const uint64_t start = NowNs() - 100000000ULL;
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    for (uint32_t i = 0; i < 2; ++i) {
        ResolvedRoutingTrack track;
        track.id = NewId<AudioTrackId>();
        track.mux_index = {i};
        track.fixed_average_divisor = 1;
        track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
        plan.tracks.push_back(track);
    }
    auto opens = std::make_shared<std::atomic<uint32_t>>(0);
    auto gate = std::make_shared<FiniteCapturePacketGate>();
    auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    const auto make_request = MakeGatedFiniteCaptureRequest(opens, start, false, gate);
    auto graph = AudioRuntimeGraph::Prepare(plan, registry, start, 48000, 2, false, error, make_request);
    ASSERT_NE(graph, nullptr) << error;
    const auto observer = registry->AttachReaders({make_request(source)}, start, error);
    ASSERT_EQ(observer.size(), 1u) << error;
    ASSERT_EQ(opens->load(), 1u);
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 2u);
    for (auto& track : tracks)
        ASSERT_TRUE(track->Init(error)) << error;
    graph->Start();
    const StopGraphOnExit stop{*graph};
    std::vector<float> pcm[2];
    auto consume = [&](size_t index) {
        RawAudioBuffer raw;
        if (!tracks[index]->AcquireBuffer(raw, error))
            return false;
        const auto* samples = reinterpret_cast<const float*>(raw.bytes);
        pcm[index].insert(pcm[index].end(), samples, samples + static_cast<size_t>(raw.num_frames) * 2);
        tracks[index]->ReleaseBuffer();
        return true;
    };
    ASSERT_TRUE(WaitForGraphCondition([&] { return tracks[0]->PendingFrameCount() == 480; }));
    ASSERT_TRUE(consume(0)) << error;
    ASSERT_EQ(pcm[0].size(), 960u);
    const auto empty_polls = gate->empty_polls.load();
    // The second subsequent empty poll proves the owner processed the first while packet two was held.
    ASSERT_TRUE(WaitForGraphCondition([&] { return gate->empty_polls.load() >= empty_polls + 2; }));
    EXPECT_EQ(SharedAudioReaderCompletedThrough(*observer.front()), start + 10000000ULL);
    const auto premature_frames = tracks[0]->PendingFrameCount();
    EXPECT_EQ(premature_frames, 0u);
    if (premature_frames)
        ASSERT_TRUE(consume(0)) << error;
    gate->released.store(true);
    ASSERT_TRUE(WaitForGraphCondition([&] { return graph->SourceMeters().front().processed_frames == 960; }));
    const uint64_t end = start + 20000000ULL;
    graph->RequestStop(end);
    ASSERT_TRUE(graph->WaitDrained(1500, error)) << error;
    for (size_t i = 0; i < 2; ++i) {
        auto* output = dynamic_cast<AudioRuntimeTrackSource*>(tracks[i].get());
        ASSERT_NE(output, nullptr);
        ASSERT_TRUE(output->BeginDrain(end, error)) << error;
        while (output->PendingFrameCount())
            ASSERT_TRUE(consume(i)) << error;
    }
    EXPECT_EQ(pcm[0], pcm[1]);
    ASSERT_EQ(pcm[0].size(), 1920u);
    EXPECT_TRUE(std::all_of(pcm[0].begin(), pcm[0].end(), [](float value) { return value == 0.25f; }));
}
TEST(AudioRuntimeGraph, NativePacketBeforeCertifiedIdleFrontierFailsGraph) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    for (uint32_t i = 0; i < 2; ++i) {
        ResolvedRoutingTrack track;
        track.id = NewId<AudioTrackId>();
        track.mux_index = {i};
        track.fixed_average_divisor = 1;
        track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
        plan.tracks.push_back(track);
    }
    auto opens = std::make_shared<std::atomic<uint32_t>>(0);
    auto gate = std::make_shared<FiniteCapturePacketGate>();
    auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    const auto make_request = MakeGatedFiniteCaptureRequest(opens, start, true, gate);
    auto graph = AudioRuntimeGraph::Prepare(plan, registry, start, 48000, 2, false, error, make_request);
    ASSERT_NE(graph, nullptr) << error;
    // Observe the same physical owner directly. A pending track buffer can still be first-packet PCM.
    const auto observer = registry->AttachReaders({make_request(source)}, start, error);
    ASSERT_EQ(observer.size(), 1u) << error;
    ASSERT_EQ(opens->load(), 1u);
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 2u);
    for (auto& track : tracks)
        ASSERT_TRUE(track->Init(error)) << error;
    graph->Start();
    const StopGraphOnExit stop{*graph};
    for (auto& track : tracks) {
        ASSERT_TRUE(WaitForGraphCondition([&] { return track->PendingFrameCount() == 480; }));
        RawAudioBuffer raw;
        ASSERT_TRUE(track->AcquireBuffer(raw, error)) << error;
        EXPECT_FALSE(raw.silent);
        const auto frames = raw.num_frames;
        track->ReleaseBuffer();
        ASSERT_EQ(frames, 480u);
    }
    const uint64_t second_packet_qpc = start + 10000000ULL;
    ASSERT_TRUE(WaitForGraphCondition([&] {
        return SharedAudioReaderCompletedThrough(*observer.front()) > second_packet_qpc &&
               tracks[0]->PendingFrameCount() > 0;
    }));
    gate->released.store(true);
    EXPECT_FALSE(graph->WaitDrained(1500, error));
    EXPECT_EQ(error, "Physical audio packet precedes its certified idle frontier");
}
TEST(AudioRuntimeGraph, HealthyIdleCaptureSealsAndPadsTheFiniteTrackWithoutPackets) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.fixed_average_divisor = 1;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
    plan.tracks.push_back(track);
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
            request.native_factory = [opens, start] { return std::make_unique<FiniteCapture>(opens, start, 0); };
            return request;
        });
    ASSERT_NE(graph, nullptr) << error;
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 1u);
    ASSERT_TRUE(tracks[0]->Init(error)) << error;
    graph->Start();
    const uint64_t end = start + 20000000ULL;
    graph->RequestStop(end);
    ASSERT_TRUE(graph->WaitDrained(1500, error)) << error;
    auto* output = dynamic_cast<AudioRuntimeTrackSource*>(tracks[0].get());
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->BeginDrain(end, error)) << error;
    uint64_t frames = 0;
    while (output->PendingFrameCount()) {
        RawAudioBuffer raw;
        ASSERT_TRUE(output->AcquireBuffer(raw, error)) << error;
        frames += raw.num_frames;
        const auto* pcm = reinterpret_cast<const float*>(raw.bytes);
        EXPECT_TRUE(
            std::all_of(pcm, pcm + static_cast<size_t>(raw.num_frames) * 2, [](float value) { return value == 0.f; }));
        output->ReleaseBuffer();
    }
    EXPECT_EQ(frames, 960u);
    EXPECT_EQ(opens->load(), 1u);
}
TEST(AudioRuntimeGraph, UnknownCaptureLatencyCannotSealIdleSource) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.fixed_average_divisor = 1;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
    plan.tracks.push_back(track);
    auto opens = std::make_shared<std::atomic<uint32_t>>(0);
    auto registry = std::make_shared<SharedAudioCaptureRegistry>();
    std::string error;
    auto graph = AudioRuntimeGraph::Prepare(plan, registry, start, 48000, 2, false, error,
                                            [opens, start](const ResolvedAudioSource& resolved) {
                                                SharedAudioCaptureRequest request;
                                                request.key = *resolved.capture_key;
                                                request.admission.sample_rate = 48000;
                                                request.admission.channels = 2;
                                                request.admission.maximum_packet_frames = 480;
                                                request.native_factory = [opens, start] {
                                                    return std::make_unique<FiniteCapture>(opens, start, 0, 0, false);
                                                };
                                                return request;
                                            });
    ASSERT_NE(graph, nullptr) << error;
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 1u);
    ASSERT_TRUE(tracks[0]->Init(error)) << error;
    graph->Start();
    const uint64_t end = start + 20000000ULL;
    graph->RequestStop(end);
    EXPECT_FALSE(graph->WaitDrained(1500, error));
    EXPECT_EQ(error, "Physical audio reader could not seal its finite endpoint");
}

TEST(AudioRuntimeGraph, DelayedPacketSpanningStopKeepsItsPreStopSamples) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    ResolvedAudioSource source;
    source.spec.id = NewId<AudioSourceId>();
    source.capture_key = PhysicalCaptureKey{};
    plan.sources.push_back(source);
    ResolvedRoutingTrack track;
    track.id = NewId<AudioTrackId>();
    track.fixed_average_divisor = 1;
    track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, 1.f});
    plan.tracks.push_back(track);
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
            request.native_factory = [opens, start] {
                return std::make_unique<FiniteCapture>(opens, start, 1, 30000000ULL);
            };
            return request;
        });
    ASSERT_NE(graph, nullptr) << error;
    auto tracks = graph->TakeTracks();
    ASSERT_EQ(tracks.size(), 1u);
    ASSERT_TRUE(tracks[0]->Init(error)) << error;
    graph->Start();
    const uint64_t end = start + 5000000ULL;
    graph->RequestStop(end);
    ASSERT_TRUE(graph->WaitDrained(1500, error)) << error;
    auto* output = dynamic_cast<AudioRuntimeTrackSource*>(tracks[0].get());
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->BeginDrain(end, error)) << error;
    uint64_t frames = 0;
    while (output->PendingFrameCount()) {
        RawAudioBuffer raw;
        ASSERT_TRUE(output->AcquireBuffer(raw, error)) << error;
        frames += raw.num_frames;
        const auto* pcm = reinterpret_cast<const float*>(raw.bytes);
        EXPECT_TRUE(std::all_of(pcm, pcm + static_cast<size_t>(raw.num_frames) * 2,
                                [](float value) { return value == 0.25f; }));
        output->ReleaseBuffer();
    }
    EXPECT_EQ(frames, 240u);
    EXPECT_EQ(opens->load(), 1u);
}
TEST(AudioRuntimeGraph, MetersKeepPhysicalLogicalAndTrackOwnershipDistinct) {
    const uint64_t start = NowNs();
    ResolvedAudioPlan plan;
    plan.revision = {1};
    AudioSourceId source_ids[2];
    AudioTrackId track_ids[2];
    for (uint32_t i = 0; i < 2; ++i) {
        ResolvedAudioSource source;
        source.spec.id = NewId<AudioSourceId>();
        source.capture_key = PhysicalCaptureKey{};
        source_ids[i] = source.spec.id;
        plan.sources.push_back(source);
        ResolvedRoutingTrack track;
        track.id = NewId<AudioTrackId>();
        track.mux_index = {i};
        track.fixed_average_divisor = 1;
        track.limiter.enabled = i == 1;
        track.limiter.ceiling_db = -6.f;
        track.routes.push_back({NewId<AudioRouteId>(), source.spec.id, i == 1 ? 4.f : 1.f});
        track_ids[i] = track.id;
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
    const auto source_before = graph->SourceMeters();
    ASSERT_EQ(source_before.size(), 2u);
    EXPECT_FALSE(source_before[0].has_measurement);
    EXPECT_FALSE(source_before[0].processing_time_available);
    EXPECT_EQ(source_before[0].processing_ms, 0);
    const auto before = graph->TrackMeters();
    ASSERT_EQ(before.size(), 2u);
    EXPECT_FALSE(before[0].has_measurement);
    EXPECT_FALSE(before[0].processing_time_available);
    EXPECT_EQ(before[0].processing_ms, 0);
    graph->Start();
    for (uint32_t wait = 0; wait < 2000; ++wait) {
        const auto meters = graph->SourceMeters();
        const auto owners = graph->PhysicalMeters();
        if (meters.size() == 2 && meters[0].processed_frames == 9600 && meters[1].processed_frames == 9600 &&
            owners.size() == 1 && owners[0].input_blocks == 20)
            break;
        Sleep(1);
    }
    const auto logical = graph->SourceMeters();
    ASSERT_EQ(logical.size(), 2u);
    const auto physical = graph->PhysicalMeters();
    ASSERT_EQ(physical.size(), 1u);
    EXPECT_EQ(opens->load(), 1u);
    EXPECT_TRUE(physical[0].available);
    EXPECT_EQ(physical[0].clock_quality, AudioMeterClockQuality::Unknown);
    EXPECT_EQ(physical[0].reader_count, 2u);
    EXPECT_EQ(physical[0].capture_opens, 1u);
    EXPECT_EQ(physical[0].input_blocks, 20u);
    EXPECT_EQ(physical[0].input_frames, 9600u);
    EXPECT_EQ(physical[0].reinit_count, 0u);
    EXPECT_TRUE(physical[0].processing_time_available);
    EXPECT_GT(physical[0].processing_ms, 0);
    for (uint32_t i = 0; i < 2; ++i) {
        const auto source = std::find_if(logical.begin(), logical.end(),
                                         [&](const auto& meter) { return meter.source_id == source_ids[i]; });
        ASSERT_NE(source, logical.end());
        EXPECT_EQ(source->clock_id, physical[0].clock_id);
        EXPECT_EQ(source->reader_count, 1u);
        EXPECT_EQ(source->processed_blocks, 20u);
        EXPECT_EQ(source->processed_frames, 9600u);
        EXPECT_EQ(source->reader_overflow_frames, 0u);
        EXPECT_TRUE(source->has_measurement);
        EXPECT_GT(source->rms, 0);
        EXPECT_TRUE(source->processing_time_available);
        EXPECT_GT(source->processing_ms, 0);
    }
    EXPECT_FALSE(graph->TrackMeters()[0].has_measurement);
    graph->RequestStop(start + 200000000ULL);
    ASSERT_TRUE(graph->WaitDrained(3000, error)) << error;
    TrackMeterSnapshot active[2];
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto* track = dynamic_cast<AudioRuntimeTrackSource*>(tracks[i].get());
        ASSERT_NE(track, nullptr);
        ASSERT_TRUE(track->BeginDrain(start + 200000000ULL, error)) << error;
        while (track->PendingFrameCount()) {
            RawAudioBuffer raw;
            ASSERT_TRUE(track->AcquireBuffer(raw, error)) << error;
            const auto meters = graph->TrackMeters();
            if (meters[i].pre_limiter_peak > 0)
                active[i] = meters[i];
            track->ReleaseBuffer();
        }
    }
    EXPECT_EQ(active[0].track_id, track_ids[0]);
    EXPECT_EQ(active[1].track_id, track_ids[1]);
    EXPECT_TRUE(active[0].has_measurement);
    EXPECT_TRUE(active[1].has_measurement);
    EXPECT_NEAR(active[0].pre_limiter_peak, 0.25f, 0.001f);
    EXPECT_NEAR(active[0].post_limiter_peak, 0.25f, 0.001f);
    EXPECT_NEAR(active[0].rms, 0.25f, 0.001f);
    EXPECT_NEAR(active[0].limiter_gain, 1.f, 0.001f);
    EXPECT_FALSE(active[0].limiter_enabled);
    EXPECT_NEAR(active[1].pre_limiter_peak, 1.f, 0.001f);
    EXPECT_LE(active[1].post_limiter_peak, 0.502f);
    EXPECT_GT(active[1].post_limiter_peak, 0.f);
    EXPECT_TRUE(active[1].limiter_enabled);
    EXPECT_LT(active[1].limiter_gain, 1.f);
    EXPECT_TRUE(active[0].processing_time_available);
    EXPECT_TRUE(active[1].processing_time_available);
    EXPECT_GT(active[0].processing_ms, 0);
    EXPECT_GT(active[1].processing_ms, 0);
    const auto retained = graph->TrackMeters();
    tracks.clear();
    const auto after = graph->TrackMeters();
    ASSERT_EQ(after.size(), 2u);
    EXPECT_EQ(after[0].track_id, track_ids[0]);
    EXPECT_EQ(after[1].track_id, track_ids[1]);
    EXPECT_EQ(after[0].processing_ms, retained[0].processing_ms);
    EXPECT_EQ(after[1].processing_ms, retained[1].processing_ms);
}

TEST(AudioRuntimeGraph, LogicalMeterSeparatesReaderOverflowFromOtherMissingFrames) {
    ResolvedAudioSource resolved;
    resolved.spec.id = NewId<AudioSourceId>();
    AudioCaptureDescriptor descriptor;
    descriptor.sample_rate = 48000;
    descriptor.channels = 2;
    descriptor.maximum_packet_frames = 480;
    descriptor.clock_id = NewId<PhysicalClockId>();
    std::string error;
    auto source = LogicalAudioSource::Create(resolved, descriptor, error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_FALSE(source->Meter().processing_time_available);
    auto readers = source->AttachReaders(1, 0, error);
    ASSERT_EQ(readers.size(), 1u);
    std::vector<float> samples(960, 0.25f);
    AudioBlockView block;
    block.descriptor = descriptor;
    block.samples = samples;
    block.metadata.frames = 480;
    block.metadata.output_qpc_ns = 1000000000ULL;
    ASSERT_TRUE(source->Process(block, error)) << error;
    block.metadata.sequence = 1;
    block.metadata.output_qpc_ns += 10000000ULL;
    block.metadata.device_gap_frames = 120;
    block.reader_overflow_frames = 240;
    ASSERT_TRUE(source->Process(block, error)) << error;
    auto meter = source->Meter();
    EXPECT_EQ(meter.clock_id, descriptor.clock_id);
    EXPECT_EQ(meter.reader_count, 1u);
    EXPECT_EQ(meter.reader_overflow_frames, 240u);
    EXPECT_EQ(meter.missing_frames, 360u);
    EXPECT_EQ(meter.processed_frames, 1320u);
    EXPECT_TRUE(meter.processing_time_available);
    EXPECT_GT(meter.processing_ms, 0);
    readers.clear();
    EXPECT_EQ(source->Meter().reader_count, 0u);
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
