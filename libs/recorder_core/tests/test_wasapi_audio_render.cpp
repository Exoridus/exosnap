#include <gtest/gtest.h>

#include <vector>

#include "recorder_core/wasapi_audio_render.h"

namespace {

using recorder_core::WasapiAudioRenderer;

// Construction/destruction without Init() must be safe -- no COM object was
// ever created, Shutdown() (called from the destructor) must handle that.
TEST(WasapiAudioRenderer, ConstructDestructWithoutInitIsSafe) {
    WasapiAudioRenderer renderer;
    SUCCEED();
}

TEST(WasapiAudioRenderer, FramesRenderedStartsAtZero) {
    WasapiAudioRenderer renderer;
    EXPECT_EQ(renderer.FramesRendered(), 0u);
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

} // namespace
