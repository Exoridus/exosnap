// Steady-state mux queue backpressure.
//
// The premux buffers are bounded, but once codec-private data is ready the
// producers used to push into mux_queue unchecked: a destination volume that
// cannot keep up (slow NAS, AV scan, low disk) made the queue grow until OOM
// with no diagnosis. These tests pin the bound:
//   * a full queue blocks the producer and times out into a deterministic
//     ErrorPhase::Mux failure — no silent packet drops, no unbounded growth,
//   * a draining consumer wakes a blocked producer,
//   * a recorded failure wakes a blocked producer (teardown never deadlocks
//     on the bound).

#include <gtest/gtest.h>

#include "audio_thread.h"
#include "session_internal.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using recorder_core::AudioCodec;
using recorder_core::AudioSampleFormat;
using recorder_core::AudioThread;
using recorder_core::EncodedAudioPacket;
using recorder_core::ErrorPhase;
using recorder_core::IAudioCaptureSource;
using recorder_core::MuxItem;
using recorder_core::RawAudioBuffer;
using recorder_core::SessionState;

MuxItem MakeAudioItem(size_t payload_bytes) {
    EncodedAudioPacket pkt;
    pkt.bytes.assign(payload_bytes, 0x5A);
    MuxItem item;
    item.payload = std::move(pkt);
    return item;
}

// ---------------------------------------------------------------------------
// SessionState-level bound semantics
// ---------------------------------------------------------------------------

TEST(MuxQueueBackpressure, FullQueueTimesOutDeterministically) {
    SessionState state{};
    state.mux_queue_packet_limit = 4;
    state.mux_queue_full_timeout_ms = 50;

    {
        std::unique_lock lk(state.mux_mutex);
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(state.WaitForMuxQueueSpace(lk));
            state.PushMuxItemLocked(MakeAudioItem(16));
        }
        const auto t0 = std::chrono::steady_clock::now();
        EXPECT_FALSE(state.WaitForMuxQueueSpace(lk));
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 40);
    }
    EXPECT_EQ(state.mux_queue.size(), 4u);
}

TEST(MuxQueueBackpressure, ByteLimitBlocksBeforePacketLimit) {
    SessionState state{};
    state.mux_queue_packet_limit = 1000;
    state.mux_queue_byte_limit = 64;
    state.mux_queue_full_timeout_ms = 50;

    std::unique_lock lk(state.mux_mutex);
    ASSERT_TRUE(state.WaitForMuxQueueSpace(lk));
    state.PushMuxItemLocked(MakeAudioItem(64)); // reaches the byte bound
    EXPECT_FALSE(state.WaitForMuxQueueSpace(lk));
}

TEST(MuxQueueBackpressure, DrainingConsumerWakesBlockedProducer) {
    SessionState state{};
    state.mux_queue_packet_limit = 2;
    state.mux_queue_full_timeout_ms = 5000;

    {
        std::unique_lock lk(state.mux_mutex);
        state.PushMuxItemLocked(MakeAudioItem(16));
        state.PushMuxItemLocked(MakeAudioItem(16));
    }

    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        std::unique_lock lk(state.mux_mutex);
        if (state.WaitForMuxQueueSpace(lk)) {
            state.PushMuxItemLocked(MakeAudioItem(16));
            pushed.store(true);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load());

    // Consumer pops one item — the producer must wake and push.
    {
        std::lock_guard lk(state.mux_mutex);
        MuxItem item = std::move(state.mux_queue.front());
        state.mux_queue.pop_front();
        state.OnMuxItemPopped(item);
    }
    producer.join();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(state.mux_queue.size(), 2u);
}

TEST(MuxQueueBackpressure, RecordedFailureWakesBlockedProducer) {
    SessionState state{};
    state.mux_queue_packet_limit = 1;
    state.mux_queue_full_timeout_ms = 60000; // must NOT be what unblocks us

    {
        std::unique_lock lk(state.mux_mutex);
        state.PushMuxItemLocked(MakeAudioItem(16));
    }

    std::atomic<bool> wait_result{true};
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::unique_lock lk(state.mux_mutex);
        wait_result.store(state.WaitForMuxQueueSpace(lk));
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(done.load());

    state.RecordFailure(E_FAIL, ErrorPhase::Mux, "test failure");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(done.load()) << "failure must wake a producer blocked on the queue bound";
    producer.join();
    EXPECT_FALSE(wait_result.load());
}

// ---------------------------------------------------------------------------
// Producer-level behavior: AudioThread against a stalled consumer
// ---------------------------------------------------------------------------

// Delivers `packet_count` Float32 packets as fast as the thread drains them.
class FloodMockSource : public IAudioCaptureSource {
  public:
    FloodMockSource(std::atomic<bool>* stop_flag, size_t packet_count) : stop_flag_(stop_flag) {
        packets_ = packet_count;
        data_.assign(static_cast<size_t>(kFramesPerPacket) * kChannels, 0.1f);
    }

    bool Init(std::string&) override {
        initialized_ = true;
        return true;
    }
    uint32_t PendingFrameCount() override {
        if (!initialized_ || acquired_)
            return 0;
        if (delivered_ < packets_)
            return kFramesPerPacket;
        if (stop_flag_)
            stop_flag_->store(true);
        return 0;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        if (!initialized_ || acquired_ || delivered_ >= packets_)
            return false;
        acquired_ = true;
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = kFramesPerPacket;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
        if (!acquired_)
            return;
        acquired_ = false;
        ++delivered_;
        if (delivered_ >= packets_ && stop_flag_)
            stop_flag_->store(true);
    }
    uint32_t SampleRate() const override {
        return 48000;
    }
    uint32_t Channels() const override {
        return kChannels;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return name_;
    }
    void Shutdown() override {
    }

    static constexpr uint32_t kFramesPerPacket = 960;
    static constexpr uint32_t kChannels = 2;

  private:
    std::atomic<bool>* stop_flag_ = nullptr;
    bool initialized_ = false;
    bool acquired_ = false;
    size_t packets_ = 0;
    size_t delivered_ = 0;
    std::vector<float> data_;
    std::string name_ = "FloodMock";
};

// With codec-private data already ready (bothReady == true) the producer pushes
// straight into mux_queue. Nothing consumes it — exactly the slow-destination
// scenario. The queue must stop at the bound and the session must fail with an
// ErrorPhase::Mux error instead of growing without limit.
TEST(MuxQueueBackpressure, AudioProducerFailsInsteadOfUnboundedGrowth) {
    SessionState state{};
    state.config.audio_codec = AudioCodec::Pcm;
    state.config.audio_bit_depth = 16;
    state.audio_track_count = 1;
    state.mux_queue_packet_limit = 8;
    state.mux_queue_full_timeout_ms = 100;

    // Video codec private already ready: packets route past the premux phase.
    state.codec_private.av1_ready = true;

    auto source = std::make_unique<FloodMockSource>(&state.stop_requested, 100);
    AudioThread thread(state, std::move(source), 0);
    thread.Start();
    ASSERT_TRUE(thread.Join(15000));

    EXPECT_TRUE(state.HasFailure());
    {
        std::lock_guard lk(state.failure_mutex);
        EXPECT_EQ(state.failure.error_phase, ErrorPhase::Mux);
    }

    // Bounded: at most the limit plus the EOS sentinel the drain still enqueues.
    std::lock_guard lk(state.mux_mutex);
    EXPECT_LE(state.mux_queue.size(), 8u + 2u);
}

} // namespace
