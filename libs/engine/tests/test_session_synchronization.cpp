#include "audio_capture_drain.h"
#include "exosnap/engine/codec_types.h"
#include "exosnap/engine/error_types.h"
#include "exosnap/engine/packet_types.h"
#include "exosnap/engine/record_request_id.h"
#include "premux_state.h"
#include "session_failure.h"
#include "session_request_gate.h"
#include "split_sentinel_policy.h"
#include <exosnap/engine/audio_limits.h>

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace exosnap::engine;
using namespace std::chrono_literals;

TEST(PremuxState, ReadinessWaitRequiresVideoAndEveryAudioTrack) {
    PremuxState state;
    std::atomic<bool> stopped{false};
    auto waiter = std::async(std::launch::async,
                             [&] { return state.WaitReady(VideoCodec::Av1, 2, [&] { return stopped.load(); }); });
    EXPECT_TRUE(state.WaitForWaiter(1s));
    state.PublishVideo(VideoCodec::Av1, {1, 2});
    state.PublishAudio(0, {{}, 13});
    EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
    state.PublishAudio(1, {{}, 17});
    const auto ready = waiter.wait_for(1s);
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready != std::future_status::ready) {
        stopped.store(true);
        state.NotifyStop();
    }
    EXPECT_TRUE(waiter.get());
    const auto codec = state.CodecSnapshot();
    EXPECT_EQ(codec.av1_codec_private, (std::vector<uint8_t>{1, 2}));
    EXPECT_EQ(codec.audio_codec_private[1].codec_delay_samples, 17u);
}

TEST(PremuxState, StopReleasesAnActuallyWaitingMuxWithoutInventingReadiness) {
    PremuxState state;
    std::atomic<bool> stopped{false};
    auto waiter = std::async(std::launch::async,
                             [&] { return state.WaitReady(VideoCodec::H264, 1, [&] { return stopped.load(); }); });
    EXPECT_TRUE(state.WaitForWaiter(1s));
    stopped.store(true);
    state.NotifyStop();
    EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(waiter.get());
}

