// Worker teardown ownership.
//
// When a finalize stall or producer hang exhausts the shutdown budget, the
// session abandons the worker. The old destructors detach()ed the joinable
// thread, leaving it to write through the worker object and the SessionState
// while both were being destroyed — a latent use-after-free in the stop phase.
//
// The fix hands the running thread shared ownership of its worker (and through
// it the SessionState) at Start(). These tests pin the resulting invariants
// with deterministic blocked workers (no capture hardware, no wall-clock
// dependence beyond bounded polls):
//   * dropping every external handle on a still-blocked worker keeps the
//     SessionState alive (observed via weak_ptr),
//   * the stop flag / failure path wakes the blocked worker, which then runs
//     out and releases the state (weak_ptr expires),
//   * a producer blocked on the mux queue bound never deadlocks teardown.

#include <gtest/gtest.h>

#include "audio_thread.h"
#include "mux_thread.h"
#include "session_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using exosnap::engine::AudioCodec;
using exosnap::engine::AudioSampleFormat;
using exosnap::engine::AudioThread;
using exosnap::engine::ErrorPhase;
using exosnap::engine::IAudioCaptureSource;
using exosnap::engine::MuxThread;
using exosnap::engine::RawAudioBuffer;
using exosnap::engine::SessionState;

bool PollUntil(const std::function<bool()>& pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ---------------------------------------------------------------------------
// MuxThread: blocked in Run() (codec-private wait) while every external
// handle is dropped — the stand-in for the real finalize stall, where the
// session returns from Record() while the mux worker is still inside a
// blocking writer call.
// ---------------------------------------------------------------------------

TEST(WorkerTeardownOwnership, AbandonedMuxWorkerKeepsSessionStateAliveUntilItRunsOut) {
    auto state = std::make_shared<SessionState>();
    std::weak_ptr<SessionState> state_weak = state;
    state->audio_track_count = 1; // nothing marks readiness -> Run() blocks on premux_cv

    auto mux = std::make_shared<MuxThread>(state);
    std::weak_ptr<MuxThread> mux_weak = mux;
    mux->Start();

    // The worker is blocked; a bounded join must fail.
    EXPECT_FALSE(mux->Join(100));

    // Session gives up: every external handle is dropped while the thread is
    // still blocked inside Run().
    mux.reset();
    state.reset();

    // The thread's shared ownership keeps worker AND state alive — this is the
    // exact window where the old detach() left them freed under the thread.
    EXPECT_FALSE(mux_weak.expired());
    ASSERT_FALSE(state_weak.expired());

    // The shutdown flag reaches the abandoned worker and wakes it...
    {
        auto st = state_weak.lock();
        ASSERT_NE(st, nullptr);
        st->stop_requested.store(true);
        st->premux_cv.notify_all();
    }

    // ...and once it runs out, everything it owned is released.
    EXPECT_TRUE(PollUntil([&] { return state_weak.expired(); }, std::chrono::seconds(10)))
        << "abandoned mux worker did not release the SessionState after waking";
    EXPECT_TRUE(PollUntil([&] { return mux_weak.expired(); }, std::chrono::seconds(10)));
}

// ---------------------------------------------------------------------------
// AudioThread: blocked inside its capture source while abandoned. The source
// is owned BY the worker, so with the old detach() the source itself was
// destroyed under the blocked thread. The destruction-order flag makes that
// observable without ASAN.
// ---------------------------------------------------------------------------

// Blocks inside AcquireBuffer until released; flags destruction so the test
// can prove the source outlives the blocked call.
class BlockingSource : public IAudioCaptureSource {
  public:
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool release = false;
        std::atomic<bool> in_acquire{false};
        std::atomic<bool> destroyed{false};
    };

    explicit BlockingSource(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {
    }
    ~BlockingSource() override {
        shared_->destroyed.store(true);
    }

    bool Init(std::string&) override {
        return true;
    }
    uint32_t PendingFrameCount() override {
        return released_once_ ? 0u : 480u; // exactly one (blocking) acquire
    }
    bool AcquireBuffer(RawAudioBuffer&, std::string& out_error) override {
        shared_->in_acquire.store(true);
        std::unique_lock lk(shared_->m);
        shared_->cv.wait(lk, [&] { return shared_->release; });
        released_once_ = true;
        out_error.clear(); // benign "no data" result -> drain loop continues
        return false;
    }
    void ReleaseBuffer() override {
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
    }

  private:
    std::shared_ptr<Shared> shared_;
    bool released_once_ = false;
    std::string name_ = "BlockingSource";
};

TEST(WorkerTeardownOwnership, AbandonedAudioWorkerKeepsItsSourceAliveWhileBlockedInsideIt) {
    auto state = std::make_shared<SessionState>();
    std::weak_ptr<SessionState> state_weak = state;
    state->config.audio_codec = AudioCodec::Pcm;
    state->config.audio_bit_depth = 16;
    state->audio_track_count = 1;

    auto gate = std::make_shared<BlockingSource::Shared>();
    auto worker = std::make_shared<AudioThread>(state, std::make_unique<BlockingSource>(gate), 0);
    worker->Start();

    // Wait until the thread is provably blocked INSIDE the source.
    ASSERT_TRUE(PollUntil([&] { return gate->in_acquire.load(); }, std::chrono::seconds(10)));

    // Stop is requested, but the thread cannot observe it while blocked.
    state->stop_requested.store(true);
    EXPECT_FALSE(worker->Join(100));

    // Session gives up and drops every handle.
    worker.reset();
    state.reset();

    // Worker, state, and — critically — the source the thread is currently
    // executing inside must all still be alive.
    EXPECT_FALSE(state_weak.expired());
    EXPECT_FALSE(gate->destroyed.load()) << "source destroyed while the worker thread is blocked inside it";

    // Unblock; the worker observes stop_requested, runs out, and releases
    // everything in order (source destroyed only after the thread left it).
    {
        std::lock_guard lk(gate->m);
        gate->release = true;
    }
    gate->cv.notify_all();

    EXPECT_TRUE(PollUntil([&] { return gate->destroyed.load(); }, std::chrono::seconds(10)));
    EXPECT_TRUE(PollUntil([&] { return state_weak.expired(); }, std::chrono::seconds(10)))
        << "abandoned audio worker did not release the SessionState after running out";
}

// ---------------------------------------------------------------------------
// Interaction with the mux queue bound: a producer blocked on the bound is
// woken by the failure path during teardown, then runs out and releases the
// state — the bound can never deadlock an abandoned worker.
// ---------------------------------------------------------------------------

// Floods packets forever (until stop is observed).
class EndlessSource : public IAudioCaptureSource {
  public:
    bool Init(std::string&) override {
        return true;
    }
    uint32_t PendingFrameCount() override {
        return 480;
    }
    bool AcquireBuffer(RawAudioBuffer& out, std::string&) override {
        out.bytes = reinterpret_cast<const uint8_t*>(data_.data());
        out.num_frames = 480;
        out.silent = false;
        return true;
    }
    void ReleaseBuffer() override {
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
    }

  private:
    std::vector<float> data_ = std::vector<float>(480 * 2, 0.1f);
    std::string name_ = "EndlessSource";
};

TEST(WorkerTeardownOwnership, ProducerBlockedOnQueueBoundNeverDeadlocksTeardown) {
    auto state = std::make_shared<SessionState>();
    std::weak_ptr<SessionState> state_weak = state;
    state->config.audio_codec = AudioCodec::Pcm;
    state->config.audio_bit_depth = 16;
    state->audio_track_count = 1;
    state->codec_private.av1_ready = true;    // route straight into mux_queue
    state->mux_queue_packet_limit = 4;        // tiny bound, nobody consumes
    state->mux_queue_full_timeout_ms = 60000; // the failure path, not the timeout, must unblock

    auto worker = std::make_shared<AudioThread>(state, std::make_unique<EndlessSource>(), 0);
    worker->Start();

    // Wait until the producer has filled the bound (it is now blocked in
    // WaitForMuxQueueSpace with an hour-scale timeout).
    ASSERT_TRUE(PollUntil(
        [&] {
            std::lock_guard lk(state->mux_mutex);
            return state->mux_queue.size() >= 4;
        },
        std::chrono::seconds(10)));
    EXPECT_FALSE(worker->Join(100));

    // Teardown: the failure path must wake the blocked producer.
    state->RecordFailure(E_FAIL, ErrorPhase::Mux, "teardown");
    worker.reset();
    state.reset();

    EXPECT_TRUE(PollUntil([&] { return state_weak.expired(); }, std::chrono::seconds(10)))
        << "producer blocked on the mux queue bound did not run out during teardown";
}

} // namespace
