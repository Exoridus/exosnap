#include "RecordingWorkspaceState.h"

#include <gtest/gtest.h>

namespace exosnap::quick {

TEST(RecordingWorkspaceTest, StartsClosedAndOpeningNarrowOverlayClosesTheOther) {
    RecordingWorkspaceState state;
    state.setWorkspaceWidth(1100);
    state.setWorkspaceHeight(500);
    EXPECT_EQ(state.sourceState(), "Closed");
    EXPECT_EQ(state.profileState(), "Closed");
    state.openSources();
    state.openProfile();
    EXPECT_EQ(state.sourceState(), "Closed");
    EXPECT_EQ(state.profileState(), "Overlay");
    EXPECT_EQ(state.workspaceWidth(), 1100);
}
TEST(RecordingWorkspaceTest, WideOverlaysRemainOpenAndEscapeClosesLatest) {
    RecordingWorkspaceState state;
    state.setWorkspaceWidth(1600);
    state.setWorkspaceHeight(500);
    state.openSources();
    state.openProfile();
    EXPECT_EQ(state.sourceState(), "Overlay");
    EXPECT_EQ(state.profileState(), "Overlay");
    EXPECT_TRUE(state.closeTopPanel());
    EXPECT_EQ(state.sourceState(), "Overlay");
    EXPECT_EQ(state.profileState(), "Closed");
    EXPECT_TRUE(state.closeTopPanel());
    EXPECT_FALSE(state.closeTopPanel());
}
TEST(RecordingWorkspaceTest, ResizeClosesLatestPinnedPanelAndNeverReopensIt) {
    RecordingWorkspaceState state;
    state.setWorkspaceWidth(1600);
    state.setWorkspaceHeight(500);
    state.pinSources();
    state.pinProfile();
    state.setWorkspaceWidth(1100);
    EXPECT_EQ(state.sourceState(), "Pinned");
    EXPECT_EQ(state.profileState(), "Closed");
    EXPECT_FALSE(state.notice().isEmpty());
    state.setWorkspaceWidth(1600);
    EXPECT_EQ(state.profileState(), "Closed");
    state.openProfile();
    EXPECT_EQ(state.profileState(), "Pinned");
}
TEST(RecordingWorkspaceTest, PinRequiresBothPreviewDimensionsAndUnpinIsExplicit) {
    RecordingWorkspaceState state;
    state.setWorkspaceWidth(860);
    state.setWorkspaceHeight(279);
    EXPECT_FALSE(state.canPinSources());
    state.pinSources();
    EXPECT_EQ(state.sourceState(), "Closed");
    state.setWorkspaceHeight(280);
    EXPECT_TRUE(state.canPinSources());
    state.pinSources();
    EXPECT_FALSE(state.canPinProfile());
    state.toggleSourcePin();
    EXPECT_EQ(state.sourceState(), "Overlay");
    state.setWorkspaceWidth(1600);
    EXPECT_EQ(state.sourceState(), "Overlay");
}
TEST(RecordingWorkspaceTest, NarrowResizeRetainsTheLatestOverlayWithoutPinning) {
    RecordingWorkspaceState state;
    state.setWorkspaceWidth(1600);
    state.setWorkspaceHeight(500);
    state.openSources();
    state.openProfile();
    state.setWorkspaceWidth(860);
    EXPECT_EQ(state.sourceState(), "Closed");
    EXPECT_EQ(state.profileState(), "Overlay");
    EXPECT_EQ(state.sourceWidth(), 320);
    EXPECT_EQ(state.profileWidth(), 360);
}

} // namespace exosnap::quick