TEST(PremuxState, RoutesPreserveBoundsPacketOwnershipAndResetReadiness) {
    PremuxState state;
    EncodedVideoPacket video;
    video.bytes = {1};
    for (size_t i = 0; i < PremuxState::kVideoLimit; ++i) {
        video.bytes = {1};
        ASSERT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Buffered);
    }
    video.bytes = {2};
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Full);
    EXPECT_EQ(video.bytes, (std::vector<uint8_t>{2}));
    auto pending = state.TakePending();
    EXPECT_EQ(pending.video.size(), PremuxState::kVideoLimit);
    EXPECT_TRUE(state.PendingSnapshot().video.empty());
    state.PublishVideo(VideoCodec::Av1, {3});
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Mux);
    EXPECT_EQ(video.bytes, (std::vector<uint8_t>{2}));
    state.Reset();
    EXPECT_FALSE(state.CodecSnapshot().VideoReady(VideoCodec::Av1));
    EXPECT_EQ(state.Route(video, VideoCodec::Av1, 0), PremuxRoute::Buffered);

    EncodedAudioPacket audio;
    uint32_t depth = 0;
    for (size_t i = 0; i < AudioHoldPacketLimit(1); ++i) {
        audio.bytes = {4};
        ASSERT_EQ(state.Route(audio, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
        EXPECT_EQ(depth, i + 1);
    }
    audio.bytes = {5};
    EXPECT_EQ(state.Route(audio, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(audio.bytes, (std::vector<uint8_t>{5}));
    state.Reset();
    EXPECT_TRUE(state.PendingSnapshot().audio.empty());
}

TEST(PremuxState, EightShortOpusTracksRetainThreeSecondsBeforeHeaders) {
    PremuxState state;
    uint32_t depth = 0;
    for (uint32_t frame = 0; frame < 1200; ++frame) {
        for (uint32_t track = 0; track < 8; ++track) {
            EncodedAudioPacket packet{{1}, static_cast<uint64_t>(frame) * 2500000, track};
            ASSERT_EQ(state.Route(packet, VideoCodec::Av1, 8, depth), PremuxRoute::Buffered);
        }
    }
    EXPECT_EQ(state.TakePending().audio.size(), 9600u);
}

TEST(PremuxState, AudioByteOverflowPreservesTheRejectedPacket) {
    PremuxState state;
    uint32_t depth = 0;
    EncodedAudioPacket packet;
    packet.bytes.resize(32u * 1024 * 1024 + 1, 7);
    EXPECT_EQ(state.Route(packet, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(packet.bytes.size(), 32u * 1024 * 1024 + 1);
    EXPECT_TRUE(state.PendingSnapshot().audio.empty());
}

TEST(PremuxState, AudioAdmissionCountsRetainedCapacityAndResetsAfterTransfer) {
    PremuxState state;
    uint32_t depth = 0;
    EncodedAudioPacket packet;
    packet.bytes.reserve(32u * 1024 * 1024);
    packet.bytes.push_back(7);
    ASSERT_EQ(state.Route(packet, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
    EncodedAudioPacket extra{{8}, 0, 0};
    EXPECT_EQ(state.Route(extra, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(extra.bytes, (std::vector<uint8_t>{8}));
    auto pending = state.TakePending();
    ASSERT_EQ(pending.audio.size(), 1u);
    EXPECT_EQ(pending.audio.front().bytes.capacity(), 32u * 1024 * 1024);
    EXPECT_EQ(state.Route(extra, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
    state.Reset();
    packet.bytes.reserve(32u * 1024 * 1024 + 1);
    packet.bytes.push_back(9);
    EXPECT_EQ(state.Route(packet, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(packet.bytes.front(), 9);
}

TEST(PremuxState, MediaTimeLimitRejectsLongGapsEvenWithOnlyTwoPackets) {
    PremuxState state;
    uint32_t depth = 0;
    EncodedAudioPacket first{{1}, 0, 0};
    ASSERT_EQ(state.Route(first, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
    EncodedAudioPacket late{{2}, 3000000001, 0};
    ASSERT_EQ(state.Route(late, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(late.bytes.front(), 2);
    EXPECT_EQ(state.TakePending().audio.size(), 1u);
}

TEST(PremuxState, OutOfOrderPacketsCannotHideMediaTimeOrInvalidTrack) {
    PremuxState state;
    uint32_t depth = 0;
    EncodedAudioPacket late{{1}, 9000000000, 0};
    ASSERT_EQ(state.Route(late, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
    EncodedAudioPacket earlier{{2}, 5999999999, 0};
    EXPECT_EQ(state.Route(earlier, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(earlier.bytes, (std::vector<uint8_t>{2}));
    (void)state.TakePending();
    EXPECT_EQ(state.Route(earlier, VideoCodec::Av1, 1, depth), PremuxRoute::Buffered);
    state.PublishVideo(VideoCodec::Av1, {3});
    state.PublishAudio(0, {});
    EncodedAudioPacket invalid{{4}, 6000000000, 1};
    EXPECT_EQ(state.Route(invalid, VideoCodec::Av1, 1, depth), PremuxRoute::Full);
    EXPECT_EQ(invalid.bytes, (std::vector<uint8_t>{4}));
}

TEST(SessionFailureState, ConcurrentWritersPublishOneWholeFailure) {
    for (int iteration = 0; iteration < 128; ++iteration) {
        SessionFailureState state;
        std::barrier start(3);
        std::thread audio([&] {
            start.arrive_and_wait();
            state.RecordFirst({1, ErrorPhase::AudioCapture, "audio"});
        });
        std::thread mux([&] {
            start.arrive_and_wait();
            state.RecordFirst({2, ErrorPhase::Mux, "mux"});
        });
        start.arrive_and_wait();
        audio.join();
        mux.join();
        const auto first = state.Snapshot();
        ASSERT_TRUE(first);
        if (first->error_code == 1) {
            EXPECT_EQ(first->error_phase, ErrorPhase::AudioCapture);
            EXPECT_EQ(first->error_detail, "audio");
        } else {
            EXPECT_EQ(first->error_code, 2);
            EXPECT_EQ(first->error_phase, ErrorPhase::Mux);
            EXPECT_EQ(first->error_detail, "mux");
        }
        state.RecordFirst({3, ErrorPhase::Shutdown, "late"});
        EXPECT_EQ(state.Snapshot()->error_code, first->error_code);
        state.Reset();
        EXPECT_FALSE(state.HasFailure());
    }
}

TEST(SplitRequest, ConcurrentPublicationNeverTearsSequenceAndTrigger) {
    std::atomic<uint64_t> request{0};
    std::barrier step(2);
    std::thread writer([&] {
        for (uint32_t i = 1; i <= 4096; ++i) {
            step.arrive_and_wait();
            request.store(PackSplitRequest({i, i % 4, 0}), std::memory_order_release);
            step.arrive_and_wait();
        }
    });
    for (uint32_t i = 1; i <= 4096; ++i) {
        step.arrive_and_wait();
        const auto racing = UnpackSplitRequest(request.load(std::memory_order_acquire));
        EXPECT_EQ(racing.primary_trigger, racing.sequence % 4);
        step.arrive_and_wait();
        const auto published = UnpackSplitRequest(request.load(std::memory_order_acquire));
        EXPECT_EQ(published.sequence, i);
        EXPECT_EQ(published.primary_trigger, i % 4);
    }
    writer.join();
}

TEST(SessionRequestGate, AStaleStopCannotCancelTheRequestBeingPrepared) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(2);
    requests.active_request = 22;

    EXPECT_EQ(requests.SelectForStop(mutex, state, 11), nullptr);
    EXPECT_FALSE(requests.pending_stop.Consume(11));
    EXPECT_EQ(requests.SelectForStop(mutex, state, 22), state);
}

TEST(SessionRequestGate, StopBeforeAdmissionRemainsScopedAndOneShot) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(0);

    EXPECT_EQ(requests.SelectForStop(mutex, state, 22), state);
    EXPECT_TRUE(requests.pending_stop.Consume(22));
    EXPECT_FALSE(requests.pending_stop.Consume(22));
    EXPECT_EQ(requests.SelectForStop(mutex, state, 11), state);
    EXPECT_FALSE(requests.pending_stop.Consume(22));
}

TEST(SessionRequestGate, AStopDuringRecordingDoesNotArmTheNextRecording) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(1);
    requests.active_request = 11;
    requests.recording.store(true);

    EXPECT_EQ(requests.SelectForStop(mutex, state, 11), state);
    EXPECT_FALSE(requests.pending_stop.Consume(11));
    EXPECT_EQ(requests.SelectForStop(mutex, state, kUnscopedRecordRequest), state);
    EXPECT_FALSE(requests.pending_stop.Consume(11));
    EXPECT_EQ(requests.SelectForStop(mutex, state, 22), nullptr);
    EXPECT_FALSE(requests.pending_stop.Consume(22));
}

TEST(SessionRequestGate, LeavingPreparationRetiresItsRequestBeforeTheNextPreAdmissionStop) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(1);
    requests.active_request = 11;
    {
        SessionRequestScope scope(mutex, requests);
        EXPECT_EQ(requests.SelectForStop(mutex, state, 11), state);
    }

    EXPECT_FALSE(requests.pending_stop.Consume(11));
    EXPECT_EQ(requests.SelectForStop(mutex, state, 22), state);
    EXPECT_TRUE(requests.pending_stop.Consume(22));
}

TEST(SessionRequestGate, RequestRetirementAlsoClosesSplitAdmission) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(1);
    requests.active_request = 11;
    requests.recording.store(true);
    {
        SessionRequestScope scope(mutex, requests);
    }

    EXPECT_EQ(requests.SelectForSplit(mutex, state, 11), nullptr);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, kUnscopedRecordRequest), nullptr);
}

TEST(SessionRequestGate, SplitRequiresRunningWorkersAndTheMatchingRequest) {
    SessionRequestGate requests;
    std::mutex mutex;
    auto state = std::make_shared<int>(1);
    requests.active_request = 11;

    EXPECT_EQ(requests.SelectForSplit(mutex, state, 11), nullptr);
    requests.recording.store(true);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, 22), nullptr);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, 11), state);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, kUnscopedRecordRequest), state);
    requests.recording.store(false);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, 11), nullptr);
}

// Schedule a second session at the first point where an operation releases its
// lock. This exercises the production selector's locking boundary without sleeps.
class SessionTurnoverMutex {
  public:
    void lock() {
        mutex_.lock();
    }
    void unlock() {
        mutex_.unlock();
        if (auto next = std::exchange(after_unlock, {}))
            next();
    }
    std::function<void()> after_unlock;

  private:
    std::mutex mutex_;
};

TEST(SessionRequestGate, AcceptedSplitKeepsTheStateWhoseRequestWasChecked) {
    SessionRequestGate requests;
    SessionTurnoverMutex mutex;
    const auto first = std::make_shared<int>(1);
    const auto second = std::make_shared<int>(2);
    auto state = first;
    requests.active_request = 11;
    requests.recording.store(true);
    mutex.after_unlock = [&] {
        std::lock_guard lock(mutex);
        state = second;
        requests.active_request = 22;
    };

    const auto selected = requests.SelectForSplit(mutex, state, 11);

    ASSERT_EQ(state, second);
    EXPECT_EQ(selected, first);
    EXPECT_EQ(requests.SelectForSplit(mutex, state, 11), nullptr);
}

TEST(SessionRequestGate, AcceptedStopKeepsItsStateWhenTheNextRequestIsAdmitted) {
    SessionRequestGate requests;
    SessionTurnoverMutex mutex;
    const auto first = std::make_shared<int>(1);
    const auto second = std::make_shared<int>(2);
    auto state = first;
    requests.active_request = 11;
    requests.recording.store(true);
    mutex.after_unlock = [&] {
        std::lock_guard lock(mutex);
        state = second;
        requests.active_request = 22;
        requests.recording.store(false);
    };

    const auto selected = requests.SelectForStop(mutex, state, 11);

    ASSERT_EQ(state, second);
    EXPECT_EQ(selected, first);
    EXPECT_EQ(requests.SelectForStop(mutex, state, 11), nullptr);
}

struct ReadyAudioPackets {
    uint32_t remaining = 8;
    uint32_t polls = 0;

    uint32_t PendingFrameCount() {
        ++polls;
        return remaining == 0 ? 0 : 480;
    }
};

TEST(AudioCaptureDrain, StopEndsAnAlwaysReadyDrainAfterTheCurrentPacket) {
    ReadyAudioPackets source;
    std::atomic<bool> stopped{false};
    uint32_t processed = 0;

    DrainAudioCapture(source, stopped, [&] {
        --source.remaining;
        ++processed;
        stopped.store(true);
        return true;
    });

    EXPECT_EQ(processed, 1u);
    EXPECT_EQ(source.polls, 1u);
    EXPECT_EQ(source.remaining, 7u);
}

TEST(AudioCaptureDrain, AnAlreadyStoppedDrainDoesNotPollOrAcquire) {
    ReadyAudioPackets source;
    std::atomic<bool> stopped{true};
    uint32_t processed = 0;

    DrainAudioCapture(source, stopped, [&] {
        --source.remaining;
        ++processed;
        return true;
    });

    EXPECT_EQ(processed, 0u);
    EXPECT_EQ(source.polls, 0u);
    EXPECT_EQ(source.remaining, 8u);
}

TEST(AudioCaptureDrain, AnAcquisitionFailureEndsTheDrainWithoutAnotherPoll) {
    ReadyAudioPackets source;
    std::atomic<bool> stopped{false};
    uint32_t attempted = 0;

    DrainAudioCapture(source, stopped, [&] {
        ++attempted;
        return false;
    });

    EXPECT_EQ(attempted, 1u);
    EXPECT_EQ(source.polls, 1u);
    EXPECT_EQ(source.remaining, 8u);
}

TEST(AudioCaptureDrain, ReadinessBecomingEmptyEndsAnUnstoppedDrain) {
    ReadyAudioPackets source;
    source.remaining = 2;
    std::atomic<bool> stopped{false};
    uint32_t processed = 0;

    DrainAudioCapture(source, stopped, [&] {
        --source.remaining;
        ++processed;
        return true;
    });

    EXPECT_EQ(processed, 2u);
    EXPECT_EQ(source.polls, 3u);
}

} // namespace
