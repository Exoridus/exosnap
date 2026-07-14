#include <gtest/gtest.h>

#include "recorder_core/edit_player_session.h"

#include <filesystem>

namespace {

using recorder_core::EditPlayerSession;

TEST(EditPlayerSession, OpenNonexistentFileFails) {
    EditPlayerSession session;
    std::string err;
    EXPECT_FALSE(session.Open(std::filesystem::path("Z:/does/not/exist.mkv"), err));
}

TEST(EditPlayerSession, ClosedSessionReportsNoAudioStream) {
    EditPlayerSession session;
    EXPECT_FALSE(session.HasAudioStream());
}

TEST(EditPlayerSession, PlayPauseSeekWithoutOpenAreSafeNoOps) {
    EditPlayerSession session;
    session.Play();
    session.Pause();
    session.SeekTo(0);
    SUCCEED();
}

TEST(EditPlayerSession, CloseWithoutOpenIsSafeNoOp) {
    EditPlayerSession session;
    session.Close();
    SUCCEED();
}

} // namespace
