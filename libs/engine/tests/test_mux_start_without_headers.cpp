// A mux thread that is stopped before any codec private data arrives must not
// claim the session's failure.
//
// The mux waits for the encoders' headers before it can open a track. When the
// session stops first -- a capture that delivered no frame, or a stop that
// arrived before capture started -- the mux is merely the first worker to
// notice something missing. It used to record a Mux-phase failure there, and
// because the session keeps the FIRST failure, that misattribution became the
// reported cause: a recording that captured nothing was reported as a mux
// problem, sending the reader to the wrong end of the pipeline.

#include <gtest/gtest.h>

#include "mux_thread.h"
#include "session_internal.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "test_unique_temp.h"

namespace {

using exosnap::engine::MuxThread;
using exosnap::engine::SessionState;
using exosnap::engine::VideoCodec;

std::shared_ptr<SessionState> MakeStateAwaitingHeaders(const std::filesystem::path& out_path) {
    auto state = std::make_shared<SessionState>();
    state->config.output_path = out_path;
    state->config.container = exosnap::engine::Container::Matroska; // H.264 is not a WebM codec
    state->config.video_codec = VideoCodec::H264;
    state->config.frame_rate_num = 60;
    state->config.frame_rate_den = 1;
    state->audio_track_count = 0;
    state->encode_width = 640;
    state->encode_height = 360;
    // Deliberately no codec_private: this is the state of a session whose
    // encoder never produced a first frame.
    return state;
}

TEST(MuxStartWithoutHeaders, AStopBeforeTheHeadersRecordsNoFailure) {
    auto state = MakeStateAwaitingHeaders(exosnap_test::UniqueTempPath("no_headers.mkv"));

    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();

    // The session stops (user stop, or a capture that never delivered) while the
    // mux is still waiting for headers.
    state->stop_requested.store(true);
    state->SignalStopEvent();
    state->premux_cv.notify_all();
    state->mux_cv.notify_all();

    ASSERT_TRUE(mux->Join(5000)) << "the mux must return on the stop instead of waiting on headers that cannot come";
    EXPECT_FALSE(state->HasFailure()) << "the mux reported a failure for a cause that is not its own";
}

TEST(MuxStartWithoutHeaders, ItWritesNoFileWhenItNeverGotHeaders) {
    const std::filesystem::path out = exosnap_test::UniqueTempPath("no_headers_file.mkv");
    auto state = MakeStateAwaitingHeaders(out);

    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();
    state->stop_requested.store(true);
    state->SignalStopEvent();
    state->premux_cv.notify_all();
    state->mux_cv.notify_all();
    ASSERT_TRUE(mux->Join(5000));

    EXPECT_FALSE(std::filesystem::exists(out));
}

// The quiet exit is scoped to the stop: a mux that CAN open its tracks must
// still report a genuine failure of its own (covered in full by
// test_mux_annexb_conversion_failure).
TEST(MuxStartWithoutHeaders, TheQuietExitDoesNotOutlastTheWait) {
    auto state = MakeStateAwaitingHeaders(exosnap_test::UniqueTempPath("no_headers_scope.mkv"));

    auto mux = std::make_shared<MuxThread>(state);
    mux->Start();
    // Without a stop the mux stays in its wait rather than exiting.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(mux->Join(0)) << "the mux left its header wait without being stopped";

    state->stop_requested.store(true);
    state->SignalStopEvent();
    state->premux_cv.notify_all();
    state->mux_cv.notify_all();
    EXPECT_TRUE(mux->Join(5000));
}

} // namespace
