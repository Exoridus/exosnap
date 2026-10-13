#include "audio_tee.h"
#include "shared_audio_capture.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <span>
#include <utility>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace exosnap::engine;

TEST(SharedAudioCaptureBudget, TicketsShareCapacityAndReleaseAfterTheLastReference) {
    SharedAudioCaptureRegistry registry(128);
    std::string error;
    auto first = registry.ReservePayload(64, error);
    ASSERT_NE(first, nullptr) << error;
    EXPECT_EQ(registry.ReservedPayloadBytes(), 64u);
    auto held = first;
    first.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 64u);
    EXPECT_EQ(registry.ReservePayload(65, error), nullptr);
    EXPECT_NE(error.find("budget"), std::string::npos);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 64u);
    auto second = registry.ReservePayload(64, error);
    ASSERT_NE(second, nullptr) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(registry.ReservedPayloadBytes(), 128u);
    held.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 64u);
    second.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}

TEST(SharedAudioCaptureBudget, RuntimeCeilingRejectsNinthGroupUntilLastLeaseIsReleased) {
    SharedAudioCaptureRegistry registry;
    std::string error;
    std::vector<std::shared_ptr<void>> tickets;
    for (size_t group = 0; group < 8; ++group) {
        auto ticket = registry.ReservePayload(32u * 1024 * 1024, error);
        ASSERT_NE(ticket, nullptr) << error;
        tickets.push_back(std::move(ticket));
    }
    EXPECT_EQ(registry.ReservedPayloadBytes(), 256u * 1024 * 1024);
    EXPECT_EQ(registry.ReservePayload(1, error), nullptr);
    auto held = tickets.back();
    tickets.pop_back();
    EXPECT_EQ(registry.ReservePayload(1, error), nullptr);
    held.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 224u * 1024 * 1024);
    EXPECT_NE(registry.ReservePayload(32u * 1024 * 1024, error), nullptr);
    tickets.clear();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}

TEST(SharedAudioCaptureBudget, InvalidComponentAdmissionDoesNotReserveCapacity) {
    SharedAudioCaptureRegistry registry;
    std::string error;
    EXPECT_EQ(registry.ReservePayload(0, error), nullptr);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
    EXPECT_EQ(registry.ReservePayload(kAudioTeePayloadBytes + 1, error), nullptr);
    EXPECT_NE(error.find("budget"), std::string::npos);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
    auto admitted = registry.ReservePayload(kAudioTeePayloadBytes, error);
    ASSERT_NE(admitted, nullptr) << error;
    EXPECT_EQ(registry.ReservedPayloadBytes(), kAudioTeePayloadBytes);
    admitted.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}

TEST(SharedAudioCaptureBudget, ReservationTicketOutlivesTheRegistryAndReleasesSafely) {
    auto registry = std::make_unique<SharedAudioCaptureRegistry>();
    std::string error;
    auto ticket = registry->ReservePayload(4096, error);
    ASSERT_NE(ticket, nullptr) << error;
    std::weak_ptr<void> observer = ticket;
    registry.reset();
    EXPECT_FALSE(observer.expired());
    ticket.reset();
    EXPECT_TRUE(observer.expired());
}

struct NativeState {
    std::atomic<uint32_t> opens = 0;
    std::atomic<uint32_t> shutdowns = 0;
    std::thread::id owner_thread;
    std::atomic<bool> wrong_thread = false;
};

