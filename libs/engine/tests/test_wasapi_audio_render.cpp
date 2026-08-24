#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "exosnap/engine/wasapi_audio_render.h"

namespace {

using exosnap::engine::WasapiAudioRenderer;

// Construction/destruction without Init() must be safe -- no COM object was
// ever created, Shutdown() (called from the destructor) must handle that.
TEST(WasapiAudioRenderer, ConstructDestructWithoutInitIsSafe) {
    WasapiAudioRenderer renderer;
    SUCCEED();
}

TEST(WasapiAudioRenderer, FramesPlayedStartsAtZero) {
    WasapiAudioRenderer renderer;
    EXPECT_EQ(renderer.FramesPlayed(), 0u);
}

// Without a device the play cursor is unavailable and the clock degrades to
// the written-frame fallback -- which, with nothing ever written, is 0. The
// point is that reading the clock on an un-Init()ed renderer is safe.
TEST(WasapiAudioRenderer, FramesPlayedWithoutInitIsSafe) {
    WasapiAudioRenderer renderer;
    renderer.Start();
    EXPECT_EQ(renderer.FramesPlayed(), 0u);
    renderer.Stop();
    EXPECT_EQ(renderer.FramesPlayed(), 0u);
}

TEST(WasapiAudioRenderer, PushSamplesWithoutInitIsSafeNoOp) {
    WasapiAudioRenderer renderer;
    const std::vector<float> silence(200, 0.0f); // 100 stereo frames
    renderer.PushSamples(silence.data(), 100);
    SUCCEED();
}

TEST(WasapiAudioRenderer, StopWithoutStartIsSafeNoOp) {
    WasapiAudioRenderer renderer;
    renderer.Stop();
    SUCCEED();
}

TEST(WasapiAudioRenderer, PushSamplesBlocksWhenRingIsFullAndStopWakesIt) {
    // A tiny capacity makes this deterministic without a real device: the
    // ring is a pure producer/consumer queue independent of whether a device
    // is open (Init() clears it via Shutdown() at the top of Init(), so
    // nothing pushed pre-Init can leak into playback) -- this test never
    // calls Init(), matching this file's existing no-real-device convention.
    exosnap::engine::WasapiAudioRenderer renderer(/*ring_capacity_frames=*/4);

    const std::vector<float> chunk(8, 0.0f); // 4 stereo frames == exactly the capacity
    renderer.PushSamples(chunk.data(), 4);   // fills the ring; must return immediately

    std::atomic<bool> push_returned{false};
    std::thread pusher([&] {
        renderer.PushSamples(chunk.data(), 4); // ring is full: must block
        push_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(push_returned.load()) << "PushSamples returned before Stop() -- ring capacity is not enforced";

    renderer.Stop(); // must wake the blocked push (dropping its data) without needing Init()

    for (int i = 0; i < 50 && !push_returned.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(push_returned.load()) << "Stop() did not wake a PushSamples() blocked on a full ring";
    pusher.join();
}

} // namespace
