#include "audio_tee.h"
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <string>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <thread>
#include <vector>

namespace {
using namespace exosnap::engine;

AudioCaptureDescriptor Descriptor() {
    AudioCaptureDescriptor descriptor;
    descriptor.sample_rate = 1000;
    descriptor.channels = 1;
    descriptor.maximum_packet_frames = 100;
    return descriptor;
}

void Publish(AudioTee& tee, uint64_t packet, uint32_t frames = 100, float value = 0.5f) {
    std::vector<float> samples(frames, value);
    AudioBlockMetadata metadata;
    metadata.frames = frames;
    metadata.output_qpc_ns = 1'000'000'000ULL + packet * 100'000'000ULL;
    metadata.capture_qpc_ns = metadata.output_qpc_ns;
    metadata.native_sample_position = packet * 100;
    metadata.native_sample_position_valid = true;
    std::string error;
    ASSERT_TRUE(tee.Publish(samples, metadata, error)) << error;
}

TEST(AudioTee, HeldLeaseSurvivesRepeatedRingWrapAndSecondAcquireIsRejected) {
    std::string error;
    auto tee = AudioTee::Create(Descriptor(), error);
    ASSERT_NE(tee, nullptr) << error;
    auto readers = tee->AttachReaders(2, 0, error);
    ASSERT_EQ(readers.size(), 2u) << error;
    Publish(*tee, 0, 100, 0.25f);
    AudioBlockView held;
    ASSERT_TRUE(readers[0]->Acquire(held, error)) << error;
    const float* address = held.samples.data();
    AudioBlockView duplicate;
    EXPECT_FALSE(readers[0]->Acquire(duplicate, error));
    const size_t payload = tee->ReservedPayloadBytes();
    for (uint64_t packet = 1; packet < 100; ++packet) {
        Publish(*tee, packet, 100, 0.75f);
        AudioBlockView fast;
        ASSERT_TRUE(readers[1]->Acquire(fast, error)) << error;
        readers[1]->Release();
    }
    EXPECT_EQ(held.samples.data(), address);
    EXPECT_TRUE(std::all_of(held.samples.begin(), held.samples.end(), [](float sample) { return sample == 0.25f; }));
    EXPECT_EQ(held.metadata.native_sample_position, 0u);
    EXPECT_EQ(tee->ReservedPayloadBytes(), payload);
    readers[0]->Release();
}

TEST(AudioTee, SlowReaderReportsExactLocalGapAndHasIndependentWakeup) {
    std::string error;
    auto tee = AudioTee::Create(Descriptor(), error);
    ASSERT_NE(tee, nullptr) << error;
    auto readers = tee->AttachReaders(2, 0, error);
    ASSERT_EQ(readers.size(), 2u);
#ifdef _WIN32
    EXPECT_NE(readers[0]->ReadyEvent(), nullptr);
    EXPECT_NE(readers[0]->ReadyEvent(), readers[1]->ReadyEvent());
#endif
    const uint64_t wake = readers[0]->WakeSequence();
    for (uint64_t packet = 0; packet < 25; ++packet) {
        Publish(*tee, packet);
        AudioBlockView fast;
        ASSERT_TRUE(readers[1]->Acquire(fast, error)) << error;
        EXPECT_EQ(fast.reader_overflow_frames, 0u);
        readers[1]->Release();
    }
    AudioBlockView slow;
    ASSERT_TRUE(readers[0]->Acquire(slow, error)) << error;
    EXPECT_EQ(slow.reader_overflow_frames, 500u);
    EXPECT_EQ(slow.metadata.sequence, 5u);
    EXPECT_EQ(slow.metadata.device_gap_frames, 0u);
    EXPECT_GT(readers[0]->WakeSequence(), wake);
    readers[0]->Release();
}

TEST(AudioTee, AdmissionCountsRingAllHeldReadersAndCorrectionScratch) {
    auto descriptor = Descriptor();
    const size_t expected = (2000u + 16u * 100u) * sizeof(float) + AudioCorrectionPayloadBytes(descriptor);
    EXPECT_EQ(AudioTee::RequiredPayloadBytes(descriptor), expected);
    std::string error;
    auto tee = AudioTee::Create(descriptor, error);
    ASSERT_NE(tee, nullptr) << error;
    EXPECT_EQ(tee->ReservedPayloadBytes(), expected);
    EXPECT_GT(tee->MetadataBytes(), 0u);
    descriptor.sample_rate = 384000;
    descriptor.channels = 32;
    descriptor.maximum_packet_frames = 96000;
    EXPECT_EQ(AudioTee::Create(descriptor, error), nullptr);
    EXPECT_NE(error.find("budget"), std::string::npos);
    descriptor.sample_rate = 48000;
    descriptor.channels = 2;
    descriptor.maximum_packet_frames = 12256;
    EXPECT_LT(16u * AudioTee::RequiredPayloadBytes(descriptor), kAudioRuntimePayloadBytes);
}

TEST(AudioTee, AttachIsAtomicAndSealDrainsFinitePrefixWithContinuingProducer) {
    std::string error;
    auto tee = AudioTee::Create(Descriptor(), error);
    ASSERT_NE(tee, nullptr) << error;
    auto readers = tee->AttachReaders(15, 0, error);
    ASSERT_EQ(readers.size(), 15u);
    EXPECT_TRUE(tee->AttachReaders(2, 0, error).empty());
    EXPECT_EQ(tee->ReaderCount(), 15u);
    Publish(*tee, 0);
    Publish(*tee, 1);
    const auto end = readers[0]->SealEnd(1'150'000'000ULL);
    EXPECT_EQ(end.end_sequence_exclusive, 2u);
    for (uint64_t packet = 2; packet < 10; ++packet)
        Publish(*tee, packet);
    uint64_t frames = 0;
    AudioBlockView block;
    while (readers[0]->Acquire(block, error)) {
        frames += block.metadata.frames;
        readers[0]->Release();
    }
    EXPECT_EQ(frames, 150u);
    EXPECT_TRUE(readers[0]->Ended());
    EXPECT_FALSE(readers[1]->Ended());
    EXPECT_EQ(readers[0]->PendingFrames(), 0u);
}

TEST(AudioTee, CommonStartBoundaryTrimsExistingPrefixAndFreezesGeneration) {
    std::string error;
    auto tee = AudioTee::Create(Descriptor(), error);
    ASSERT_NE(tee, nullptr) << error;
    Publish(*tee, 0);
    Publish(*tee, 1);
    auto readers = tee->AttachReaders(2, 1'050'000'000ULL, error);
    ASSERT_EQ(readers.size(), 2u);
    AudioBlockView a;
    AudioBlockView b;
    ASSERT_TRUE(readers[0]->Acquire(a, error));
    ASSERT_TRUE(readers[1]->Acquire(b, error));
    EXPECT_EQ(a.metadata.output_qpc_ns, 1'050'000'000ULL);
    EXPECT_EQ(a.metadata.frames, 50u);
    EXPECT_EQ(a.metadata.native_sample_position, 50u);
    EXPECT_EQ(a.metadata.sequence, b.metadata.sequence);
    auto descriptor = Descriptor();
    descriptor.source_generation = 2;
    descriptor.format_generation = 2;
    ASSERT_TRUE(tee->SetDescriptor(descriptor, error));
    Publish(*tee, 2);
    EXPECT_EQ(a.descriptor.source_generation, 1u);
    readers[0]->Release();
    readers[1]->Release();
}

TEST(AudioTee, AttachStopRaceCannotExposeAReaderAfterStoppedState) {
    std::string error;
    auto tee = AudioTee::Create(Descriptor(), error);
    ASSERT_NE(tee, nullptr) << error;
    std::thread stop([&] { tee->Stop(); });
    auto readers = tee->AttachReaders(1, 0, error);
    stop.join();
    if (!readers.empty()) {
        EXPECT_TRUE(readers.front()->Ended());
    }
    EXPECT_TRUE(tee->AttachReaders(1, 0, error).empty());
    AudioBlockMetadata metadata;
    metadata.frames = 1;
    const std::array<float, 1> sample{0.0f};
    EXPECT_FALSE(tee->Publish(sample, metadata, error));
}
} // namespace