class VirtualNative final : public IAudioCaptureSource {
  public:
    VirtualNative(double ppm, uint32_t rate = 8000, uint32_t channels = 1,
                  std::shared_ptr<NativeState> state = std::make_shared<NativeState>())
        : ppm_(ppm), rate_(rate), channels_(channels), state_(std::move(state)), samples_((rate / 100) * channels) {
    }
    bool Init(std::string&) override {
        state_->owner_thread = std::this_thread::get_id();
        ++state_->opens;
        return true;
    }
    uint32_t PendingFrameCount() override {
        CheckThread();
        return rate_ / 100;
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string&) override {
        CheckThread();
        timing_.device_position_ns = DeviceFramesToNs(position_, rate_);
        timing_.device_position_frames = position_;
        timing_.device_position_frames_valid = true;
        timing_.qpc_position_ns =
            1'000'000'000ULL +
            static_cast<uint64_t>(std::llround(static_cast<double>(timing_.device_position_ns) * (1.0 + ppm_ * 1e-6)));
        const uint32_t frames = rate_ / 100;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const double time = static_cast<double>(position_ + frame) / rate_ * (1.0 + ppm_ * 1e-6);
            const float value = std::fmod(time, 300.0) < 0.004 ? 1.0f : 0.0f;
            for (uint32_t channel = 0; channel < channels_; ++channel)
                samples_[frame * channels_ + channel] = value;
        }
        buffer = {};
        buffer.bytes = reinterpret_cast<const uint8_t*>(samples_.data());
        buffer.num_frames = frames;
        position_ += frames;
        return true;
    }
    void ReleaseBuffer() override {
        CheckThread();
    }
    uint32_t SampleRate() const override {
        return rate_;
    }
    uint32_t Channels() const override {
        return channels_;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    bool LastBufferDeviceTiming(AudioDeviceTiming& timing) const override {
        timing = timing_;
        return true;
    }
    void Shutdown() override {
        CheckThread();
        ++state_->shutdowns;
    }

  private:
    void CheckThread() {
        if (state_->owner_thread != std::this_thread::get_id())
            state_->wrong_thread = true;
    }
    double ppm_;
    uint32_t rate_;
    uint32_t channels_;
    std::shared_ptr<NativeState> state_;
    std::vector<float> samples_;
    uint64_t position_ = 0;
    AudioDeviceTiming timing_;
    std::string name_ = "virtual native clock";
};

AudioCaptureDescriptor Admission(uint32_t rate = 8000, uint32_t channels = 1) {
    AudioCaptureDescriptor descriptor;
    descriptor.sample_rate = rate;
    descriptor.channels = channels;
    descriptor.maximum_packet_frames = rate / 4 + 256;
    return descriptor;
}

template <typename Predicate> bool WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

enum class StallStage { Init, Acquire, Shutdown };

