#include <exosnap/engine/display_color_watch.h>

#include <gtest/gtest.h>

using exosnap::engine::DisplayColorWatch;

TEST(DisplayColorWatchLifetime, RepeatedWorkerShutdownIsSafe) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        DisplayColorWatch watch;
        watch.Watch(nullptr);
        watch.Stop();
        EXPECT_FALSE(watch.Notified());
    }
}