struct NativeGate {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> entered = false;
    bool released = false;
    void Stall() {
        std::unique_lock lock(mutex);
        entered = true;
        condition.wait(lock, [this] { return released; });
    }
    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

struct GateRelease {
    std::shared_ptr<NativeGate> gate;
    ~GateRelease() {
        gate->Release();
    }
};

class StalledNative final : public IAudioCaptureSource {
  public:
    StalledNative(StallStage stage, std::shared_ptr<NativeGate> gate, std::shared_ptr<NativeState> state)
        : stage_(stage), gate_(std::move(gate)), native_(0, 8000, 1, std::move(state)) {
    }
    bool Init(std::string& error) override {
        if (stage_ == StallStage::Init)
            gate_->Stall();
        return native_.Init(error);
    }
    uint32_t PendingFrameCount() override {
        return native_.PendingFrameCount();
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override {
        if (stage_ == StallStage::Acquire)
            gate_->Stall();
        return native_.AcquireBuffer(buffer, error);
    }
    void ReleaseBuffer() override {
        native_.ReleaseBuffer();
    }
    uint32_t SampleRate() const override {
        return native_.SampleRate();
    }
    uint32_t Channels() const override {
        return native_.Channels();
    }
    AudioSampleFormat SampleFormat() const override {
        return native_.SampleFormat();
    }
    const std::string& EndpointName() const override {
        return native_.EndpointName();
    }
    bool LastBufferDeviceTiming(AudioDeviceTiming& timing) const override {
        return native_.LastBufferDeviceTiming(timing);
    }
    void Shutdown() override {
        if (stage_ == StallStage::Shutdown)
            gate_->Stall();
        native_.Shutdown();
    }

  private:
    StallStage stage_;
    std::shared_ptr<NativeGate> gate_;
    VirtualNative native_;
};

SharedAudioCaptureRequest StalledRequest(StallStage stage, const std::shared_ptr<NativeGate>& gate,
                                         const std::shared_ptr<NativeState>& state) {
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "gated native device";
    request.admission = Admission();
    request.preparation_timeout = std::chrono::milliseconds(50);
    request.native_factory = [stage, gate, state] { return std::make_unique<StalledNative>(stage, gate, state); };
    return request;
}

TEST(SharedAudioCapture, GraphAdmissionRejectsBeforeAnyNativeInit) {
    SharedAudioCaptureRegistry registry;
    std::vector<std::shared_ptr<NativeState>> states;
    std::vector<SharedAudioCaptureRequest> requests(2);
    for (uint32_t index = 0; index < 2; ++index) {
        auto state = std::make_shared<NativeState>();
        states.push_back(state);
        requests[index].key.endpoint_id = "preflight endpoint " + std::to_string(index);
        requests[index].admission = Admission();
        requests[index].native_factory = [state] { return std::make_unique<VirtualNative>(0, 8000, 1, state); };
    }
    uint32_t admissions = 0;
    std::string error;
    auto readers = registry.AttachReaders(
        requests, 0, error, [&](std::span<const AudioCaptureDescriptor> descriptors, std::string& reason) {
            ++admissions;
            EXPECT_EQ(descriptors.size(), 2u);
            for (const auto& descriptor : descriptors) {
                EXPECT_EQ(descriptor.sample_rate, 8000u);
                EXPECT_EQ(descriptor.channels, 1u);
                EXPECT_EQ(descriptor.sample_format, AudioSampleFormat::Float32);
                EXPECT_NE(descriptor.clock_id, PhysicalClockId{});
            }
            EXPECT_GT(registry.ReservedPayloadBytes(), 0u);
            reason = "Graph payload budget rejected";
            return false;
        });
    EXPECT_TRUE(readers.empty());
    EXPECT_EQ(admissions, 1u);
    EXPECT_EQ(error, "Graph payload budget rejected");
    for (const auto& state : states)
        EXPECT_EQ(state->opens.load(), 0u);
    readers.clear();
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

TEST(SharedAudioCapture, EmptyCaptureSetStillRunsAdmissionAndCanReserveSilentTrackPayload) {
    SharedAudioCaptureRegistry registry;
    uint32_t admissions = 0;
    std::shared_ptr<void> track_reservation;
    std::string error;
    auto readers = registry.AttachReaders(
        {}, 0, error, [&](std::span<const AudioCaptureDescriptor> descriptors, std::string& reason) {
            ++admissions;
            EXPECT_TRUE(descriptors.empty());
            track_reservation = registry.ReservePayload(4096, reason);
            return track_reservation != nullptr;
        });
    EXPECT_TRUE(readers.empty());
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(admissions, 1u);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 4096u);
    track_reservation.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}

TEST(SharedAudioCapture, AcceptedAdmissionReservesAdditionalPayloadBeforeInitOnTheSameOwnerThread) {
    auto state = std::make_shared<NativeState>();
    SharedAudioCaptureRegistry registry;
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "accepted preparation";
    request.admission = Admission();
    std::thread::id prepared_thread;
    request.describe_native = [&](AudioCaptureDescriptor&, std::string&) {
        prepared_thread = std::this_thread::get_id();
        return true;
    };
    request.native_factory = [state] { return std::make_unique<VirtualNative>(0, 8000, 1, state); };
    std::shared_ptr<void> logical_reservation;
    std::string error;
    auto readers = registry.AttachReaders(
        {request}, 0, error, [&](std::span<const AudioCaptureDescriptor> descriptors, std::string& reason) {
            EXPECT_EQ(descriptors.size(), 1u);
            EXPECT_EQ(state->opens.load(), 0u);
            logical_reservation = registry.ReservePayload(4096, reason);
            return logical_reservation != nullptr;
        });
    ASSERT_EQ(readers.size(), 1u) << error;
    EXPECT_EQ(state->opens.load(), 1u);
    EXPECT_EQ(state->owner_thread, prepared_thread);
    EXPECT_NE(prepared_thread, std::this_thread::get_id());
    readers.clear();
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 4096; }));
    logical_reservation.reset();
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}

TEST(SharedAudioCapture, NativeFormatChangeAfterGraphAdmissionRequiresReprepare) {
    SharedAudioCaptureRegistry registry;
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "changed format during preparation";
    request.admission = Admission(16000, 2);
    request.native_factory = [] { return std::make_unique<VirtualNative>(0, 8000, 1); };
    std::string error;
    EXPECT_TRUE(registry
                    .AttachReaders({request}, 0, error,
                                   [](std::span<const AudioCaptureDescriptor>, std::string&) { return true; })
                    .empty());
    EXPECT_NE(error.find("reprepare required"), std::string::npos);
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

TEST(SharedAudioCapture, BatchSealQueuesAllOwnersAndUsesOneDeadline) {
    auto first_gate = std::make_shared<NativeGate>();
    auto second_gate = std::make_shared<NativeGate>();
    GateRelease release_first{first_gate};
    GateRelease release_second{second_gate};
    auto first_state = std::make_shared<NativeState>();
    auto second_state = std::make_shared<NativeState>();
    SharedAudioCaptureRegistry registry;
    auto first = StalledRequest(StallStage::Acquire, first_gate, first_state);
    auto second = StalledRequest(StallStage::Acquire, second_gate, second_state);
    first.preparation_timeout = std::chrono::milliseconds(2000);
    second.preparation_timeout = std::chrono::milliseconds(2000);
    second.key.endpoint_id = "second gated device";
    std::string error;
    auto readers = registry.AttachReaders({first, second}, 0, error);
    ASSERT_EQ(readers.size(), 2u) << error;
    ASSERT_TRUE(WaitUntil([&] { return first_gate->entered && second_gate->entered; }));
    std::vector<IAudioCaptureSource*> pointers{readers[0].get(), readers[1].get()};
    const auto started = std::chrono::steady_clock::now();
    const auto ends = BatchSealAudioReadersEnd(pointers, 2'000'000'000ULL);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(750));
    ASSERT_EQ(ends.size(), 2u);
    for (const auto& end : ends) {
        EXPECT_TRUE(end.stalled);
        EXPECT_FALSE(end.undrained_frames.has_value());
    }
    readers.clear();
    first_gate->Release();
    second_gate->Release();
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

TEST(SharedAudioCapture, StalledPreparationHasFiniteDeadlineAndRetainsResourcesUntilOwnerCleanup) {
    auto gate = std::make_shared<NativeGate>();
    GateRelease release{gate};
    auto state = std::make_shared<NativeState>();
    SharedAudioCaptureRegistry registry;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    auto readers = registry.AttachReaders({StalledRequest(StallStage::Init, gate, state)}, 0, error);
    EXPECT_TRUE(readers.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(750));
    EXPECT_NE(error.find("stalled"), std::string::npos);
    EXPECT_TRUE(gate->entered.load());
    EXPECT_GT(registry.ReservedPayloadBytes(), 0u);
    gate->Release();
    ASSERT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
    EXPECT_FALSE(state->wrong_thread.load());
    EXPECT_GE(state->shutdowns.load(), 1u);
}

TEST(SharedAudioCapture, StalledAcquireHasFiniteSealAndLastReaderShutdownDoesNotPinTheOwner) {
    auto gate = std::make_shared<NativeGate>();
    GateRelease release{gate};
    auto state = std::make_shared<NativeState>();
    auto registry = std::make_unique<SharedAudioCaptureRegistry>();
    std::string error;
    auto readers = registry->AttachReaders({StalledRequest(StallStage::Acquire, gate, state)}, 0, error);
    ASSERT_EQ(readers.size(), 1u) << error;
    ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(); }));
    auto started = std::chrono::steady_clock::now();
    const auto end = SealAudioReaderEnd(*readers[0], 2'000'000'000ULL);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(750));
    EXPECT_TRUE(end.stalled);
    EXPECT_FALSE(end.undrained_frames.has_value());
    started = std::chrono::steady_clock::now();
    readers.clear();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(250));
    EXPECT_GT(registry->ReservedPayloadBytes(), 0u);
    gate->Release();
    ASSERT_TRUE(WaitUntil([&] { return registry->ReservedPayloadBytes() == 0; }));
    EXPECT_FALSE(state->wrong_thread.load());
    EXPECT_GE(state->shutdowns.load(), 1u);
}

TEST(SharedAudioCapture, StalledNativeShutdownDoesNotBlockReaderOrRegistryDestruction) {
    auto gate = std::make_shared<NativeGate>();
    GateRelease release{gate};
    auto state = std::make_shared<NativeState>();
    auto registry = std::make_unique<SharedAudioCaptureRegistry>();
    std::string error;
    auto readers = registry->AttachReaders({StalledRequest(StallStage::Shutdown, gate, state)}, 0, error);
    ASSERT_EQ(readers.size(), 1u) << error;
    auto started = std::chrono::steady_clock::now();
    readers.clear();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(250));
    ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(); }));
    EXPECT_GT(registry->ReservedPayloadBytes(), 0u);
    started = std::chrono::steady_clock::now();
    registry.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(250));
    gate->Release();
    ASSERT_TRUE(WaitUntil([&] { return state->shutdowns.load() != 0; }));
    EXPECT_FALSE(state->wrong_thread.load());
}

class FailedNativeInit final : public IAudioCaptureSource {
  public:
    bool Init(std::string&) override {
        return false;
    }
    uint32_t PendingFrameCount() override {
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer&, std::string&) override {
        return false;
    }
    void ReleaseBuffer() override {
    }
    uint32_t SampleRate() const override {
        return 8000;
    }
    uint32_t Channels() const override {
        return 1;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    void Shutdown() override {
    }

  private:
    std::string name_ = "failed native Init";
};

TEST(SharedAudioCapture, FailedInitWithEmptyBackendErrorRollsBackEveryPreparedGroup) {
    SharedAudioCaptureRegistry registry;
    auto state = std::make_shared<NativeState>();
    SharedAudioCaptureRequest healthy;
    healthy.key.endpoint_id = "a healthy endpoint";
    healthy.admission = Admission();
    healthy.native_factory = [state] { return std::make_unique<VirtualNative>(0, 8000, 1, state); };
    auto failed = healthy;
    failed.key.endpoint_id = "z failed endpoint";
    failed.native_factory = [] { return std::make_unique<FailedNativeInit>(); };
    std::string error;
    auto readers = registry.AttachReaders({healthy, failed}, 0, error);
    EXPECT_TRUE(readers.empty());
    EXPECT_FALSE(error.empty());
    readers.clear();
    ASSERT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
    EXPECT_GE(state->shutdowns.load(), 1u);
    EXPECT_FALSE(state->wrong_thread.load());
}

TEST(SharedAudioCapture, NullNativeFactoryResultFailsBeforeSourceInitialization) {
    SharedAudioCaptureRegistry registry;
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "null source";
    request.admission = Admission();
    request.native_factory = [] { return std::unique_ptr<IAudioCaptureSource>{}; };
    std::string error;
    EXPECT_TRUE(registry.AttachReaders({request}, 0, error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

TEST(SharedAudioCapture, ConflictingSharedKeyPolicyFailsBeforeOpeningAndForExistingOwner) {
    SharedAudioCaptureRegistry registry;
    auto state = std::make_shared<NativeState>();
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "shared policy";
    request.admission = Admission();
    request.native_factory = [state] { return std::make_unique<VirtualNative>(0, 8000, 1, state); };
    auto different = request;
    different.correction_enabled = false;
    std::string error;
    auto rejected = registry.AttachReaders({request, different}, 0, error);
    EXPECT_TRUE(rejected.empty());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(state->opens.load(), 0u);
    rejected.clear();
    ASSERT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
    auto readers = registry.AttachReaders({request}, 0, error);
    ASSERT_EQ(readers.size(), 1u) << error;
    EXPECT_TRUE(registry.AttachReaders({different}, 0, error).empty());
    EXPECT_FALSE(error.empty());
    different = request;
    ++different.admission.maximum_packet_frames;
    EXPECT_TRUE(registry.AttachReaders({different}, 0, error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(state->opens.load(), 1u);
    readers.clear();
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

TEST(SharedAudioCapture, RuntimeCapacityRejectsBeforeNativeOpenAndRollsBackTheWholeAttachment) {
    const size_t group_bytes = AudioTee::RequiredPayloadBytes(Admission());
    SharedAudioCaptureRegistry registry(2 * group_bytes);
    std::vector<std::shared_ptr<NativeState>> states;
    std::vector<SharedAudioCaptureRequest> requests(3);
    for (uint32_t index = 0; index < 3; ++index) {
        auto state = std::make_shared<NativeState>();
        states.push_back(state);
        requests[index].key.endpoint_id = "budget endpoint " + std::to_string(index);
        requests[index].admission = Admission();
        requests[index].native_factory = [state] { return std::make_unique<VirtualNative>(0, 8000, 1, state); };
    }
    const auto opens = [&] {
        uint32_t count = 0;
        for (const auto& state : states)
            count += state->opens.load();
        return count;
    };
    std::string error;
    EXPECT_TRUE(registry.AttachReaders(requests, 0, error).empty());
    EXPECT_NE(error.find("budget"), std::string::npos);
    EXPECT_EQ(opens(), 0u);
    ASSERT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
    auto readers = registry.AttachReaders({requests[0], requests[1]}, 0, error);
    ASSERT_EQ(readers.size(), 2u) << error;
    EXPECT_EQ(registry.ReservedPayloadBytes(), 2 * group_bytes);
    EXPECT_TRUE(registry.AttachReaders({requests[2]}, 0, error).empty());
    EXPECT_NE(error.find("budget"), std::string::npos);
    EXPECT_EQ(opens(), 2u);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 2 * group_bytes);
    EXPECT_TRUE(readers[0]->ClockCorrected());
    EXPECT_TRUE(readers[1]->ClockCorrected());
    readers.clear();
    EXPECT_TRUE(WaitUntil([&] { return registry.ReservedPayloadBytes() == 0; }));
}

class PhysicalAudioClock : public testing::TestWithParam<double> {};

TEST_P(PhysicalAudioClock, TwoHoursRealPcmCorrectionAndMarkerErrorWithin15MsAfterSettling) {
    constexpr uint32_t rate = 8000;
    const double ppm = GetParam();
    PhysicalAudioClockSource source(std::make_unique<VirtualNative>(ppm), Admission());
    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;
    EXPECT_TRUE(source.ClockCorrected());
    uint64_t output_frames = 0;
    bool inside_marker = false;
    uint32_t markers = 0;
    double maximum_error_ms = 0;
    for (uint32_t packet = 0; packet < 720000; ++packet) {
        RawAudioBuffer block;
        ASSERT_TRUE(source.AcquireBuffer(block, error)) << error;
        const auto* pcm = reinterpret_cast<const float*>(block.bytes);
        for (uint32_t frame = 0; frame < block.num_frames; ++frame) {
            const bool pulse = pcm[frame] > 0.4f;
            if (pulse && !inside_marker) {
                if (markers > 0) {
                    const double event_ms = static_cast<double>(output_frames + frame) / rate * 1000.0;
                    const double expected_ms = markers * 300'000.0;
                    maximum_error_ms = (std::max)(maximum_error_ms, std::abs(event_ms - expected_ms));
                }
                ++markers;
            }
            inside_marker = pulse;
        }
        output_frames += block.num_frames;
        source.ReleaseBuffer();
    }
    const auto stats = source.Statistics();
    EXPECT_GE(markers, 24u);
    EXPECT_LE(maximum_error_ms, 15.0);
    EXPECT_EQ(stats.input_frames, 7200ULL * rate);
    EXPECT_EQ(stats.output_frames, output_frames);
    EXPECT_GT(stats.compensation_updates, 0u);
    EXPECT_EQ(stats.clock_quality, PhysicalClockQuality::Measured);
    EXPECT_GT(std::abs(stats.applied_compensation_ms), 100.0);
    EXPECT_LT(std::abs(stats.residual_ms), 15.0);
    EXPECT_EQ(stats.applied_compensation_ms > 0, ppm > 0);
    EXPECT_EQ(output_frames > stats.input_frames, ppm > 0);
    source.Shutdown();
}

INSTANTIATE_TEST_SUITE_P(IndependentNativeClocks, PhysicalAudioClock,
                         testing::Values(-500.0, -250.0, -100.0, -50.0, 50.0, 100.0, 250.0, 500.0));

TEST(PhysicalAudioClock, Native96kSixChannelsRemainNativeAndOptOutDoesNotCorrect) {
    PhysicalAudioClockSource source(std::make_unique<VirtualNative>(250, 96000, 6), Admission(96000, 6), false);
    std::string error;
    ASSERT_TRUE(source.Init(error)) << error;
    EXPECT_EQ(source.SampleRate(), 96000u);
    EXPECT_EQ(source.Channels(), 6u);
    for (uint32_t packet = 0; packet < 1000; ++packet) {
        RawAudioBuffer block;
        ASSERT_TRUE(source.AcquireBuffer(block, error)) << error;
        EXPECT_EQ(block.num_frames, 960u);
        source.ReleaseBuffer();
    }
    const auto stats = source.Statistics();
    EXPECT_EQ(stats.input_frames, stats.output_frames);
    EXPECT_DOUBLE_EQ(stats.compensation_ppm, 0.0);
    EXPECT_EQ(stats.compensation_updates, 0u);
    source.Shutdown();
}

TEST(SharedAudioCapture, MasterOnlySingleReadersHaveOneCorrectorPerPhysicalClock) {
    SharedAudioCaptureRegistry registry;
    std::vector<SharedAudioCaptureRequest> requests(2);
    for (uint32_t index = 0; index < 2; ++index) {
        requests[index].key.endpoint_id = "independent" + std::to_string(index);
        requests[index].admission = Admission();
        requests[index].native_factory = [index] { return std::make_unique<VirtualNative>(index ? -250.0 : 250.0); };
    }
    std::string error;
    auto readers = registry.AttachReaders(requests, 1'000'000'000ULL, error);
    ASSERT_EQ(readers.size(), 2u) << error;
    EXPECT_TRUE(readers[0]->ClockCorrected());
    EXPECT_TRUE(readers[1]->ClockCorrected());
    EXPECT_EQ(registry.Statistics().capture_opens, 2u);
    EXPECT_GT(registry.ReservedPayloadBytes(), 0u);
    for (auto& reader : readers) {
        (void)SealAudioReaderEnd(*reader, 2'000'000'000ULL);
        reader->Shutdown();
    }
}

TEST(SharedAudioCapture, SharedKeyOpensOnceAndShutdownStaysOnPhysicalOwnerThread) {
    auto state = std::make_shared<NativeState>();
    SharedAudioCaptureRegistry registry;
    SharedAudioCaptureRequest request;
    request.key.endpoint_id = "fixed native device";
    request.admission = Admission();
    request.native_factory = [state] { return std::make_unique<VirtualNative>(50.0, 8000, 1, state); };
    std::string error;
    auto readers = registry.AttachReaders({request, request, request}, 1'000'000'000ULL, error);
    ASSERT_EQ(readers.size(), 3u) << error;
    EXPECT_EQ(state->opens.load(), 1u);
    EXPECT_EQ(registry.Statistics().capture_opens, 1u);
    EXPECT_NE(state->owner_thread, std::this_thread::get_id());
    for (auto& reader : readers)
        reader->Shutdown();
    readers.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (registry.ReservedPayloadBytes() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_FALSE(state->wrong_thread.load());
    EXPECT_GE(state->shutdowns.load(), 1u);
    EXPECT_EQ(registry.ReservedPayloadBytes(), 0u);
}
} // namespace
